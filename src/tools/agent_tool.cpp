// AgentTool(骨架拆解批三后的工具门面):execute 的入参校验与前后台分岔、
// Hooks 转发合同、每任务装配(RunTask)。台账在 TaskLedger、调度在
// SubagentScheduler、房务在 IsolationRooms、循环与收场分型在
// agent::TurnHarness(与主回合 turn_runner 同一份)。本文件顶部的常驻注释
// (工具语义、递归治理、回调贯通)见 agent_tool.hpp。
#include "tools/agent_tool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "agent/turn_harness.hpp"
#include "cli/i18n.hpp"  // trf:墙钟/预算文案(参数校验的错误文案发给模型看,不走 i18n)
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:标题宽度(纯逻辑编辑核的零流符号)
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:inbox 投递文本的编码关口
#include "runtime/turn_runtime.hpp"    // MapPreToolDecision:PreToolUse 归并映射与主路径同一颗
#include "tools/path_utils.hpp"
#include "tools/subagent_isolation.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明/persona)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

// 子代理人格(模型可见文字):文案在 src/prompts/tools/<语言>/agent.md 的
// persona.general / persona.explore 两节,查表现取,兜底是迁移前原文。
std::string SubAgentPersona() {
    return ToolText("agent", "persona.general",
                    "你是 general-purpose 子代理,能搜索、分析并完成多步任务。专注给定任务,完成后直接给出结论,不要寒暄。");
}

std::string ExplorePersona() {
    return ToolText("agent", "persona.explore",
                    "你是 Explore 子代理,专门快速搜索、阅读并分析代码库。只读,不得改文件、启动会改动环境的命令或做别的写操作。"
                    "完成后给出简明结论和具体文件位置,不要寒暄。");
}

// 自定义 Agent 的人格段(P2-2):名字 + YAML description 拼一份跟内置两枚
// 同骨架的 persona。description 是用户写给主代理看的"何时派它出场",给子
// 代理自己也念一遍——角色边界(tools.allow)与任务姿态都在这句里。
std::string CustomAgentPersona(const agent::AgentDefinition& definition) {
    std::string persona = "你是 " + definition.name + " 子代理。";
    if (!definition.description.empty()) {
        persona += definition.description + " ";
    }
    persona += "专注给定任务,完成后直接给出结论,不要寒暄。";
    if (!definition.tools.allow.empty()) {
        persona += "你的工具面按定义收窄,撞到不在白名单内的工具属正常边界,改用白名单内工具完成。";
    }
    return persona;
}

// 预装技能段(P2-2:skills.preload):body 与名字按位对齐,缺正文的技能只
// 登记名字(正文读不到不挡派发——doctor 那边另有诊断)。空名单返回空串,
// 一个字不注入。
std::string AppendPreloadedSkills(const std::vector<std::string>& names,
                                  const std::vector<std::string>& bodies) {
    if (names.empty()) {
        return std::string();
    }
    std::string out = "\n\n[预装技能] 以下技能说明书已随任务预载,直接照做,不必再用 skill 工具加载:";
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += "\n\n--- 预装技能 " + names[i] + " ---\n";
        out += i < bodies.size() && !bodies[i].empty() ? bodies[i] : "(正文未能读取——如需细节可用 skill 工具按名加载)";
    }
    return out;
}

std::string ExtractLastText(const agent::Agent& loop) {
    const auto& history = loop.History();
    if (history.empty()) {
        return std::string();
    }
    std::string text;
    for (const auto& block : history.back().content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            text += std::get_if<api::TextBlock>(&block)->text;
        }
    }
    return text;
}

class DetachedRequestBackend : public api::Backend {
public:
    explicit DetachedRequestBackend(DetachedAgentBackend& detached) : detached_(detached) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        api::Request patched = request;
        api::ApplyRequestProfile(patched, detached_.request_profile);
        return detached_.backend->send_stream(patched, on_event, cancel);
    }

private:
    DetachedAgentBackend& detached_;
};

bool ExploreAllows(const Tool& tool) {
    const std::string name = tool.name();
    return name == "read_file" || name == "search" || name == "web_fetch" || name == "web_search" ||
           name == "lsp";
}

// title 的硬上限(显示列,不是码点数):终端窄时显示层可以再截标题字段
// 本身,但入参这里超过就拒绝,不替调用方截成另一句话。
constexpr int kMaxTitleDisplayWidth = 40;

// 每任务私有 todo(规格"同级能力审计"todo 行):转发壳原样借用源表工具,
// 唯独把 todo_write 换成这只任务独占的新实例——子代理有自己的私有 todo,
// 不乱写 main 的待办,也不与别只子代理共用一块板。源表没有 todo_write
// (Explore 只读表、旧测试直建的表)时返回空,调用方原样直用源表。
class ForwardingTool : public Tool {
public:
    explicit ForwardingTool(Tool& target) : target_(target) {}

    std::string name() const override { return target_.name(); }
    std::string description() const override { return target_.description(); }
    nlohmann::json input_schema() const override { return target_.input_schema(); }
    bool needs_confirm() const override { return target_.needs_confirm(); }
    // deferred 必须转发:私有 todo 的包装表出现在延迟挂载的会话里时,
    // 外挂工具的延迟身份不能被包装层洗掉(洗掉=未挂载也全量直挂,
    // tool_search 的账就错了)。
    bool deferred() const override { return target_.deferred(); }
    Result execute(const nlohmann::json& input) override { return target_.execute(input); }
    // 取消旗透传(子代理 x 停止失效单):包装不许洗掉 context。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override {
        return target_.execute(input, context);
    }

private:
    Tool& target_;
};

std::unique_ptr<ToolRegistry> BuildPrivateTodoRegistry(ToolRegistry& source) {
    if (source.Find("todo_write") == nullptr) {
        return nullptr;
    }
    auto out = std::make_unique<ToolRegistry>();
    for (const auto& tool : source.All()) {
        if (tool->name() == "todo_write") {
            out->Register(std::make_unique<TodoWriteTool>(std::make_shared<TodoListState>()));
        } else {
            out->Register(std::make_unique<ForwardingTool>(*tool));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 子代理流事件诊断日志(规格"二、治'无法证明'")
//
// LUBANCODE_DEBUG_SUBAGENT=1(或 =<目录>)时,每个子代理任务一只日志文件
// (<目录>/subagent-<task_id>.log,缺省 ~/.lubancode/logs/),逐流事件一行:
// 请求发出/首事件(首字节耗时可查)/每种事件型的字节数与累计/错误与超时
// 也落。只打类型与计数,不打思考与正文内容;开关解析与文件打开都走
// TraceBackend 自己,不设环境变量时零开销。
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path> SubagentDebugLogDir() {
    const auto value = lubancode::platform::GetEnvVar("LUBANCODE_DEBUG_SUBAGENT");
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    std::string mode = *value;
    for (char& c : mode) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (mode == "0" || mode == "false" || mode == "off" || mode == "no") {
        return std::nullopt;
    }
    std::filesystem::path dir;
    if (mode == "1" || mode == "true" || mode == "yes" || mode == "on") {
        const auto home = lubancode::platform::HomeDir();
        if (!home.has_value()) {
            return std::nullopt;
        }
        dir = std::filesystem::path(*home) / ".lubancode" / "logs";
    } else {
        dir = lubancode::tools::Utf8ToPath(*value);
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::nullopt;
    }
    return dir;
}

// 一行日志的毫秒级墙钟时间戳(HH:MM:SS.mmm)。
std::string LogTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm_buffer{};
#ifdef _WIN32
    localtime_s(&tm_buffer, &seconds);
#else
    localtime_r(&seconds, &tm_buffer);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", tm_buffer.tm_hour, tm_buffer.tm_min,
                  tm_buffer.tm_sec, static_cast<int>(ms));
    return buffer;
}

// 流事件 -> 诊断短名与字节数(只数长度,不看内容)。
struct EventTag {
    const char* type = "";
    std::size_t bytes = 0;
};
EventTag TagOfEvent(const api::StreamEvent& event) {
    if (std::holds_alternative<api::MessageStart>(event)) {
        return {"message_start", 0};
    }
    if (const auto* text = std::get_if<api::TextDelta>(&event)) {
        return {"delta.text", text->text.size()};
    }
    if (const auto* thinking = std::get_if<api::ThinkingDelta>(&event)) {
        return {"delta.thinking", thinking->text.size() + thinking->signature.size()};
    }
    if (std::holds_alternative<api::ToolUseStart>(event)) {
        return {"tool_use.start", 0};
    }
    if (const auto* input = std::get_if<api::ToolUseInputDelta>(&event)) {
        return {"delta.tool_input", input->partial_json.size()};
    }
    if (std::holds_alternative<api::ContentBlockDone>(event)) {
        return {"block_done", 0};
    }
    if (std::holds_alternative<api::BuiltinToolStart>(event)) {
        return {"builtin_tool.start", 0};
    }
    if (const auto* builtin_done = std::get_if<api::BuiltinToolDone>(&event)) {
        return {"builtin_tool.done", builtin_done->summary.size()};
    }
    if (std::holds_alternative<api::MessageDone>(event)) {
        return {"done", 0};
    }
    if (const auto* error = std::get_if<api::StreamError>(&event)) {
        return {"stream_error", error->message.size()};
    }
    return {"unknown", 0};
}

// 错误短文:第一行,截 200 字节(按 UTF-8 续字节截齐)。错误正文是服务端
// 诊断信息,可以落;模型思考与正文依然一个字不落。
std::string FirstLineCapped(const std::string& text, std::size_t cap = 200) {
    std::size_t end = text.find('\n');
    if (end == std::string::npos) {
        end = text.size();
    }
    end = std::min(end, cap);
    while (end > 0 && (static_cast<unsigned char>(text[end - 1]) & 0xC0) == 0x80) {
        --end;  // 不劈半个字符
    }
    return text.substr(0, end);
}

}  // namespace

// 子代理请求的包装后端:一进(请求发出)、一首个事件、逐事件、一收场,全数
// 记进活度账(台账锁下)与诊断日志(开了环境变量才有文件)。前台与后台
// 任务都从 RunTask 走这里,主会话的请求不经此包装。
class AgentTool::TraceBackend : public api::Backend {
public:
    TraceBackend(api::Backend& inner, TaskLedger& ledger, const std::shared_ptr<TaskRecord>& task)
        : inner_(inner), ledger_(ledger), task_(task) {
        if (const auto dir = SubagentDebugLogDir(); dir.has_value()) {
            const std::filesystem::path path = *dir / ("subagent-" + std::to_string(task->snapshot.id) + ".log");
            log_.open(path, std::ios::binary | std::ios::app);
        }
    }

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        const auto request_started = std::chrono::steady_clock::now();
        const int seq = ++request_seq_;
        bool saw_first_event = false;
        std::uint64_t events_total = 0;
        std::uint64_t text_bytes = 0;
        std::uint64_t thinking_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(ledger_.mutex);
            task_->activity.stage = AgentTaskActivity::Stage::WaitingFirstByte;
            task_->activity.request_started = request_started;
            task_->activity.first_byte_ms = -1;
            task_->activity.reasoning_bytes = 0;
            task_->activity.text_bytes = 0;
            ++task_->content_revision;
            ledger_.Touch();
        }
        LogLine("request seq=" + std::to_string(seq) + " task=" + std::to_string(task_->snapshot.id) +
                " agent=" + task_->snapshot.agent_type + " model=" + request.model +
                " messages=" + std::to_string(request.messages.size()));
        const auto wrapped = [&](const api::StreamEvent& event) {
            const EventTag tag = TagOfEvent(event);
            ++events_total;
            if (tag.type == std::string_view("delta.text")) {
                text_bytes += tag.bytes;
            } else if (tag.type == std::string_view("delta.thinking")) {
                thinking_bytes += tag.bytes;
            }
            if (!saw_first_event) {
                saw_first_event = true;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - request_started)
                                    .count();
                {
                    std::lock_guard<std::mutex> lock(ledger_.mutex);
                    task_->activity.first_byte_ms = static_cast<int>(ms);
                    ++task_->content_revision;
                }
                LogLine("first_event seq=" + std::to_string(seq) + " type=" + tag.type +
                        " ttfb_ms=" + std::to_string(ms));
            }
            LogLine("event seq=" + std::to_string(seq) + " type=" + tag.type +
                    " bytes=" + std::to_string(tag.bytes) + " events=" + std::to_string(events_total) +
                    " text_bytes=" + std::to_string(text_bytes) +
                    " thinking_bytes=" + std::to_string(thinking_bytes));
            on_event(event);
        };
        auto result = inner_.send_stream(request, wrapped, cancel);
        // 硬超时触发(cpr 并发挂死单):错误文案带 request_hard_timeout_secs
        // 字样的,单独补一行埋点——这面墙落锤是稀罕事,真落了多半是本机
        // 网络(代理/TUN 截胡回环)在作祟,现场要一眼认出来。
        if (!result.has_value() &&
            result.error().message.find("request_hard_timeout_secs") != std::string::npos) {
            LogLine("hard_timeout seq=" + std::to_string(seq) +
                    " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - request_started)
                                           .count()));
        }
        if (result.has_value()) {
            LogLine("stream_end seq=" + std::to_string(seq) + " ok events=" + std::to_string(events_total) +
                    " text_bytes=" + std::to_string(text_bytes) +
                    " thinking_bytes=" + std::to_string(thinking_bytes));
        } else {
            const api::Error& error = result.error();
            std::string kind;
            switch (error.kind) {
                case api::ErrorKind::Network:
                    kind = "network";
                    break;
                case api::ErrorKind::HttpStatus:
                    kind = "http_" + std::to_string(error.http_status);
                    break;
                case api::ErrorKind::Parse:
                    kind = "parse";
                    break;
                case api::ErrorKind::Api:
                    kind = "api";
                    break;
                case api::ErrorKind::Cancelled:
                    kind = "cancelled";
                    break;
            }
            LogLine("stream_end seq=" + std::to_string(seq) + " error kind=" + kind +
                    " events=" + std::to_string(events_total) + " text_bytes=" + std::to_string(text_bytes) +
                    " thinking_bytes=" + std::to_string(thinking_bytes) +
                    " message=" + FirstLineCapped(error.message));
        }
        return result;
    }

private:
    void LogLine(const std::string& body) {
        if (!log_.is_open()) {
            return;
        }
        log_ << LogTimestamp() << " " << body << "\n";
        log_.flush();  // 挂死现场要的是"杀进程也留得住"的那几行
    }

    api::Backend& inner_;
    TaskLedger& ledger_;
    std::shared_ptr<TaskRecord> task_;
    std::ofstream log_;
    int request_seq_ = 0;
};

AgentTool::AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model,
                      int default_max_steps_per_turn, std::string skills_segment)
    : backend_(backend),
      sub_registry_(sub_registry),
      cwd_(std::move(cwd)),
      model_(std::move(model)),
      default_max_steps_per_turn_(default_max_steps_per_turn),
      skills_segment_(std::move(skills_segment)) {
    agent_profile_.request.model = model_;
}

AgentTool::~AgentTool() {
    // 退出兜底(cpr 并发挂死单):先广播取消,再给每只后台线程一枚有界
    // join 窗口。旧代码是无界 join——子代理那枚请求若正卡在 cpr::Post 里
    // (挂死绝境),析构就跟着冻死。请求级硬墙钟(request_hard_timeout_secs)
    // 落进 client 之后,正常现场线程都会在墙内回来;这里仍留一道保命:join
    // 等不到的线程 detach 掉放它走。台账已是终态(或由看门狗强制收账),
    // detach 不丢账;线程闭包自持 TaskRecord 的 shared_ptr,晚归也不悬垂。
    ledger_.BroadcastCancel();
    for (auto& entry : task_threads_) {
        auto& thread = entry.thread;
        if (!thread.joinable()) {
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool settled = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            settled = !ledger_.HasRunningTasks();
            if (settled) {
                break;
            }
        }
        if (settled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 给收尾尾格一点余量
            thread.join();
        } else {
            thread.detach();  // 挂死绝境:放线程走,不冻退出
        }
    }
}

std::string AgentTool::name() const {
    return "agent";
}

std::string AgentTool::description() const {
    // 文案在 src/prompts/tools/<语言>/agent.md,兜底是迁移前的原文。
    return ToolText("agent", "description",
                    "把独立任务委托给子代理。先想一个 4~16 字(英文 2~6 个词)的语义短标题填 title——名词短语或短命令,"
                    "能彼此区分,不要照抄 prompt 首句、不要塞路径清单或套话;再把完整的任务说明写进 prompt。"
                    "title 给人看(代理面板/日志),prompt 给子代理执行,两者各司其职。agent_type=Explore 是只读代码搜索代理;"
                    "general-purpose 能研究、执行多步任务和改代码。子代理有独立上下文,只把结论交回主对话。"
                    "执行模式看 execution_mode(缺省 auto):交互会话里探索、生成、写代码、调研这类独立任务用缺省 auto "
                    "即可——后台独立跑,完成后结论自动交回,主对话还能继续干别的;不要为了拿结果习惯性写 foreground,"
                    "后台结果一样会回流,只有紧接着的下一步非等这份结果不可才显式写 foreground。管道/单发场景 auto "
                    "等价前台(阻塞等结论)。后台任务不能弹权限确认,未预先放行的操作会被拒绝。子代理看不见当前对话历史,"
                    "prompt 必须自包含。");
}

nlohmann::json AgentTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json title_prop = nlohmann::json::object();
    title_prop["type"] = "string";
    title_prop["description"] =
        ToolText("agent", "param.title",
                 "任务短标题,必填。给人看的语义字段:中文 4~16 字、英文 2~6 个词,名词短语或短命令,能与其他任务区分。"
                 "不得照抄 prompt 首句,不得含路径清单/验收全文/换行/制表符,硬上限 40 显示列。先概括 title,再写完整 prompt。");
    properties["title"] = title_prop;

    nlohmann::json prompt_prop = nlohmann::json::object();
    prompt_prop["type"] = "string";
    prompt_prop["description"] =
        ToolText("agent", "param.prompt",
                 "交给子代理的任务描述,必须自包含——子代理看不见主对话历史,任务目标、范围、期望的输出形式都要"
                 "写清楚。");
    properties["prompt"] = prompt_prop;

    // 步数预算不出 schema(规格"现场四"收尾):默认 0 = 不限步是产品判断——
    // 限步不是常态,不该摆在模型每次派工都要过一遍的参数表里。解析层仍收
    // 这两个键(见 execute 的入参双读),手写 JSON、老脚本照旧能用。

    nlohmann::json type_prop = nlohmann::json::object();
    type_prop["type"] = "string";
    type_prop["description"] =
        ToolText("agent", "param.agent_type",
                 "子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作(默认);或 /agents 清单里的自定义 "
                 "Agent 名(各自带工具边界、预装技能与预算,清单以 /agents 实时输出为准)。");
    properties["agent_type"] = type_prop;

    nlohmann::json mode_prop = nlohmann::json::object();
    mode_prop["type"] = "string";
    mode_prop["enum"] = nlohmann::json::array({"auto", "foreground", "background"});
    mode_prop["description"] =
        ToolText("agent", "param.execution_mode",
                 "执行模式,缺省 auto。auto:交互会话里默认后台独立跑(结论完成后自动交回主对话,"
                 "主对话可继续干别的)——不要习惯性写 foreground,只有下一步非等这份结果不可才显式写;"
                 "管道/单发场景 auto 等价前台(阻塞等结论)。"
                 "background:立刻返回任务编号,后台独立跑;background 任务不能弹权限确认,"
                 "未预先放行的操作会被拒绝。foreground:本次调用阻塞等子代理结论。"
                 "旧参数 run_in_background 仍认(true=background,false=foreground);"
                 "两者都给时,显式(非 auto)的 execution_mode 优先。");
    properties["execution_mode"] = mode_prop;

    nlohmann::json background_prop = nlohmann::json::object();
    background_prop["type"] = "boolean";
    background_prop["description"] =
        ToolText("agent", "param.run_in_background",
                 "(兼容旧参)是否放到会话后台运行:true 等价 execution_mode=background,"
                 "false 等价 foreground。新调用建议用 execution_mode。");
    properties["run_in_background"] = background_prop;

    nlohmann::json isolation_prop = nlohmann::json::object();
    isolation_prop["type"] = "string";
    isolation_prop["enum"] = nlohmann::json::array({"none", "worktree"});
    isolation_prop["description"] =
        ToolText("agent", "param.isolation",
                 "worktree = 给子代理单独开一间 git worktree 隔离房干活:写不碰主 checkout(文件/命令/git 三道闸拦),"
                 "干完没改动房自动删,有改动则保留并在结果里附房路径与分支,由主代理或用户收尾。"
                 "改代码的多步任务建议带上;只读摸排不必。缺省 none。");
    properties["isolation"] = isolation_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"title", "prompt"});

    return schema;
}

Tool::Result AgentTool::execute(const nlohmann::json& input) {
    // 参数错的统一出口(缺 title 无限重试拖死主循环单):文案必须写明哪个
    // 字段必填、示例长什么样,模型一遍就能补上;同一回合内同一原因连败到
    // kParamFailLimit 次,第 3 次起明拒收场——不再无限喂重试,主循环不被
    // 拖死。换一个错误原因各自重新起算;入参一旦过检(本函数尾段)或宿主
    // 新回合 SetHooks,计数清零,不记仇。拒绝文案是发给模型看的,不走
    // cli/i18n(那边只管界面上给人看的文案),跟本函数其余错误文案同一规矩。
    const auto reject = [this](const std::string& cause, const std::string& message) -> Result {
        if (param_fail_cause_ == cause) {
            ++param_fail_streak_;
        } else {
            param_fail_cause_ = cause;
            param_fail_streak_ = 1;
        }
        if (param_fail_streak_ < kParamFailLimit) {
            return {message, true};
        }
        return {"[agent 工具连败保险] 本回合内 agent 工具已第 " + std::to_string(param_fail_streak_) +
                    " 次因同一参数错误被拒(" + cause +
                    "),本次调用直接拒绝:同样的入参再重试也不会成功。出路二选一:1) 按下面的参数要求把入参"
                    "改对后再调用,参数合规的调用照常受理;2) 不再委托子代理,直接在主对话里完成任务,并向"
                    "用户如实说明未能派工的原因。\n\n" +
                    message,
                true};
    };

    // title:必填语义短标题。缺失/空白/多行/超 40 显示列一律拒绝,提示主模型
    // 补标题后重试——绝不替调用方截成另一句话,更不拿 prompt 片段冒充。
    const std::string title_missing_hint =
        "缺少必填参数 title(字符串)。title 是给人看的任务短标题:中文 4~16 字、英文 2~6 个词,名词短语或"
        "短命令;不要照抄 prompt 首句,不要塞路径清单或验收全文;不含换行/制表符,不超过 40 显示列。"
        "示例:title=\"检索构建配置\",或英文 title=\"fix login timeout\"。请补上 title 后重新调用 agent 工具。";
    const std::string title_bad_hint =
        "title 格式不合要求:须是中文 4~16 字、英文 2~6 个词的名词短语或短命令;不含换行/制表符,不超过 "
        "40 显示列;不要照抄 prompt 首句,不要塞路径清单。示例:title=\"检索构建配置\"。请换一个合规的 "
        "title 后重新调用 agent 工具。";
    if (!input.contains("title") || !input.at("title").is_string()) {
        return reject("缺少必填参数 title", title_missing_hint);
    }
    std::string title = input.at("title").get<std::string>();
    {
        const std::size_t first = title.find_first_not_of(" \t\r\n");
        const std::size_t last = title.find_last_not_of(" \t\r\n");
        title = first == std::string::npos ? std::string() : title.substr(first, last - first + 1);
    }
    if (title.empty()) {
        return reject("缺少必填参数 title", title_missing_hint);
    }
    if (title.find('\n') != std::string::npos || title.find('\r') != std::string::npos ||
        title.find('\t') != std::string::npos) {
        return reject("title 格式不合要求", title_bad_hint);
    }
    if (lubancode::cli::DisplayWidthUtf8(title) > kMaxTitleDisplayWidth) {
        return reject("title 格式不合要求", title_bad_hint);
    }

    if (!input.contains("prompt") || !input.at("prompt").is_string()) {
        return reject("缺少必填参数 prompt",
                      "缺少必填参数 prompt(字符串)。prompt 是交给子代理的完整任务说明:子代理看不见主对话"
                      "历史,任务目标、范围、期望产出都要写全、写自包含。示例:prompt=\"在 D:/repo/src 里找到"
                      "解析命令行参数的函数,报告文件路径与行号\"。请补上 prompt 后重新调用 agent 工具。");
    }
    const std::string prompt = input.at("prompt").get<std::string>();
    if (prompt.empty()) {
        return reject("prompt 为空字符串",
                      "prompt 不能是空字符串:子代理需要完整的任务说明(目标、范围、期望产出),它看不见主"
                      "对话历史。示例:prompt=\"统计 src 目录下 .cpp 文件总数并回报\"。请写明任务后重新调用 "
                      "agent 工具。");
    }

    if (const auto it = input.find("agent_type"); it != input.end() && !it->is_string()) {
        return reject("agent_type 类型不对",
                      "agent_type 得是字符串。示例:agent_type=\"Explore\"(只读调查);不确定就不传,默认 "
                      "general-purpose。");
    }
    if (const auto it = input.find("run_in_background"); it != input.end() && !it->is_boolean()) {
        return reject("run_in_background 类型不对",
                      "run_in_background 得是布尔值。示例:run_in_background=true(放后台)、false(前台阻塞);"
                      "新调用建议改用 execution_mode。");
    }
    std::string agent_type = input.value("agent_type", std::string("general-purpose"));
    if (agent_type == "explore") {
        agent_type = "Explore";
    }
    // 自定义 Agent(P2-1/P2-2):内置两枚之外的类型问解析口——从 AgentCatalog
    // 按名取定义,取到就按它派发:身份记 resolved name(Dock/台账/日志不再
    // 冒名 Explore)、tools.allow/deny 收窄、skills.preload 预装、runtime 预算
    // 落账。没配解析口(旧调用方)或名字不在清单,维持旧拒绝口径,文案指
    // 引去 /agents 看清单。
    std::optional<CustomAgentMaterial> custom;
    if (agent_type != "general-purpose" && agent_type != "Explore") {
        if (!custom_agent_resolver_) {
            return reject("agent_type 取值不合法",
                          "agent_type 只认 general-purpose 或 Explore(本入口未接自定义 Agent 目录)。示例:"
                          "agent_type=\"Explore\"(只读搜索分析);不确定就不传,默认 general-purpose。");
        }
        custom = custom_agent_resolver_(agent_type);
        if (!custom.has_value()) {
            return reject("agent_type 取值不合法",
                          "agent_type 只认 general-purpose、Explore,或 /agents 清单里可用的自定义 Agent。\""
                              + agent_type +
                              "\" 不在清单(或定义解析有错)——先看 /agents 列清单、/agent doctor " + agent_type
                              + " 查诊断;确认名字拼写后重试,或改用 general-purpose。");
        }
    }

    // execution_mode(默认 auto):auto 在交互会话等价后台、管道/单发等价前台
    // ——由 background_by_default_ 承载,首版不做自动猜测,模型自己显式覆盖。
    // 旧 run_in_background 仍认;两者都给时显式(非 auto)的优先。自定义
    // Agent 的 runtime.execution_mode 是缺省档(P2-2):入参显式压过它。
    bool mode_explicit = false;
    bool mode_background = false;
    if (const auto it = input.find("execution_mode"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return reject("execution_mode 类型不对",
                          "execution_mode 得是字符串(auto/foreground/background)。示例:execution_mode="
                          "\"foreground\";不确定就不传,默认 auto。");
        }
        const std::string mode = it->get<std::string>();
        if (mode == "foreground") {
            mode_explicit = true;
            mode_background = false;
        } else if (mode == "background") {
            mode_explicit = true;
            mode_background = true;
        } else if (mode != "auto") {
            return reject("execution_mode 取值不合法",
                          "execution_mode 只认 auto、foreground 或 background。示例:execution_mode="
                          "\"background\"(后台跑,结论完成后自动回流);不确定就不传,默认 auto。");
        }
    }
    if (!mode_explicit && !input.contains("run_in_background") && custom.has_value()) {
        const std::string& defined = custom->definition.execution_mode;
        if (defined == "foreground" || defined == "background") {
            mode_explicit = true;
            mode_background = defined == "background";
        }
    }

    std::string isolation = input.value("isolation", std::string("none"));
    if (isolation != "none" && isolation != "worktree") {
        return reject("isolation 取值不合法",
                      "isolation 只认 none 或 worktree。示例:isolation=\"worktree\"(给改代码的多步任务开"
                      "隔离房);只读摸排不传即可,默认 none。");
    }
    // 自定义 Agent 的 runtime.isolation 同是缺省档:入参显式压过它(YAML
    // 解析层已保证值只可能是 none/worktree)。
    if (!input.contains("isolation") && custom.has_value() &&
        (custom->definition.isolation == "worktree" || custom->definition.isolation == "none")) {
        isolation = custom->definition.isolation;
    }
    if (isolation == "worktree" && agent_type == "Explore") {
        return reject("Explore 用不上 worktree 隔离",
                      "Explore 是只读代理,用不上 worktree 隔离(isolation 去掉或换 general-purpose)。");
    }
    const bool isolate = isolation == "worktree";

    // 入参双读(命名规范第二批):预算类键都不出 schema(见 input_schema 的
    // 说明——模型见字段就填,索性不给),但解析层照旧收:手写 JSON、老脚本、
    // 测试都还走这条路。新名 max_steps_per_turn 优先,旧名 max_turns 兼容;
    // 两者同现取新名。没给就用 default_max_steps_per_turn_(配置来的)。
    // 成本刹车(P2-6)同批:max_time_secs(墙钟硬线,秒)、max_tokens(累计
    // token 硬线,完整输入+输出)、budget_soft_percent(软线百分比,1~100,
    // 缺省 80;0 = 只留硬闸不催办)。解析次序:入参显式 > 自定义 Agent YAML
    // 的 runtime 字段(P2-1:max_steps_per_turn 真能落到派出预算)> 配置默认。
    SubagentBudget budget;
    budget.max_steps_per_turn = default_max_steps_per_turn_;
    if (custom.has_value() && custom->definition.max_steps_per_turn.has_value()) {
        budget.max_steps_per_turn = *custom->definition.max_steps_per_turn;
    }
    const auto steps_arg = input.find("max_steps_per_turn");
    const auto turns_arg = input.find("max_turns");
    const nlohmann::json* budget_arg = nullptr;
    if (steps_arg != input.end() && !steps_arg->is_null()) {
        budget_arg = &*steps_arg;
    } else if (turns_arg != input.end() && !turns_arg->is_null()) {
        budget_arg = &*turns_arg;  // 旧名,兼容读入
    }
    if (budget_arg != nullptr) {
        const std::string budget_key = steps_arg != input.end() ? "max_steps_per_turn" : "max_turns";
        if (!budget_arg->is_number_integer()) {
            return reject(budget_key + " 类型不对",
                          budget_key + " 得是整数(0 = 不设上限)。示例:max_steps_per_turn=10。");
        }
        const int value = budget_arg->get<int>();
        if (value < 0) {
            return reject(budget_key + " 不能是负数",
                          budget_key + " 不能是负数(0 = 不设上限)。示例:max_steps_per_turn=10。");
        }
        budget.max_steps_per_turn = value;
    }
    // 时间硬线(max_time_secs,秒)。
    if (const auto it = input.find("max_time_secs"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return reject("max_time_secs 类型不对",
                          "max_time_secs 得是整数秒(0 = 不设)。示例:max_time_secs=180(任务整轮墙钟 3 分钟)。");
        }
        const int value = it->get<int>();
        if (value < 0) {
            return reject("max_time_secs 不能是负数",
                          "max_time_secs 不能是负数(0 = 不设)。示例:max_time_secs=180。");
        }
        budget.max_wall_secs = value;
    }
    // token 硬线(max_tokens,累计完整输入+输出)。
    if (const auto it = input.find("max_tokens"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return reject("max_tokens 类型不对",
                          "max_tokens 得是整数(0 = 不设)。示例:max_tokens=50000(任务累计 token 五万)。");
        }
        const std::int64_t value = it->get<std::int64_t>();
        if (value < 0) {
            return reject("max_tokens 不能是负数",
                          "max_tokens 不能是负数(0 = 不设)。示例:max_tokens=50000。");
        }
        budget.max_total_tokens = value;
    }
    // 软线百分比(budget_soft_percent)。
    if (const auto it = input.find("budget_soft_percent"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return reject("budget_soft_percent 类型不对",
                          "budget_soft_percent 得是 0~100 的整数(0 = 不催办,只留硬闸)。示例:"
                          "budget_soft_percent=80。");
        }
        const int value = it->get<int>();
        if (value < 0 || value > 100) {
            return reject("budget_soft_percent 取值不合法",
                          "budget_soft_percent 只收 0~100(0 = 不催办,只留硬闸)。示例:budget_soft_percent=80。");
        }
        budget.soft_percent = value;
    }

    // 入参过了检:连败账翻篇——改对了就不记仇,后续调用从引导文案重新起。
    param_fail_cause_.clear();
    param_fail_streak_ = 0;

    ToolRegistry& task_registry =
        agent_type == "Explore" && explore_registry_ != nullptr ? *explore_registry_ : sub_registry_;
    const bool background =
        mode_explicit ? mode_background : input.value("run_in_background", background_by_default_);
    if (background) {
        return LaunchBackground(input, title, agent_type, task_registry, budget, isolate,
                                custom.has_value() ? &*custom : nullptr);
    }
    return ExecuteForeground(input, title, agent_type, task_registry, budget, isolate,
                             custom.has_value() ? &*custom : nullptr);
}

Tool::Result AgentTool::ExecuteForeground(const nlohmann::json& input, const std::string& title,
                                          const std::string& agent_type, ToolRegistry& task_registry,
                                          const SubagentBudget& budget, bool isolate,
                                          const CustomAgentMaterial* custom) {
    // isolation=worktree:建房、锁房、工具表套 base_dir 包装、隔离范围压栈,
    // 跑完收工(干净删房,有活留房附路径)。cwd 一根指头都不动。
    std::optional<lubancode::cli::AgentWorktree> room;
    std::unique_ptr<ToolRegistry> isolated_registry;
    std::optional<ScopedIsolation> scope_guard;
    std::optional<IsolationScope> scope_storage;
    if (isolate) {
        Result setup_error;
        room = SetupIsolationRoom(cwd_, git_runner_, setup_error);
        if (!room.has_value()) {
            return setup_error;
        }
        scope_storage = IsolationScope{room->name, PathToUtf8(room->room_path), PathToUtf8(room->repo_root)};
        isolated_registry = BuildIsolatedRegistry(task_registry, *scope_storage);
        scope_guard.emplace(*scope_storage);
    }
    ToolRegistry& effective_registry = isolated_registry != nullptr ? *isolated_registry : task_registry;

    // 统一台账:前台任务同样分稳定 task id、进台账——面板列表/详情/定向
    // 介入 inbox/统计全认这一条。语义不变:execute() 仍阻塞父级工具调用等
    // 结论。delivered 置 true:结论直接交回父级,不走后台完成回流。
    AgentTaskSnapshot snapshot;
    snapshot.agent_type = agent_type;
    snapshot.title = title;
    snapshot.prompt = input.at("prompt").get<std::string>();
    snapshot.foreground = true;
    snapshot.step_limit = budget.max_steps_per_turn;  // 派出时预算进快照(规格"现场四")
    snapshot.wall_limit_secs = budget.max_wall_secs;
    snapshot.token_limit = budget.max_total_tokens;
    snapshot.state = AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    snapshot.delivered = true;
    const std::shared_ptr<TaskRecord> task = ledger_.Register(std::move(snapshot));

    const Hooks hooks = hooks_;
    Result result = RunTask(backend_, effective_registry, task->snapshot.prompt, agent_type, budget,
                            &hooks, task,
                            /*detached=*/nullptr,
                            /*prepared_system_prompt=*/nullptr,
                            scope_storage.has_value() ? &*scope_storage : nullptr,
                            /*background_hooks=*/nullptr,
                            /*background_permissions=*/nullptr,
                            custom);
    if (room.has_value()) {
        result.AppendText(FinishIsolationRoom(*room, git_runner_));
    }
    // 收尾入账:未送达的介入消息逐条列原文记进结果文本,不无声遗失;面板
    // x 停掉(task->cancel)与父轮 ESC 打断(hooks.cancel)都算取消。
    result.AppendText(TaskLedger::UndeliveredInboxNote(task));
    ledger_.FinalizeFromToolResult(
        task, result.content,
        task->cancel.load(std::memory_order_acquire) ||
            (hooks.cancel != nullptr && hooks.cancel->load(std::memory_order_acquire)));
    return result;
}

Tool::Result AgentTool::LaunchBackground(const nlohmann::json& input, const std::string& title,
                                         const std::string& agent_type, ToolRegistry& task_registry,
                                         const SubagentBudget& budget, bool isolate,
                                         const CustomAgentMaterial* custom) {
    if (!detached_backend_factory_) {
        return {"当前入口没有配置后台子代理后端,请把 run_in_background 设为 false", true};
    }
    // isolation=worktree:主线程里把房建好、锁上,建不成同步报错——后台
    // 任务没人可问,失败要立刻回给模型。房信息带进线程,收工清理。
    std::optional<lubancode::cli::AgentWorktree> room;
    if (isolate) {
        Result setup_error;
        room = SetupIsolationRoom(cwd_, git_runner_, setup_error);
        if (!room.has_value()) {
            return setup_error;
        }
    }

    // 已收尾的 std::thread 若一直不 join，系统线程句柄会跟着会话一路攒。
    // 收柄对账按**自家任务号**查，不按注册序下标——台账里混着前台任务
    // (注册了但无线程),按下标对齐会把"早终态的旧任务"误认成"这只线程
    // 已收尾",join 到正在跑的任务线程上:孵化请求被押到那只任务寿终才
    // 放行(病灶一,SERVER 时戳逮出的并发押死)。settled ⇒ 任务线程至多
    // 还差看门狗那一小步(ForceFinalize 的绝境另有请求硬墙钟兜底),join
    // 不跨网络等;join 完的条目当场抹掉,表不攒。
    for (std::size_t i = 0; i < task_threads_.size();) {
        if (ledger_.TaskSettled(task_threads_[i].task_id) && task_threads_[i].thread.joinable()) {
            task_threads_[i].thread.join();
            task_threads_.erase(task_threads_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    {
        // 全局并发槽(规格"递归派工")的同步先手检查:满了明报,不等
        // RunTask 里那笔跨前台后台的硬账。
        if (ledger_.RunningCount() >= static_cast<std::size_t>(scheduler_.max_active())) {
            return {"后台子代理已跑满 " + std::to_string(scheduler_.max_active()) +
                        " 路，请等一项收尾后再开",
                    true};
        }
    }

    DetachedAgentBackend detached;
    std::unique_ptr<ToolRegistry> detached_registry;
    try {
        detached = detached_backend_factory_();
        detached_registry = detached_registry_factory_ ? detached_registry_factory_() : nullptr;
    } catch (const std::exception& error) {
        return {"后台子代理初始化失败: " + std::string(error.what()), true};
    } catch (...) {
        return {"后台子代理初始化失败: 未知错误", true};
    }
    if (!detached.backend) {
        return {"后台子代理后端创建失败", true};
    }

    AgentTaskSnapshot snapshot;
    snapshot.agent_type = agent_type;
    snapshot.title = title;
    snapshot.prompt = input.at("prompt").get<std::string>();
    snapshot.step_limit = budget.max_steps_per_turn;  // 派出时预算进快照(规格"现场四")
    snapshot.wall_limit_secs = budget.max_wall_secs;
    snapshot.token_limit = budget.max_total_tokens;
    snapshot.state = AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    const std::shared_ptr<TaskRecord> task = ledger_.Register(std::move(snapshot));

    const int id = task->snapshot.id;
    const std::string prompt = task->snapshot.prompt;
    // 病十(批三):四段开关从皮(AgentProfile)来,子代理默认与主代理同段。
    // 自定义 Agent(P2-2)另换人格段并预装技能。
    agent::PromptOptions prompt_options;
    prompt_options.cwd = cwd_;
    prompt_options.persona = agent_type == "Explore"
                                 ? ExplorePersona()
                                 : (custom != nullptr ? CustomAgentPersona(custom->definition) : SubAgentPersona());
    prompt_options.skills_segment = agent_type == "Explore" ? std::string() : skills_segment_;
    prompt_options.prompts_dir = prompts_dir_;
    prompt_options.project_instructions = project_instructions_;
    prompt_options.mcp = agent_profile_.prompt_sections.mcp;
    prompt_options.web = agent_profile_.prompt_sections.web;
    prompt_options.lsp = agent_profile_.prompt_sections.lsp;
    prompt_options.wire = agent_profile_.prompt_sections.wire;
    std::string system_prompt = agent::AssembleSystemPrompt(prompt_options);
    if (custom != nullptr) {
        system_prompt += AppendPreloadedSkills(custom->definition.skills_preload, custom->preloaded_skills);
    }
    system_prompt += "\n\n这是后台任务。启动目录是 " + cwd_ +
                     "。调用文件与搜索工具时一律传绝对路径；不要依赖进程当前目录，它可能随主会话切换。";
    system_prompt = agent::WithModelInstructions(system_prompt, detached.model_instructions);
    system_prompt = agent::WithSoul(system_prompt, detached.soul);
    ToolRegistry* registry = detached_registry != nullptr ? detached_registry.get() : &task_registry;
    // 后台 hooks 会话:主线程里造好(拷一份只读策略快照,含信任/禁用账)再
    // 带进线程——后台线程不碰 dispatcher 账本与定义表,记录只投递,主会话
    // 安全点归并(hooks/detached.hpp 的线程规矩)。
    std::shared_ptr<lubancode::hooks::DetachedHookSession> background_hooks;
    if (hooks_.hook_dispatcher != nullptr && !hooks_.hook_dispatcher->Empty()) {
        background_hooks = std::make_shared<lubancode::hooks::DetachedHookSession>(
            hooks_.hook_dispatcher, hooks_.hook_dispatcher->context());
    }
    // 放行账快照(修"后台审批不查放行账"):也在主线程里定格——源读的是主
    // 会话的活账(按 a 落名随时会插),跨线程读没锁;这里拷成 const 份,
    // 任务线程只查不改。快照为什么定格在派出时刻、不跟主会话账活涨,见
    // BackgroundPermissionLedger 的注释。源没配 = 空账,后台照旧全拒。
    std::shared_ptr<const BackgroundPermissionLedger> background_permissions;
    if (background_permission_source_) {
        background_permissions =
            std::make_shared<BackgroundPermissionLedger>(background_permission_source_());
    }
    task_threads_.push_back(
        {id, std::thread([this, task, registry, prompt, agent_type, budget,
                                custom_copy = custom != nullptr ? std::make_optional(*custom)
                                                                : std::nullopt,
                                detached = std::move(detached),
                                system_prompt = std::move(system_prompt),
                                detached_registry = std::move(detached_registry),
                                room = std::move(room),
                                background_hooks,
                                background_permissions]() mutable {
        (void)detached_registry;  // 让独立工具表活到线程收尾
        // isolation=worktree:线程里包表、压隔离范围,收工清理。包装表按
        // 引用持源表工具,声明在源表之后,析构反序先亡,引用不悬垂。
        std::unique_ptr<ToolRegistry> isolated_registry;
        std::optional<ScopedIsolation> scope_guard;
        std::optional<IsolationScope> scope_storage;
        if (room.has_value()) {
            scope_storage = IsolationScope{room->name, PathToUtf8(room->room_path), PathToUtf8(room->repo_root)};
            isolated_registry = BuildIsolatedRegistry(*registry, *scope_storage);
            scope_guard.emplace(*scope_storage);
        }
        ToolRegistry& effective_registry =
            isolated_registry != nullptr ? *isolated_registry : *registry;
        DetachedRequestBackend backend(detached);
        Result result;
        try {
            result = RunTask(backend, effective_registry, prompt, agent_type, budget, nullptr, task,
                             &detached, &system_prompt, scope_storage.has_value() ? &*scope_storage : nullptr,
                             background_hooks, background_permissions,
                             custom_copy.has_value() ? &*custom_copy : nullptr);
        } catch (const std::exception& error) {
            result = {"子代理执行失败: " + std::string(error.what()), true};
        } catch (...) {
            result = {"子代理执行失败: 未知错误", true};
        }
        if (room.has_value()) {
            result.AppendText(FinishIsolationRoom(*room, git_runner_));
        }
        // 收尾前点一遍没送达的介入消息:任务都要结束了,排着的信没有下一个
        // 轮次边界可等——逐条列原文记进结果文本,不无声遗失。
        result.AppendText(TaskLedger::UndeliveredInboxNote(task));
        ledger_.FinalizeFromToolResult(task, result.content,
                                       task->cancel.load(std::memory_order_acquire));
    })});

    return {"后台子代理 #" + std::to_string(id) + " (" + agent_type + ") 已启动。主会话可以继续；完成结果会在后续回合送达。",
            false};
}

Tool::Result AgentTool::RunTask(api::Backend& backend, ToolRegistry& task_registry, const std::string& prompt,
                                const std::string& agent_type, const SubagentBudget& budget,
                                const Hooks* foreground_hooks,
                                const std::shared_ptr<TaskRecord>& task,
                                const DetachedAgentBackend* detached,
                                const std::string* prepared_system_prompt,
                                const IsolationScope* isolation_scope,
                                const std::shared_ptr<lubancode::hooks::DetachedHookSession>& background_hooks,
                                const std::shared_ptr<const BackgroundPermissionLedger>& background_permissions,
                                const CustomAgentMaterial* custom) {
    // 派工治理(转发 SubagentScheduler):全局并发槽先占上(前台 + 后台都
    // 算),满了明报;前台任务再记一层嵌套深度,超限明报。RAII,拒绝路径也
    // 照退。
    std::string dispatch_error;
    std::unique_ptr<SubagentScheduler::Slot> dispatch_slot =
        scheduler_.Enter(detached == nullptr, &dispatch_error);
    if (dispatch_slot == nullptr) {
        return {dispatch_error, true};
    }

    // 每任务私有 todo:todo_write 换成本任务独占实例,其余工具转发;源表
    // 没有 todo_write 时原样直用(Explore 只读表/旧测试直建的表)。
    std::unique_ptr<ToolRegistry> private_todo_registry = BuildPrivateTodoRegistry(task_registry);
    ToolRegistry& effective_registry = private_todo_registry != nullptr ? *private_todo_registry : task_registry;

    // tool_search:延迟工具索引段按"此刻的 loaded 集合"现算,拼在子代理系统
    // 提示末尾。子代理运行中途自己 tool_search 挂载了新工具,这段索引不会
    // 跟着刷新(系统提示构造后定死)——但 tools 数组每轮现拼,挂载照样生效,
    // 索引段只是稍显陈旧,无害。
    // 病十(批三):mcp/web/lsp/platforms 四段开关从皮(AgentProfile)来——
    // 子代理默认与主代理同段(裁决:补,写明补),不再是 BuildSystemPrompt
    // 薄壳的"四段不传"。
    std::string system_prompt;
    if (prepared_system_prompt != nullptr) {
        system_prompt = *prepared_system_prompt;
    } else {
        agent::PromptOptions prompt_options;
        prompt_options.cwd = cwd_;
        prompt_options.persona = agent_type == "Explore"
                                     ? ExplorePersona()
                                     : (custom != nullptr ? CustomAgentPersona(custom->definition)
                                                          : SubAgentPersona());
        prompt_options.skills_segment = agent_type == "Explore" ? std::string() : skills_segment_;
        prompt_options.prompts_dir = prompts_dir_;
        prompt_options.project_instructions = project_instructions_;
        prompt_options.mcp = agent_profile_.prompt_sections.mcp;
        prompt_options.web = agent_profile_.prompt_sections.web;
        prompt_options.lsp = agent_profile_.prompt_sections.lsp;
        prompt_options.wire = agent_profile_.prompt_sections.wire;
        system_prompt = agent::WithDeferredToolsIndex(
            agent::AssembleSystemPrompt(prompt_options),
            agent_type == "Explore" ? std::string()
                                      : (deferred_index_provider_ ? deferred_index_provider_() : std::string()));
        if (custom != nullptr) {
            system_prompt += AppendPreloadedSkills(custom->definition.skills_preload, custom->preloaded_skills);
        }
    }
    if (isolation_scope != nullptr) {
        system_prompt += "\n\n本次任务运行在隔离的 git worktree 里: " + isolation_scope->base_dir +
                         "。相对路径一律以这间房为基准(包装层会自动解析);主 checkout 只读——写入、命令"
                         "工作目录、git 改道指回主树的操作都会被拦。改动留在房内,收工自会处置。";
    }
    // 每次 execute() 都是全新的、空历史的子代理——没有跨调用的状态。
    // 长任务的今天,子代理复用主 compact(CompactHierarchical)与压力通报,
    // 在"工具结果攒完、请求未发"的安全点把旧探索压成检查点式存档。
    const std::string task_model = detached != nullptr && !detached->request_profile.model.empty()
                                       ? detached->request_profile.model
                                       : model_;
    // 运行策略与 main 同一份(规格根因一):输出上限、字符安全网、续跑
    // 次数从 runtime_profile_ 继承,步数用派出时的预算。成本刹车(P2-6):
    // 时间/token 硬线与软线百分比一并落进运行档案,AgentLoop 在步顶执法。
    // main 的 budget_soft_percent 默认 0(不催),子代理派发一律带软线。
    // model 走皮上的 request 档案(批四·病十一其一:运行档案不再另存一份)。
    agent::AgentRuntimeProfile task_profile = runtime_profile_;
    task_profile.max_steps_per_turn = budget.max_steps_per_turn;
    task_profile.max_wall_secs = budget.max_wall_secs;
    task_profile.max_total_tokens = budget.max_total_tokens;
    task_profile.budget_soft_percent = budget.soft_percent;
    if (context_window_tokens_ > 0) {
        task_profile.context_window_tokens = context_window_tokens_;
    }
    // 活度账 + 诊断日志的包装后端:子代理的每次模型请求都从这里过。必须
    // 在 sub_agent 之前声明(它引用的寿命盖过 loop);上下文压缩那一路
    //(CompactHierarchical)仍用原 backend,不混进任务的阶段账。
    std::optional<TraceBackend> traced_storage;
    if (task != nullptr) {
        traced_storage.emplace(backend, ledger_, task);
    }
    api::Backend& loop_backend = traced_storage.has_value() ? *traced_storage : backend;
    agent::AgentProfile task_agent_profile = agent_profile_;
    task_agent_profile.runtime = std::move(task_profile);
    task_agent_profile.system_prompt = system_prompt;
    if (detached != nullptr) {
        task_agent_profile.provider = detached->provider;
        task_agent_profile.request = detached->request_profile;
    }
    if (task_agent_profile.request.model.empty()) {
        task_agent_profile.request.model = task_model;
    }
    // 工具可见性(病十三的方向):谓词与拒绝文案写进皮。Explore 的只读
    // 白名单是角色限制;自定义 Agent 的 tools.allow/deny 同是角色限制
    // (P2-1:library-reviewer 只给 read_file/search,写工具确实看不见);
    // 其余角色沿用装配层灌进来的过滤(tool_search)。
    if (agent_type == "Explore") {
        task_agent_profile.tool_filter = [](const Tool& tool) { return ExploreAllows(tool); };
        // 角色限制明说(规格:错误说明写"角色限制"):模型撞到这堵墙时,
        // 文案写清限制来自只读角色,并给出角色内的替代去路。
        task_agent_profile.tool_filter_denial =
            "此工具不在 Explore 角色的只读白名单内(角色限制):请改用只读工具(read_file/search/web_fetch/"
            "web_search/lsp)完成调查;确需写入,把改动建议写进结论交回主代理执行。";
    } else if (custom != nullptr) {
        auto allow = std::make_shared<const std::set<std::string>>(custom->definition.tools.allow.begin(),
                                                                   custom->definition.tools.allow.end());
        auto deny = std::make_shared<const std::set<std::string>>(custom->definition.tools.deny.begin(),
                                                                  custom->definition.tools.deny.end());
        const std::string name = custom->definition.name;
        task_agent_profile.tool_filter = [allow, deny](const Tool& tool) {
            if (deny->count(tool.name()) != 0) {
                return false;  // deny 压过 allow(与 doctor 同一本账)
            }
            return allow->empty() || allow->count(tool.name()) != 0;
        };
        task_agent_profile.tool_filter_denial =
            "此工具不在自定义 Agent " + name + " 的工具边界内(角色限制):定义只开放 tools.allow 名单"
            "(deny 再压一层)。请改用名单内工具完成;确需边界外操作,把建议写进结论交回主代理执行。";
    } else if (detached == nullptr && tool_filter_) {
        task_agent_profile.tool_filter = tool_filter_;
    }
    agent::Agent sub_agent(loop_backend, effective_registry, std::move(task_agent_profile));
    // 子代理的项目记忆召回:按这只任务的 prompt 独立检索,同预算同安全
    // 声明;provider 没设(旧调用方)就不注入,行为不变。
    if (turn_context_provider_) {
        sub_agent.SetTurnContext(turn_context_provider_(prompt));
    }
    // 接线(批四·病十二):压力钩与收件口整份进 AgentWiring。
    agent::AgentWiring sub_wiring;
    if (context_window_tokens_ > 0) {
        sub_wiring.on_context_pressure = [this, &sub_agent, &backend, &task_model, task](
                                             const agent::ContextPressure& pressure) {
            if (pressure.phase != agent::ContextPressure::Phase::PreRequest || !pressure.projected_overflow) {
                return;  // AfterHardTrim 是纯通报;安全网丢的东西压缩救不回
            }
            agent::CompactOptions options;  // 子代理没有守恒待办,manifest 只做结构校验
            if (const auto compacted =
                    agent::CompactHierarchical(backend, task_model, sub_agent.History(), options);
                compacted.has_value()) {
                sub_agent.ReplaceHistory(agent::BuildCompactedHistory(sub_agent.History(), compacted->archive));
                if (task != nullptr) {
                    // 消息账记一枚压缩检查点:查看态里看得到"前情进存档"的
                    // 边界,不是只剩最终一句。
                    std::string archive_text;
                    for (const auto& block : compacted->archive.content) {
                        if (const auto* text_block = std::get_if<api::TextBlock>(&block)) {
                            archive_text += text_block->text;
                        }
                    }
                    std::lock_guard<std::mutex> lock(ledger_.mutex);
                    AgentTaskEvent event;
                    event.kind = AgentTaskEventKind::CompactCheckpoint;
                    event.text = std::move(archive_text);
                    ledger_.AppendEventLocked(task, std::move(event));
                }
            }
            // 压缩失败:旧历史原样不动,字符安全网(TrimHistory)仍在,不硬塞。
        };
    }

    // 定向介入收件口:这只任务自己的 inbox(前台后台同款)。AgentLoop 在
    // "工具结果攒完、下一次请求未发"的轮次边界来取,工具跑着不打断、刚产
    // 出的 tool result 不丢。每只任务的 sub_agent 只接自己这只 TaskRecord,
    // 与主会话的 peer 收件点(跨会话传话)是两码事。
    if (task != nullptr) {
        sub_wiring.inbox = [this, task]() -> std::optional<api::Message> {
            std::string text;
            TaskMessageSource source = TaskMessageSource::User;
            {
                std::lock_guard<std::mutex> inbox_lock(task->inbox_mutex);
                for (auto& item : task->inbox) {
                    if (!item.delivered) {
                        text = item.text;
                        source = item.source;
                        item.delivered = true;
                        break;
                    }
                }
            }
            if (text.empty()) {
                return std::nullopt;
            }
            // 取走一条就 Touch:面板 queued 数当即归零递减。
            ledger_.Touch();
            // 消息账:轮次边界注入的介入记 steering_message——先放
            // inbox_mutex 再拿台账锁,与 SendMessage(先台账锁后 inbox_mutex)
            // 不同时持两锁,锁序不冲。
            {
                std::lock_guard<std::mutex> tasks_lock(ledger_.mutex);
                AgentTaskEvent event;
                event.kind = AgentTaskEventKind::SteeringMessage;
                event.text = text;
                ledger_.AppendEventLocked(task, std::move(event));
            }
            api::Message message;
            message.role = api::Role::User;
            // 介入文本可能带坏串(跨会话传话/外部投递),进子代理历史前洗掉。
            message.content.push_back(api::TextBlock{FormatInboxDelivery(platform::SanitizeExternalText(text), source)});
            return message;
        };
    }
    sub_agent.SetWiring(std::move(sub_wiring));

    // 统一台账回调:进 TaskRecord 的任务(前台后台都是),工具次数/usage/
    // 实时输出全写快照;前台任务再把确认/打印/usage/pre/post 钩子原样转发
    // 给父级。后台(foreground_hooks 为空)没有可停下来问话的终端,需确认
    // 的操作一律拒绝。
    // 最近一次拒绝的原因(on_tool_confirm 与 on_tool_denial_text 同线程先后
    // 调,RunTask 栈上局部共享):last_denial_hook_reason 空 = 非"钩子 deny"
    // 的拒绝;非空 = 钩子 deny 的理由。last_denial_by_deny_prefix 真 = 拒因
    // 是 run_command 命中 deny 前缀黑名单。声明必须罩住 lambda 的整段存活期。
    std::string last_denial_hook_reason;
    bool last_denial_by_deny_prefix = false;
    agent::TurnWiring turn_wiring;
    // MCP 富结果单:工具二进制 artifact 目录随派工下发,子代理的 MCP
    // 富二进制结果与主回合落同一份会话 artifact 目录。
    if (foreground_hooks != nullptr) {
        turn_wiring.tool_artifact_dir = foreground_hooks->tool_artifact_dir;
    }
    // 逐枚追踪单:子代理内层工具的 canonical 事件转发(只读 sink 并轨)。
    if (foreground_hooks != nullptr && foreground_hooks->on_tool_trace) {
        auto parent_getter = foreground_hooks->parent_execution_id_getter;
        turn_wiring.on_tool_trace = [parent_getter, trace_hook = foreground_hooks->on_tool_trace](
                                          const agent::ToolTraceEvent& event) {
            agent::ToolTraceEvent forwarded = event;
            if (parent_getter) {
                forwarded.parent_execution_id = parent_getter();
            }
            trace_hook(forwarded);
        };
    }

    if (task != nullptr) {
        // 消息账开卷:任务说明(= 第一条 user_message)。续投输入在 Run 循环
        // 里按收到次序补记(规格"现场三")。
        {
            std::lock_guard<std::mutex> lock(ledger_.mutex);
            AgentTaskEvent event;
            event.kind = AgentTaskEventKind::UserMessage;
            event.text = prompt;
            ledger_.AppendEventLocked(task, std::move(event));
        }
    }

    // 活度账的节流拍:增量路径 1s 一拍 Touch;阶段翻页与事件边界不受
    // 节流,立即拍。content_revision 不节流,每笔增量都 +1。task 为空的旧
    // 路径(测试直调)没有 activity 账,进来直接返回。
    const auto touch_activity = [this, task](AgentTaskActivity::Stage stage) {
        if (task == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool stage_changed = task->activity.stage != stage;
        if (stage_changed) {
            task->activity.stage = stage;  // 阶段翻页:立即拍,坞行当秒换文案
            task->last_activity_touch = now;
            ledger_.Touch();
        } else if (now - task->last_activity_touch >= std::chrono::seconds(1)) {
            task->last_activity_touch = now;
            ledger_.Touch();
        }
    };

    // ---- 显示出水口(骨架拆解批二余款):子代理的从路适配器 ------------------
    // 台账 sink 先吃(正文/思考增量、工具起止、usage 从 ServerEvent 流里
    // 取,闭包原文照搬),宿主给了口子(hooks.events)的再原样转发进宿主
    // 流(payload 带 subordinate 标,画屏侧跳过——子代理只画外层卡,与
    // 批二并轨时同一画面规矩);后台任务没有宿主流,只有台账。task 与前台
    // hooks 两样都没有的旧路径连适配器都不起,显示零出水。
    runtime::IdAuthority local_event_ids;
    std::optional<runtime::TurnEventAdapter> sub_events;
    runtime::TurnEventAdapter* host_events = foreground_hooks != nullptr ? foreground_hooks->events : nullptr;
    if (task != nullptr || foreground_hooks != nullptr) {
        sub_events.emplace(host_events != nullptr ? host_events->thread_id() : std::string("subagent"),
                           host_events != nullptr ? host_events->ids() : local_event_ids);
        // 台账 sink 的工具对账:item_id -> (tool_use_id, name)。ItemCompleted
        // 的载荷不带工具名(台账事件与兜底对账都要它),从这里取。
        auto open_tools = std::make_shared<std::map<std::string, std::pair<std::string, std::string>>>();
        auto ledger_sink = [this, task, foreground_hooks, &touch_activity, open_tools](
                               const runtime::ServerEvent& event) {
            switch (event.kind) {
                case runtime::ServerEventKind::ItemDelta: {
                    if (task == nullptr) {
                        break;
                    }
                    std::lock_guard<std::mutex> lock(ledger_.mutex);
                    if (event.item_kind == runtime::ItemKind::Text) {
                        task->snapshot.live_output += event.text;
                        constexpr std::size_t kLiveOutputCap = 64 * 1024;
                        if (task->snapshot.live_output.size() > kLiveOutputCap) {
                            task->snapshot.live_output.erase(
                                0, task->snapshot.live_output.size() - kLiveOutputCap);
                        }
                        task->pending_text += event.text;  // 消息账:事件边界(工具/轮次收口)切成段
                        task->activity.text_bytes = task->pending_text.size();
                        ++task->content_revision;
                        touch_activity(AgentTaskActivity::Stage::Text);
                    } else if (event.item_kind == runtime::ItemKind::Thinking) {
                        task->pending_reasoning += event.text;  // 思考也入账,查看态与 main 同款折叠
                        task->activity.reasoning_bytes = task->pending_reasoning.size();
                        ++task->content_revision;
                        touch_activity(AgentTaskActivity::Stage::Thinking);
                    }
                    break;
                }
                case runtime::ServerEventKind::ItemStarted: {
                    if (event.item_kind != runtime::ItemKind::Tool || event.payload.value("builtin", false)) {
                        break;  // 服务端内置工具:老路子代理不落台账,照旧
                    }
                    const std::string tool_use_id = event.payload.value("tool_use_id", std::string());
                    const std::string tool_name = event.payload.value("tool_name", std::string());
                    const nlohmann::json tool_input = event.payload.contains("input") ? event.payload["input"]
                                                                                      : nlohmann::json::object();
                    (*open_tools)[event.item_id] = {tool_use_id, tool_name};
                    if (task != nullptr) {
                        std::lock_guard<std::mutex> lock(ledger_.mutex);
                        // 先把已流出的正文/思考切成事件,再记工具发起——"助手文字 ->
                        // 工具卡"的时序不许倒(规格 transcript 单测第 1 条)。
                        ledger_.FlushPendingTextLocked(task);
                        AgentTaskEvent ledger_event;
                        ledger_event.kind = AgentTaskEventKind::ToolStart;
                        ledger_event.tool_name = tool_name;
                        ledger_event.input_json = tool_input.dump();
                        ledger_.AppendEventLocked(task, std::move(ledger_event));
                        task->snapshot.tool_calls.push_back(
                            AgentTaskToolCall{tool_name, tool_input.dump(), std::string(), false, false, tool_use_id});
                        task->activity.stage = AgentTaskActivity::Stage::Tool;
                        task->activity.tool_name = tool_name;
                        task->activity.last_tool_name = tool_name;  // 收口不清:坞行"上次 <工具>"常驻(P2-1)
                        task->activity.tool_started = std::chrono::steady_clock::now();
                        ++task->content_revision;
                        ledger_.Touch();
                    }
                    if (foreground_hooks != nullptr && foreground_hooks->on_sub_tool_start) {
                        foreground_hooks->on_sub_tool_start(tool_use_id, tool_name, tool_input);
                    }
                    break;
                }
                case runtime::ServerEventKind::ItemCompleted: {
                    if (event.item_kind != runtime::ItemKind::Tool) {
                        break;
                    }
                    // 适配器 Finish 的 Cancelled 兜底:老路上没有对应的台账
                    // 回调,照旧不落。
                    if (event.outcome == runtime::Outcome::Cancelled) {
                        open_tools->erase(event.item_id);
                        break;
                    }
                    const auto it = open_tools->find(event.item_id);
                    if (it == open_tools->end()) {
                        break;  // 迟到/陌生终态:丢弃不误伤(老台账同规矩)
                    }
                    const auto [tool_use_id, tool_name] = it->second;
                    open_tools->erase(it);
                    if (task == nullptr) {
                        break;
                    }
                    const std::string result_text = event.payload.value("result", std::string());
                    const bool is_error = event.payload.value("is_error", false);
                    std::lock_guard<std::mutex> lock(ledger_.mutex);
                    ledger_.FlushPendingTextLocked(task);  // 工具结果前若有残余正文,先入账
                    // 先按 tool_use_id 精确对账;老档(没存 id 的)退回"最近一笔
                    // 未完的同名工具"——两代数据都能收口。
                    bool matched_by_id = false;
                    for (auto call_it = task->snapshot.tool_calls.rbegin();
                         call_it != task->snapshot.tool_calls.rend(); ++call_it) {
                        if (!call_it->done && !call_it->tool_use_id.empty() && call_it->tool_use_id == tool_use_id) {
                            call_it->done = true;
                            call_it->is_error = is_error;
                            call_it->result = result_text;
                            matched_by_id = true;
                            break;
                        }
                    }
                    if (!matched_by_id) {
                        for (auto call_it = task->snapshot.tool_calls.rbegin();
                             call_it != task->snapshot.tool_calls.rend(); ++call_it) {
                            if (!call_it->done && call_it->name == tool_name) {
                                call_it->done = true;
                                call_it->is_error = is_error;
                                call_it->result = result_text;
                                break;
                            }
                        }
                    }
                    AgentTaskEvent ledger_event;
                    ledger_event.kind = AgentTaskEventKind::ToolResult;
                    ledger_event.tool_name = tool_name;
                    ledger_event.result = result_text;
                    ledger_event.is_error = is_error;
                    ledger_.AppendEventLocked(task, std::move(ledger_event));
                    // 工具收口:阶段退回 None;工具名即时清,不拿旧名字接着报秒。
                    task->activity.stage = AgentTaskActivity::Stage::None;
                    task->activity.tool_name.clear();
                    ++task->content_revision;
                    ledger_.Touch();
                    break;
                }
                case runtime::ServerEventKind::UsageUpdated: {
                    api::UsageReport report;
                    report.usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
                    report.usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
                    report.usage.cache_read_tokens = event.payload.value("cache_read_tokens", std::int64_t{0});
                    report.usage.cache_creation_tokens =
                        event.payload.value("cache_creation_tokens", std::int64_t{0});
                    report.usage.output_reasoning_tokens = event.payload.value("reasoning_tokens", std::int64_t{0});
                    report.step_index = event.payload.value("step_index", 0);
                    report.request_id = event.payload.value("request_id", std::string());
                    report.model = event.payload.value("model", std::string());
                    report.cache_epoch = event.payload.value("cache_epoch", 1);
                    report.epoch_break_reason = event.payload.value("epoch_break_reason", std::string());
                    report.prefix_append_only = event.payload.value("prefix_append_only", true);
                    const bool reported = event.payload.value("reported", report.reported());
                    if (task != nullptr) {
                        std::lock_guard<std::mutex> lock(ledger_.mutex);
                        task->snapshot.input_tokens += report.usage.input_tokens;
                        task->snapshot.cache_read_tokens += report.usage.cache_read_tokens;
                        task->snapshot.cache_creation_tokens += report.usage.cache_creation_tokens;
                        task->snapshot.output_tokens += report.usage.output_tokens;
                        if (reported) {
                            task->snapshot.usage_reported = true;
                        }
                        // 步数不在这里记:usage 只是"一次请求结束"的时机,拿它
                        // 猜步数,provider 漏 usage 就会少算——直接账在 Run 循环
                        // 里按 RunOutcome::steps_used 累计。
                        ledger_.Touch();
                    }
                    if (foreground_hooks != nullptr && foreground_hooks->on_usage) {
                        foreground_hooks->on_usage(report);
                    }
                    break;
                }
                default:
                    break;  // step/批次等边界事件不进台账
            }
        };
        if (host_events != nullptr) {
            runtime::TurnEventAdapter* host = host_events;
            sub_events->Attach([ledger_sink, host](const runtime::ServerEvent& event) {
                ledger_sink(event);
                host->ForwardFromSubordinate(event);
            });
            sub_events->Start(host_events->turn_id());
        } else {
            sub_events->Attach(ledger_sink);
            sub_events->Start();
        }
        turn_wiring.events = &*sub_events;
    }

    // ---- 控制面:确认/钩子/Plan 闸(问话口原样走 TurnWiring)----------------
    if (task != nullptr) {
        if (foreground_hooks != nullptr) {
            turn_wiring.on_tool_confirm = foreground_hooks->on_tool_confirm;
        } else {
            // 后台任务没人可问:审批先查放行账快照(修"后台审批不查放行账",
            // 2026-08)——deny 命令前缀压过一切(与主路 EvaluatePermission 的
            // "策略黑名单最高"同序),always_allowed 命中(settings.local 的
            // allow_tools + 会话内按 a 落的账,派出时刻定格)或 run_command
            // 的 allow 前缀命中即放行。账不认再问 PermissionRequest 钩子,钩
            // 子也不放才拒。档位成分(auto/yolo 的免问)不带给后台:那是"有
            // 人可问"时的豁免,后台的免问只有预放行一条路。需确认的操作被
            // 拒那一刻,除了给子代理一份如实的拒绝文案,还当场推一条通知进
            // 台账——主会话空闲拍里取走,toast + transcript 事件同拍落地。
            const std::shared_ptr<lubancode::hooks::DetachedHookSession> hooks_session =
                background_hooks != nullptr && !background_hooks->Empty() ? background_hooks : nullptr;
            turn_wiring.on_tool_confirm = [this, task, hooks_session, background_permissions,
                                           &last_denial_hook_reason, &last_denial_by_deny_prefix](
                                              const std::string& /*tool_use_id*/, const std::string& name,
                                              const nlohmann::json& input) {
                last_denial_hook_reason.clear();
                last_denial_by_deny_prefix = false;
                if (background_permissions != nullptr) {
                    std::string command;
                    if (name == "run_command") {
                        if (const auto it = input.find("command"); it != input.end() && it->is_string()) {
                            command = it->get<std::string>();
                        }
                    }
                    const config::CommandPermission perm =
                        name == "run_command"
                            ? config::ClassifyCommandByPermissions(command, background_permissions->allow_commands,
                                                                   background_permissions->deny_commands)
                            : config::CommandPermission::None;
                    if (perm == config::CommandPermission::Deny) {
                        // deny 压过 allow/钩子:主路黑名单语义原样(不许借预放
                        // 行或钩子表态绕 deny 名单)。
                        last_denial_by_deny_prefix = true;
                        const int task_id = task != nullptr ? task->snapshot.id : 0;
                        ledger_.PushPermissionDenialNotice("后台 #" + std::to_string(task_id) + " 请求 " + name +
                                                           " 命中 deny 命令前缀,已拒——deny 压过预放行");
                        return false;
                    }
                    if (background_permissions->always_allowed.count(name) != 0 ||
                        perm == config::CommandPermission::Allow) {
                        return true;  // 预放行:后台免问的唯一正路
                    }
                }
                if (hooks_session != nullptr) {
                    lubancode::hooks::HookPayload payload;
                    payload.event = lubancode::hooks::HookEvent::PermissionRequest;
                    payload.fields["tool_name"] = name;
                    payload.fields["tool_input"] = input.is_null() ? nlohmann::json::object() : input;
                    payload.match_value = name;
                    const auto merged =
                        hooks_session->Emit(lubancode::hooks::HookEvent::PermissionRequest, payload);
                    if (merged.permission == lubancode::hooks::HookEventResult::Permission::Allow) {
                        return true;
                    }
                    if (merged.permission == lubancode::hooks::HookEventResult::Permission::Deny) {
                        last_denial_hook_reason =
                            merged.permission_reason.empty() ? std::string("钩子未给理由") : merged.permission_reason;
                        hooks_session->PostWarning(
                            "后台子代理 #" + hooks_session->context().agent_id.value_or(std::string("?")) +
                            " 的 PermissionRequest 钩子拒绝执行 " + name + ": " + merged.permission_reason);
                    }
                }
                const int task_id = task != nullptr ? task->snapshot.id : 0;
                std::string notice = "后台 #" + std::to_string(task_id) + " 请求 " + name + " 未放行,已拒";
                if (!last_denial_hook_reason.empty()) {
                    notice += "(PermissionRequest 钩子拒绝)";
                }
                notice += "——/permissions 预放行或让其前台重试";
                ledger_.PushPermissionDenialNotice(std::move(notice));
                return false;
            };
            // 给模型的拒绝文案:如实说"后台无法弹确认、未预放行",把出路也
            // 写上——缺省那份"用户拒绝执行该工具"会把后台边界藏起来。
            turn_wiring.on_tool_denial_text = [&last_denial_hook_reason, &last_denial_by_deny_prefix](
                                                  const std::string& /*tool_use_id*/, const std::string& name) {
                std::string text;
                if (last_denial_by_deny_prefix) {
                    // deny 命中:不是"没放行",是黑名单压过放行——首句就说清,
                    // 别让子代理拿"去 /permissions 预放行"当出路白跑一趟。
                    text = "后台任务的 " + name + " 命中 deny 命令前缀黑名单,已被拒绝——deny 压过预放行。";
                } else {
                    text = "后台任务无法弹出权限确认," + name + " 未预先放行,已被拒绝。";
                    if (!last_denial_hook_reason.empty()) {
                        text += "拒绝来自 PermissionRequest 钩子:" + last_denial_hook_reason + "。";
                    }
                }
                text += "重试同一操作不会成功:请停止重试,向用户如实报告受阻(后台未预放行,并非用户拒绝),"
                        "或改走只读产出;用户可用 /permissions 预放行后重派任务。";
                return text;
            };
        }
        if (foreground_hooks != nullptr) {
            turn_wiring.on_pre_tool_hook = foreground_hooks->on_pre_tool_hook;
            turn_wiring.on_post_tool_hook = foreground_hooks->on_post_tool_hook;
            turn_wiring.on_pre_tool_use_hook = foreground_hooks->on_pre_tool_use_hook;
            turn_wiring.on_permission_request = foreground_hooks->on_permission_request;
            turn_wiring.on_tool_phase = foreground_hooks->on_tool_phase;
            turn_wiring.on_post_tool_use_hook = foreground_hooks->on_post_tool_use_hook;
            // Plan 模式:子代理同样过 ModePolicy(Explore 拿更窄表,不因独立
            // context 逃闸;单子明令)。
            turn_wiring.on_mode_policy = foreground_hooks->on_mode_policy;
        } else if (background_hooks != nullptr && !background_hooks->Empty()) {
            // 后台 hooks:同步决策用只读策略快照真跑,不静默绕过。
            const std::shared_ptr<lubancode::hooks::DetachedHookSession> hooks_session = background_hooks;
            turn_wiring.on_pre_tool_use_hook =
                [hooks_session](const std::string& /*tool_use_id*/, const std::string& name,
                                const nlohmann::json& input) {
                    lubancode::hooks::HookPayload payload;
                    payload.event = lubancode::hooks::HookEvent::PreToolUse;
                    payload.fields["tool_name"] = name;
                    payload.fields["tool_input"] = input.is_null() ? nlohmann::json::object() : input;
                    payload.match_value = name;
                    const auto merged = hooks_session->Emit(lubancode::hooks::HookEvent::PreToolUse, payload);

                    // 归并映射归 runtime::MapPreToolDecision(与主路径同一颗
                    // 脑袋);后台特有的"ask 降级为拒"在这里叠加。
                    if (merged.permission == lubancode::hooks::HookEventResult::Permission::Ask) {
                        lubancode::runtime::ToolHookDecision decision;
                        decision.decision = lubancode::runtime::ToolHookDecision::Decision::Deny;
                        decision.reason = "后台任务没有终端可问,PreToolUse 钩子的 ask 已降级为拒绝: " +
                                          merged.permission_reason;
                        hooks_session->PostWarning("后台子代理 #" +
                                                   hooks_session->context().agent_id.value_or(std::string("?")) +
                                                   " 的 PreToolUse 钩子表态 ask,后台无终端,已按拒绝降级(" +
                                                   name + ")");
                        decision.updated_input = merged.updated_input;
                        decision.additional_context = merged.additional_context;
                        return decision;
                    }
                    return lubancode::runtime::MapPreToolDecision(merged);
                };
            turn_wiring.on_post_tool_use_hook =
                [hooks_session](const std::string& /*tool_use_id*/, const std::string& name,
                                const nlohmann::json& input, const Result& result) {
                    lubancode::hooks::HookPayload payload;
                    payload.event = lubancode::hooks::HookEvent::PostToolUse;
                    payload.fields["tool_name"] = name;
                    payload.fields["tool_input"] = input.is_null() ? nlohmann::json::object() : input;
                    payload.fields["tool_response"] = result.content;
                    payload.fields["tool_response_text"] = result.content;
                    payload.fields["tool_succeeded"] = !result.is_error;
                    payload.match_value = name;
                    return hooks_session->Emit(lubancode::hooks::HookEvent::PostToolUse, payload).additional_context;
                };
        }
    } else if (foreground_hooks != nullptr) {
        // 没进台账的旧路径(测试直调 RunTask 等边缘):控制钩子照旧转发,
        // 显示走上面的台账 sink(on_sub_tool_start/on_usage 从事件流里喂)。
        turn_wiring.on_tool_confirm = foreground_hooks->on_tool_confirm;
        turn_wiring.on_pre_tool_hook = foreground_hooks->on_pre_tool_hook;
        turn_wiring.on_post_tool_hook = foreground_hooks->on_post_tool_hook;
        turn_wiring.on_pre_tool_use_hook = foreground_hooks->on_pre_tool_use_hook;
        turn_wiring.on_permission_request = foreground_hooks->on_permission_request;
        turn_wiring.on_tool_phase = foreground_hooks->on_tool_phase;
        turn_wiring.on_post_tool_use_hook = foreground_hooks->on_post_tool_use_hook;
        turn_wiring.on_mode_policy = foreground_hooks->on_mode_policy;
    }

    // 打断信号(取消链,与主回合同一份):前台任务有三根——面板 x 置的
    // task->cancel、父轮 ESC 置的 hooks.cancel、墙钟看门狗的 wall_stop;
    // 后台任务只有 task->cancel(开了墙钟再加 wall_stop)。CancelChain 单
    // 信号直通(与旧透传一字不差),多信号起合并线程。
    agent::CancelChain cancel_chain;
    if (task != nullptr) {
        cancel_chain.Add(&task->cancel);
        if (wall_clock_timeout_secs_ > 0) {
            cancel_chain.Add(&task->wall_stop);
        }
    }
    if (foreground_hooks != nullptr && foreground_hooks->cancel != nullptr) {
        cancel_chain.Add(foreground_hooks->cancel);
    }
    const std::atomic<bool>* cancel = cancel_chain.Start();

    // 墙钟看门狗(规格三):整轮上限兜底。到点置 wall_stop(取消链收进停止
    // 信号——绝不置 task->cancel,那会被收成"用户中止");宽限期内任务线程
    // 仍没报终态(所有超时全失效、后端不理取消的绝境),直接把台账翻成
    // Failed/WallClockTimeout——任务绝不无限占着坞行。任务自带时间预算
    // (P2-6 max_time_secs)时取更紧的那根:引擎侧软停(步顶查)先到,看门狗
    // 只在引擎停不下来的绝境落锤。
    int effective_wall_secs = wall_clock_timeout_secs_;
    if (budget.max_wall_secs > 0 &&
        (effective_wall_secs <= 0 || budget.max_wall_secs < effective_wall_secs)) {
        effective_wall_secs = budget.max_wall_secs;
    }
    if (task != nullptr && effective_wall_secs > 0) {
        task->wall_stop.store(false, std::memory_order_release);
        task->wall_clock_fired.store(false, std::memory_order_release);
        task->finalized.store(false, std::memory_order_release);
        task->force_finalized = false;
        const auto deadline = task->snapshot.start_time + std::chrono::seconds(effective_wall_secs);
        const int grace_secs = wall_clock_grace_secs_;
        std::thread watchdog([this, task, deadline, grace_secs, effective_wall_secs] {
            while (!task->finalized.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (task->finalized.load(std::memory_order_acquire)) {
                return;  // 正常收尾在限内办完,无事发生
            }
            task->wall_clock_fired.store(true, std::memory_order_release);
            task->wall_stop.store(true, std::memory_order_release);
            const auto grace_end = std::chrono::steady_clock::now() + std::chrono::seconds(grace_secs);
            while (!task->finalized.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < grace_end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (task->finalized.load(std::memory_order_acquire)) {
                return;  // 停止信号起了作用,任务线程自己收的账更准
            }
            ledger_.ForceFinalizeWallClock(task, effective_wall_secs);
        });
        task->watchdog = std::move(watchdog);
    }
    // hooks 第四五步:SubagentStart + 上下文切换。前台子代理在宿主主线程
    // 里同步跑,dispatcher 上下文换成这只子代理的(agent_id/agent_type),
    // 转发过来的工具事件发的 stdin JSON 就带子代理身份;跑完还原。后台
    // 子代理走只读快照会话:线程里真跑钩子、记录只投递。
    lubancode::hooks::HookDispatcher* sub_hook_dispatcher =
        foreground_hooks != nullptr ? foreground_hooks->hook_dispatcher : nullptr;
    std::optional<lubancode::hooks::HookContext> parent_hook_context;
    if (sub_hook_dispatcher != nullptr && !sub_hook_dispatcher->Empty()) {
        parent_hook_context = sub_hook_dispatcher->context();
        lubancode::hooks::HookContext sub_context = *parent_hook_context;
        sub_context.agent_id = std::to_string(task != nullptr ? task->snapshot.id : 0);
        sub_context.agent_type = agent_type;
        // parent_agent_id:外层的 agent id(主代理触发时为 null)。
        sub_context.parent_agent_id = parent_hook_context->agent_id;

        if (sub_hook_dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStart)) {
            lubancode::hooks::HookPayload start;
            start.event = lubancode::hooks::HookEvent::SubagentStart;
            start.fields["agent_id"] = *sub_context.agent_id;
            start.fields["agent_type"] = agent_type;
            start.fields["parent_agent_id"] =
                parent_hook_context->agent_id.has_value() ? nlohmann::json(*parent_hook_context->agent_id)
                                                          : nlohmann::json();
            sub_hook_dispatcher->EmitWith(lubancode::hooks::HookEvent::SubagentStart, start, sub_context);
        }
        sub_hook_dispatcher->UpdateContext(std::move(sub_context));
    }
    // 后台子代理身份:写进快照会话自己的上下文(须在第一次 Emit 之前)。
    if (background_hooks != nullptr && !background_hooks->Empty()) {
        lubancode::hooks::HookContext sub_context = background_hooks->context();
        sub_context.agent_id = std::to_string(task != nullptr ? task->snapshot.id : 0);
        sub_context.agent_type = agent_type;
        sub_context.parent_agent_id = std::nullopt;  // 后台任务由主代理派出
        sub_context.turn_id = "bgtask_" + sub_context.agent_id.value_or(std::string("0"));
        background_hooks->context() = sub_context;
        if (background_hooks->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStart)) {
            lubancode::hooks::HookPayload start;
            start.event = lubancode::hooks::HookEvent::SubagentStart;
            start.fields["agent_id"] = *sub_context.agent_id;
            start.fields["agent_type"] = agent_type;
            start.fields["parent_agent_id"] = nlohmann::json();
            background_hooks->Emit(lubancode::hooks::HookEvent::SubagentStart, start);
        }
    }
    // 上下文还原的 RAII 兜底:正常路径在收尾后手工还原;万一中途异常穿出,
    // 也不能把子代理身份留在主会话的钩子上下文里。
    struct HookContextRestore {
        lubancode::hooks::HookDispatcher* dispatcher;
        std::optional<lubancode::hooks::HookContext> saved;
        ~HookContextRestore() {
            if (dispatcher != nullptr && saved.has_value()) {
                dispatcher->UpdateContext(std::move(*saved));
            }
        }
    } hook_context_restore{sub_hook_dispatcher, parent_hook_context};

    // ---- harness(批三:与主回合同一份)------------------------------------
    // 续投源(规格第五节"排到了却没送"):一轮 Run 正常收口后与 SendTaskMessage
    // 做原子交接——inbox 空封账进终态;有未送项拼成新一轮用户输入续跑一轮,
    // 续跑失败按批退回(RestoreDrainedInbox)。
    int settled_steps = 0;  // 已刷进台账的步数账(on_round_settled 累计)
    agent::DriveOptions drive_options;
    drive_options.cancel = cancel;
    drive_options.wall_clock_fired = [task]() {
        return task != nullptr && task->wall_clock_fired.load(std::memory_order_acquire);
    };
    drive_options.on_round_settled = [this, task, &settled_steps](const agent::RunOutcome& outcome) {
        // 直接记账:步数来自 RunOutcome(循环内按模型请求累计),不靠 usage
        // 回调猜——面板与终态摘要看到的 steps_used 同一笔账。顺带把这轮流
        // 到一半的正文/思考封进消息账(轮次边界)。
        settled_steps += outcome.steps_used;
        if (task != nullptr) {
            std::lock_guard<std::mutex> lock(ledger_.mutex);
            ledger_.FlushPendingTextLocked(task);
            task->snapshot.steps_used = settled_steps;
            ledger_.Touch();
        }
    };
    if (task != nullptr) {
        drive_options.continuation = [this, task]() -> std::optional<agent::ContinuationBatch> {
            bool sealed = false;
            DrainedInbox drained = ledger_.SealOrContinueInbox(task, sealed);
            if (sealed) {
                return std::nullopt;
            }
            std::string continuation;
            for (std::size_t i = 0; i < drained.texts.size(); ++i) {
                if (!continuation.empty()) {
                    continuation += "\n\n";
                }
                continuation += FormatInboxDelivery(drained.texts[i], drained.sources[i]);
            }
            // 消息账:介入按收到次序记 steering_message——"main/用户何时补
            // 了话"在查看态里看得见落点,不沉进黑洞(规格 transcript 单测
            // 第 3 条)。cancel 已置位的短路归 harness(领批后先查再跑)。
            {
                std::lock_guard<std::mutex> lock(ledger_.mutex);
                for (const auto& text : drained.texts) {
                    AgentTaskEvent event;
                    event.kind = AgentTaskEventKind::SteeringMessage;
                    event.text = text;
                    ledger_.AppendEventLocked(task, std::move(event));
                }
            }
            agent::ContinuationBatch batch;
            batch.input = std::move(continuation);
            batch.restore = [this, task, drained = std::move(drained)]() mutable {
                ledger_.RestoreDrainedInbox(task, drained);
            };
            return batch;
        };
    }
    // 首轮输入:任务说明(与 Agent::Run 的字符串重载同一包装——user 消息
    // + TextBlock)。
    api::Message initial_input;
    initial_input.role = api::Role::User;
    initial_input.content.push_back(api::TextBlock{prompt});
    agent::DriveReport drive = agent::DriveTurn(sub_agent, turn_wiring, std::move(initial_input), drive_options);
    // 取消链收口(合流前的次序:join 在 Stop 续跑环之前;合并旗 Stop 时
    // 置真,续跑轮拿到即收——与旧行为一致)。
    cancel_chain.Stop();

    // hooks 第四五步:SubagentStop(带 agent id/type/末条 assistant 文本;
    // continue=false = "再收口一轮":续跑理由带标识入账,stop_hook_active
    // 防咬尾,最多续一次;取消/撞预算/续跑出错就如实停)。
    const bool stop_hooks_on_foreground = sub_hook_dispatcher != nullptr && !sub_hook_dispatcher->Empty() &&
                                          sub_hook_dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStop);
    const bool stop_hooks_on_background =
        background_hooks != nullptr && !background_hooks->Empty() &&
        background_hooks->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStop);
    if ((stop_hooks_on_foreground || stop_hooks_on_background) && !drive.cancelled) {
        agent::StopOptions stop_options;
        stop_options.cancel = cancel;
        stop_options.label = "[SubagentStop 钩子续跑,非用户输入] ";
        stop_options.final_text = [&sub_agent]() { return ExtractLastText(sub_agent); };
        stop_options.on_round = [this, task](const agent::RunOutcome& continuation) {
            if (task != nullptr) {
                std::lock_guard<std::mutex> lock(ledger_.mutex);
                task->snapshot.steps_used += continuation.steps_used;
                ledger_.Touch();
            }
        };
        if (stop_hooks_on_foreground) {
            const lubancode::hooks::HookContext sub_context = sub_hook_dispatcher->context();
            stop_options.emit = [sub_hook_dispatcher, sub_context](bool stop_hook_active,
                                                                   const std::string& last_text) {
                lubancode::hooks::HookPayload stop;
                stop.event = lubancode::hooks::HookEvent::SubagentStop;
                stop.fields["agent_id"] = sub_context.agent_id.value_or(std::string());
                stop.fields["agent_type"] = sub_context.agent_type.value_or(std::string());
                stop.fields["agent_transcript_path"] = std::string();  // 子代理历史不落独立文件,如实留空
                stop.fields["last_assistant_message"] = last_text;
                stop.fields["stop_hook_active"] = stop_hook_active;
                return sub_hook_dispatcher->EmitWith(lubancode::hooks::HookEvent::SubagentStop, stop, sub_context);
            };
        } else {
            stop_options.emit = [background_hooks](bool stop_hook_active, const std::string& last_text) {
                lubancode::hooks::HookPayload stop;
                stop.event = lubancode::hooks::HookEvent::SubagentStop;
                stop.fields["agent_id"] = background_hooks->context().agent_id.value_or(std::string());
                stop.fields["agent_type"] = background_hooks->context().agent_type.value_or(std::string());
                stop.fields["agent_transcript_path"] = std::string();
                stop.fields["last_assistant_message"] = last_text;
                stop.fields["stop_hook_active"] = stop_hook_active;
                return background_hooks->Emit(lubancode::hooks::HookEvent::SubagentStop, stop);
            };
        }
        // 续跑轮的步数/预算/打断账由 RunStopContinuation 直接并进 drive
        //(harness 只并增量,主账不重算)。
        agent::RunStopContinuation(sub_agent, turn_wiring, stop_options, drive);
        if (task != nullptr) {
            std::lock_guard<std::mutex> lock(ledger_.mutex);
            task->snapshot.steps_used = drive.steps_used;
            ledger_.Touch();
        }
    }

    // ---- 收场分型(批三:harness 的 ClassifyTurnEnd,一份)----------------
    TaskOutcome task_outcome;
    task_outcome.step_limit = budget.max_steps_per_turn;
    task_outcome.steps_used = drive.steps_used;
    task_outcome.stop_reason = drive.stop_reason;
    task_outcome.wall_limit_secs = budget.max_wall_secs;
    task_outcome.token_limit = budget.max_total_tokens;
    // 输出预算账(规格根因四):撞墙上限、续跑次数、usage 是否报告、思考
    // 检查点,一并交出去。
    if (drive.output_budget.has_value()) {
        task_outcome.output_limit_tokens = drive.output_budget->limit_tokens;
        task_outcome.length_continuations_used = drive.output_budget->continuations_used;
        task_outcome.usage_reported = drive.output_budget->usage_reported;
        if (drive.output_budget->thinking_bytes > 0) {
            task_outcome.thinking_checkpoint =
                "[思考检查点] 已收 " + std::to_string(drive.output_budget->thinking_bytes) +
                " 字节思考,末段: " + FirstLineOf(drive.output_budget->thinking_tail);
        }
    }
    const std::string text = ExtractLastText(sub_agent);
    std::string snapshot_fallback;
    if (task != nullptr) {
        std::lock_guard<std::mutex> lock(ledger_.mutex);
        task_outcome.input_tokens = task->snapshot.input_tokens;
        task_outcome.cache_read_tokens = task->snapshot.cache_read_tokens;
        task_outcome.cache_creation_tokens = task->snapshot.cache_creation_tokens;
        task_outcome.output_tokens = task->snapshot.output_tokens;
        task_outcome.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - task->snapshot.start_time).count();
        if (!task->snapshot.tool_calls.empty()) {
            const AgentTaskToolCall& call = task->snapshot.tool_calls.back();
            task_outcome.last_tool =
                call.name + (call.done ? (call.is_error ? "(出错) " : " ") : "(未完成) ") +
                (call.done ? FirstLineOf(call.result) : FirstLineOf(call.input_json));
        }
        snapshot_fallback = CheckpointFallback(task->snapshot);
    }
    const std::string partial = text.empty() ? snapshot_fallback : text;

    agent::TurnEndgame endgame;
    endgame.cancelled = drive.cancelled;
    endgame.hit_step_limit = drive.hit_step_limit;
    // 墙钟信号归因(P2-6):看门狗那根线若是任务自带的时间预算掐的,算
    // TimeBudget(预算断线,带部分结果),不算看门狗兜底的 WallClockTimeout
    // ——两码事,缘由要分清。
    const bool wall_fired =
        drive.wall_clock || (task != nullptr && task->wall_clock_fired.load(std::memory_order_acquire));
    const bool wall_line_is_task_budget =
        budget.max_wall_secs > 0 && (wall_clock_timeout_secs_ <= 0 || budget.max_wall_secs <= wall_clock_timeout_secs_);
    endgame.time_budget_exhausted = drive.time_budget_exhausted || (wall_fired && wall_line_is_task_budget);
    endgame.token_budget_exhausted = drive.token_budget_exhausted;
    endgame.wall_clock = wall_fired && !wall_line_is_task_budget;
    endgame.error = drive.ok ? std::string() : drive.error;
    endgame.history_empty = sub_agent.History().empty();
    endgame.final_text = text;
    endgame.output_budget_exhausted = drive.output_budget.has_value() && drive.output_budget->exhausted;
    endgame.require_final_text = true;  // 子代理要文本结论
    const agent::TurnVerdict verdict = agent::ClassifyTurnEnd(endgame);

    Result run_result;
    switch (verdict.reason) {
        case agent::TurnVerdict::Reason::WallClockTimeout:
            // 墙钟超时(规格三):接口超时全失效的最后一道闸。失败页写明超时
            // 原因与实际用时,检查点/部分结果照常带回。
            task_outcome.status = TaskOutcomeStatus::Failed;
            task_outcome.reason = TaskOutcomeReason::WallClockTimeout;
            task_outcome.message = lubancode::cli::trf("agent_outcome.wall_clock", wall_clock_timeout_secs_);
            task_outcome.partial_result = partial;
            run_result = {"子代理执行失败: " + task_outcome.message + "\n" + ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::UserStop:
            task_outcome.status = TaskOutcomeStatus::Stopped;
            task_outcome.reason = TaskOutcomeReason::UserStop;
            task_outcome.message = "用户中止了这只子代理";
            task_outcome.partial_result = partial;
            run_result = text.empty() ? Result{ComposeOutcomeText(task_outcome), true}
                                      : Result{text + "\n" + ComposeOutcomeText(task_outcome), false};
            break;
        case agent::TurnVerdict::Reason::StepLimit:
            task_outcome.status = TaskOutcomeStatus::BudgetExhausted;
            task_outcome.reason = TaskOutcomeReason::StepLimitExhausted;
            task_outcome.message = "步数预算已用满(" + std::to_string(drive.steps_used) + "/" +
                                   std::to_string(budget.max_steps_per_turn) + " 步)";
            task_outcome.partial_result = partial;
            run_result = {ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::TimeBudget:
            // 时间成本闸(P2-6):断线缘由写明,部分结果(检查点/最后工具
            // 结果)照常带走——不静默丢,也不冒充失败。
            task_outcome.status = TaskOutcomeStatus::BudgetExhausted;
            task_outcome.reason = TaskOutcomeReason::TimeBudgetExhausted;
            task_outcome.message = "时间预算已用满(上限 " + std::to_string(budget.max_wall_secs) + " 秒,已跑 " +
                                   std::to_string(drive.steps_used) + " 步)";
            task_outcome.partial_result = partial;
            run_result = {ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::TokenBudget:
            task_outcome.status = TaskOutcomeStatus::BudgetExhausted;
            task_outcome.reason = TaskOutcomeReason::TokenBudgetExhausted;
            task_outcome.message = "token 预算已用满(上限 " + std::to_string(budget.max_total_tokens) +
                                   ",已用 " + std::to_string(task_outcome.total_input_tokens() +
                                                              task_outcome.output_tokens) +
                                   ",跑了 " + std::to_string(drive.steps_used) + " 步)";
            task_outcome.partial_result = partial;
            run_result = {ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::ApiError:
        case agent::TurnVerdict::Reason::MaxContext:
            task_outcome.status = TaskOutcomeStatus::Failed;
            task_outcome.reason = verdict.reason == agent::TurnVerdict::Reason::MaxContext
                                      ? TaskOutcomeReason::MaxContext
                                      : TaskOutcomeReason::ApiError;
            task_outcome.message = drive.error;
            task_outcome.partial_result = partial;
            run_result = {"子代理执行失败: " + drive.error + "\n" + ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::ProtocolError:
            task_outcome.status = TaskOutcomeStatus::Failed;
            task_outcome.reason = TaskOutcomeReason::ProtocolError;
            task_outcome.message = "子代理没有给出任何结论(连一次应答都没有)";
            run_result = {ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::OutputBudget:
            // 输出预算耗尽(规格根因四):独立状态,不再混进 NoFinalText。
            task_outcome.status = TaskOutcomeStatus::BudgetExhausted;
            task_outcome.reason = TaskOutcomeReason::OutputBudgetExhausted;
            task_outcome.message = "输出预算耗尽(续跑 " + std::to_string(task_outcome.length_continuations_used) +
                                   " 次后仍无正文)";
            task_outcome.partial_result = partial.empty() && !task_outcome.thinking_checkpoint.empty()
                                              ? task_outcome.thinking_checkpoint
                                              : partial;
            run_result = {ComposeOutcomeText(task_outcome) + "\n" + ComposeOutputBudgetOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::NoFinalText:
            // 最后一条 assistant 没有文本:保留 stop reason 与最后工具状态。
            task_outcome.status = TaskOutcomeStatus::Failed;
            task_outcome.reason = TaskOutcomeReason::NoFinalText;
            task_outcome.message = "最后一轮没有文本结论(stop_reason=" +
                                   (drive.stop_reason.empty() ? "(无)" : drive.stop_reason) + ")";
            task_outcome.partial_result = partial;
            run_result = {ComposeOutcomeText(task_outcome), true};
            break;
        case agent::TurnVerdict::Reason::None:
            task_outcome.status = TaskOutcomeStatus::Completed;
            task_outcome.reason = TaskOutcomeReason::None;
            run_result = {text, false};
            break;
    }
    if (task != nullptr) {
        std::lock_guard<std::mutex> lock(ledger_.mutex);
        // 看门狗已强制收账(任务线程绝境下晚归):台账保持强制收账那份,
        // 这里只补一条"晚归"事件留痕,不再翻状态/结果。
        if (task->force_finalized) {
            AgentTaskEvent late_event;
            late_event.kind = AgentTaskEventKind::Failure;
            late_event.text = lubancode::cli::tr("agent_outcome.wall_clock_late");
            ledger_.AppendEventLocked(task, std::move(late_event));
            return run_result;
        }
        // 消息账收口:残余正文先封卷,再记终局事件——completion 带最终结论
        // 全文,failure 带短因与部分结果(规格"现场三"事件表)。
        ledger_.FlushPendingTextLocked(task);
        AgentTaskEvent final_event;
        if (task_outcome.status == TaskOutcomeStatus::Completed) {
            final_event.kind = AgentTaskEventKind::Completion;
            final_event.text = text;
        } else {
            final_event.kind = AgentTaskEventKind::Failure;
            final_event.text =
                task_outcome.message + (partial.empty() ? std::string() : "\n" + partial);
        }
        ledger_.AppendEventLocked(task, std::move(final_event));
        task->snapshot.outcome = std::move(task_outcome);
        task->activity = AgentTaskActivity{};  // 终态不再带阶段文案(活度账清空)
    }
    return run_result;
}

// 同级派工的转发壳(见 agent_tool.hpp 的 AgentDispatchTool 注释)。
std::string AgentDispatchTool::name() const { return "agent"; }
std::string AgentDispatchTool::description() const { return target_.description(); }
nlohmann::json AgentDispatchTool::input_schema() const { return target_.input_schema(); }
tools::Tool::Result AgentDispatchTool::execute(const nlohmann::json& input) { return target_.execute(input); }
// 取消旗透传:壳不许洗 context(AgentTool 侧另有自己的 CancelChain,外层
// 旗照旧经 Hooks.cancel 汇进去,这里只是不让链路在壳上断)。
tools::Tool::Result AgentDispatchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    return target_.execute(input, context);
}

}  // namespace lubancode::tools
