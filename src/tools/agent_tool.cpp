// AgentTool(骨架拆解批三后的工具门面):execute 的入参校验与前后台分岔、
// Hooks 转发合同、每任务装配(RunTask)。台账在 TaskLedger、调度在
// SubagentScheduler、房务在 IsolationRooms、循环与收场分型在
// agent::TurnHarness(与主回合 turn_runner 同一份)。本文件顶部的常驻注释
// (工具语义、递归治理、回调贯通)见 agent_tool.hpp。
#include "tools/agent_tool.hpp"

#include <algorithm>
#include <cctype>
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
#include "config/command_permission.hpp"  // 后台任务命令的 permissions 前缀裁定(问题 7 拆出)
#include "platform/log_sink.hpp"  // §5.3 旧预算键的弃用日志
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:inbox 投递文本的编码关口
#include "runtime/turn_runtime.hpp"    // MapPreToolDecision:PreToolUse 归并映射与主路径同一颗
#include "tools/agent_message_tool.hpp"  // scoped agent_message(P1-1:子代理只投自己直接孩子)
#include "tools/agent_watch_tool.hpp"  // scoped agent_watch(监督器单 P1-0:子代理只看自己直接孩子)
#include "tools/instruction_scope.hpp"  // 写前作用域闸(AGENTS.md 作用域单 P0)
#include "tools/observation_filter.hpp"  // 观察边界(P2-5):子代理日志目录默认不可搜
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

std::vector<std::filesystem::path> ReferencedTodoPaths(const std::string& prompt,
                                                       const std::filesystem::path& caller_cwd) {
    std::vector<std::filesystem::path> paths;
    std::size_t search_from = 0;
    while (search_from < prompt.size()) {
        const std::size_t suffix = prompt.find(".todo", search_from);
        if (suffix == std::string::npos) {
            break;
        }
        // 从 .todo 向前只认最近一枚 todos/；这样兼容中文紧贴、反斜杠和
        // 文件名空格，也不会把绝对路径前缀或自然语言吞进任务单相对路径。
        const std::size_t todos_slash = prompt.rfind("todos/", suffix);
        const std::size_t todos_backslash = prompt.rfind("todos\\", suffix);
        const std::size_t todos = todos_slash == std::string::npos
                                      ? todos_backslash
                                      : (todos_backslash == std::string::npos ? todos_slash
                                                                              : std::max(todos_slash, todos_backslash));
        if (todos == std::string::npos) {
            search_from = suffix + 5;
            continue;
        }
        const std::string raw = prompt.substr(todos, suffix + 5 - todos);
        const std::filesystem::path resolved = caller_cwd / std::filesystem::path(raw);
        const std::filesystem::path normalized = resolved.lexically_normal();
        if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
            paths.push_back(normalized);
        }
        search_from = suffix + 5;
    }
    return paths;
}

bool HasUncheckedTodoItem(const std::string& content) {
    for (std::size_t i = 0; i + 2 < content.size(); ++i) {
        if (content[i] == '[' && (content[i + 1] == ' ' || content[i + 1] == '\t') && content[i + 2] == ']') {
            return true;
        }
    }
    return false;
}

bool HasCompletedTodoStatus(const std::string& content) {
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t status = line.find("状态：");
        if (status == std::string::npos) {
            continue;
        }
        const std::string value = line.substr(status + std::string("状态：").size());
        return value.find("已实现") != std::string::npos || value.find("已销") != std::string::npos;
    }
    return false;
}

std::optional<std::string> CompletedTodoDispatchError(const std::string& prompt,
                                                      const std::filesystem::path& caller_cwd,
                                                      lubancode::cli::GitRunner runner) {
    if (!runner) {
        runner = lubancode::cli::DefaultGitRunner;
    }
    const auto repo_root = lubancode::cli::FindRepositoryRoot(caller_cwd, runner);
    if (!repo_root.has_value()) {
        return std::nullopt;
    }
    for (const std::filesystem::path& path : ReferencedTodoPaths(prompt, caller_cwd)) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, *repo_root, ec);
        if (ec || relative.empty() || relative.is_absolute()) {
            continue;
        }
        const auto component = relative.begin();
        if (component == relative.end() || *component != "todos" || path.extension() != ".todo") {
            continue;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            continue;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string content = buffer.str();
        if (!HasCompletedTodoStatus(content) || HasUncheckedTodoItem(content)) {
            continue;
        }
        const std::string relative_utf8 = relative.generic_string();
        const lubancode::cli::GitCommandResult commit = runner(
            {*repo_root, {"log", "-1", "--format=%H", "HEAD", "--", relative_utf8}});
        std::string hash = commit.exit_code == 0 ? commit.output : std::string();
        while (!hash.empty() && std::isspace(static_cast<unsigned char>(hash.back())) != 0) {
            hash.pop_back();
        }
        if (hash.empty()) {
            hash = "当前 HEAD（完成提交未查到）";
        }
        return "[todo_already_completed] 此单已修完于 " + hash + ": " + relative_utf8 +
               "。确认要在当前基线重做？如确需重做，请先把任务单状态改回待修或新增未勾批次。";
    }
    return std::nullopt;
}

// 自定义 Agent 的人格段(P2-2):名字 + YAML description 拼一份跟内置两枚
// 同骨架的 persona。description 是用户写给主代理看的"何时派它出场",给子
// 代理自己也念一遍——角色边界(tools.allow)与任务姿态都在这句里。
// 阶段 2:定义点名了 Prompt Profile 时,这段让位——core 归 Profile 的
// 五层回路解析(契约 §6.2),身份由 Profile 的 core/10-identity.md 定。
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

}  // namespace

// 子代理的提示词拼装上下文(阶段 2:Prompt Profile 与能力推导;阶段 3 起
// 自定义 Agent 的三笔决议从 ResolvedAgentProfile 来——合并归 Resolver,
// 这里只按决议拼,不再读定义原文,拼装次序一字不动)。
// 四段开关(mcp/web/lsp/wire)仍从皮(AgentProfile)来——子代理默认与主
// 代理同段;自定义 Agent 另带两笔:
//   resolved.prompt_profile -> PromptOptions.profile + project_prompts_dir:
//     core 归 Profile 五层回路解析,生成的 persona 让位(契约 §4.2/§6.2);
//     没点名就照旧用生成 persona,行为与阶段 1 一字不差。
//   resolved.effective_tools(Resolver 过滤后的有效工具表)->
//     PromptCapabilities:feature 段只描述有效工具,web/mcp/lsp 不再吃父
//     会话的配置开关(工具都不在表里,文案不装,单子 §5.4)。
//   resolved.project_instructions = false -> AGENTS.md 那段不注(契约 §4.2)。
// 阶段 5 从匿名 namespace 提出(具名导出):Workflow 的 agent 节点与
// agent 工具派发路共用这一只,"同一 Agent 两路唤起,Prompt 完全同源"
// 的验收线钉在这里。
agent::PromptOptions BuildSubagentPromptOptions(const std::string& cwd, const std::string& agent_type,
                                                const std::string& prompts_dir,
                                                const std::string& project_prompts_dir,
                                                const std::string& project_instructions,
                                                const std::string& skills_segment,
                                                const agent::AgentProfile& agent_profile,
                                                const CustomAgentMaterial* custom,
                                                const agent::ResolvedAgentProfile* resolved,
                                                const std::vector<agent::PackageProfileRoot>& package_roots) {
    agent::PromptOptions prompt_options;
    prompt_options.cwd = cwd;
    prompt_options.persona = agent_type == "Explore"
                                 ? ExplorePersona()
                                 : (custom != nullptr ? CustomAgentPersona(custom->definition) : SubAgentPersona());
    prompt_options.skills_segment = agent_type == "Explore" ? std::string() : skills_segment;
    prompt_options.prompts_dir = prompts_dir;
    prompt_options.project_instructions = project_instructions;
    prompt_options.mcp = agent_profile.prompt_sections.mcp;
    prompt_options.web = agent_profile.prompt_sections.web;
    prompt_options.lsp = agent_profile.prompt_sections.lsp;
    prompt_options.wire = agent_profile.prompt_sections.wire;
    if (resolved != nullptr) {
        if (!resolved->prompt_profile.empty()) {
            prompt_options.profile = resolved->prompt_profile;
            prompt_options.persona.clear();  // core 归 Profile 五层回路
            prompt_options.project_prompts_dir = project_prompts_dir;
            // 阶段 3:canonical 名的 Profile("<包id>:<名>")只在包层根里解析;
            // 裸名照旧走内置/用户/项目三层,不受这份根影响。
            prompt_options.package_profile_roots = package_roots;
        }
        if (!resolved->project_instructions) {
            prompt_options.project_instructions.clear();
        }
        prompt_options.capabilities = agent::DerivePromptCapabilities(resolved->effective_tools);
    }
    return prompt_options;
}

// 预装技能段(P2-2):body 与名字按位对齐,缺正文的技能只登记名字(正文
// 读不到不挡派发——doctor 那边另有诊断)。空名单返回空串,一个字不注入。
// 阶段 5 与 BuildSubagentPromptOptions 同批导出,两路共用。
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

namespace {

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

// 完整 assistant 消息的进展指纹(监督器单 P0-0):文本/思考取内容,工具取
// 名 + 入参——不掺每轮必变的 tool_use id。"同一只工具反复读同一份内容"指
// 纹不变(空转),"读了新东西"必变。只留短哈希,不留正文。
std::string AssistantMessageFingerprint(const api::Message& message) {
    std::string material;
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            material += "t:" + text->text;
            material.push_back('\x1f');
        } else if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
            material += "r:" + thinking->text;
            material.push_back('\x1f');
        } else if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            material += "u:" + call->name + ":" + call->input.dump();
            material.push_back('\x1f');
        }
    }
    return agent::FingerprintOfParts("msg", material);
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

// 每任务私有表(P0-3 两段式):第一段在这——todo_write 换私有实例,源表的
// "agent"(旧转发壳或上层 scoped 壳)剥掉;第二段在 RunTask,算出本任务的
// 身份与孩子环境后把 scoped 壳挂回去。had_agent 是"父工具面含 agent"的
// 凭证——没有就不挂,子代理不凭空长出派工权(单子不变量 2/§11.1)。
struct TaskScopedRegistry {
    std::unique_ptr<ToolRegistry> registry;  // null = 直用源表(两样都没有)
    bool had_agent = false;
};

TaskScopedRegistry BuildTaskScopedRegistry(ToolRegistry& source) {
    const bool has_todo = source.Find("todo_write") != nullptr;
    const bool has_agent = source.Find("agent") != nullptr;
    if (!has_todo && !has_agent) {
        return TaskScopedRegistry{};
    }
    auto out = std::make_unique<ToolRegistry>();
    for (const auto& tool : source.All()) {
        const std::string name = tool->name();
        if (name == "todo_write") {
            out->Register(std::make_unique<TodoWriteTool>(std::make_shared<TodoListState>()));
        } else if (name == "agent" || name == "agent_message" || name == "agent_watch") {
            continue;  // 第二段换 scoped 壳(P1-0:agent_watch 同 agent 一并收窄)
        } else {
            out->Register(std::make_unique<ForwardingTool>(*tool));
        }
    }
    TaskScopedRegistry result;
    result.registry = std::move(out);
    result.had_agent = has_agent;
    return result;
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

// 动态 schema 的清单一行(阶段 4·单子 §6.3):换行压成空格、UTF-8 按码点
// 截到 max_chars——description 是 YAML 里的自由文本,塞进 schema 前先压平
// 截短,免得一份长描述把参数说明冲成一锅(首版只放 name+description,
// 一条一句,几十枚也撑不大 schema)。
std::string AgentTypeListingLine(const AgentTypeInfo& info, std::size_t max_chars = 160) {
    std::string flat;
    flat.reserve(info.description.size());
    for (char c : info.description) {
        flat.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < flat.size()) {
        if (count == max_chars) {
            return "- " + info.name + ": " + flat.substr(0, pos) + "…";
        }
        const auto byte = static_cast<unsigned char>(flat[pos]);
        std::size_t len = 1;
        if ((byte & 0x80U) == 0x00U) {
            len = 1;
        } else if ((byte & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((byte & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((byte & 0xF8U) == 0xF0U) {
            len = 4;
        }
        pos += len;
        ++count;
    }
    return "- " + info.name + ": " + flat;
}

// ---- 后台能力的稳定拒绝文案与 schema 修形(派工单 §二)----------------------
// 错误码 [background_unavailable] + 当前入口 + 可用模式 + 改法,四件一套;
// schema、派工前 preflight、执行口三处共用同一份话,不各说各话。
std::string BackgroundUnavailableText(bool nested) {
    std::string text = "[background_unavailable] 当前入口没有配置后台子代理后端,后台派工在任务注册前即拒绝。\n";
    text += "当前入口: " +
            std::string(nested ? "嵌套子代理树(无冻结后台工厂)" : "主入口(管道/单发)") + "\n";
    text += "可用执行模式: auto(本入口等价前台)、foreground。改法: execution_mode 设为 foreground 或不传;"
            "旧参数 run_in_background 设为 false 同效。\n";
    text += "如需真后台: 在交互会话(配置了后台子代理后端的入口)派工。";
    return text;
}

// 嵌套壳按当前入口修 schema(派工单 §二):环境没有后台工厂时把 background
// 从枚举里摘掉、说明里写明不可用——模型看得到的选项与执行口判的同一本账。
void DropBackgroundFromSchema(nlohmann::json& schema) {
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        return;
    }
    nlohmann::json& properties = schema["properties"];
    if (!properties.contains("execution_mode") || !properties["execution_mode"].is_object()) {
        return;
    }
    nlohmann::json& mode = properties["execution_mode"];
    if (mode.contains("enum") && mode["enum"].is_array()) {
        std::vector<std::string> values = mode["enum"].get<std::vector<std::string>>();
        values.erase(std::remove(values.begin(), values.end(), "background"), values.end());
        mode["enum"] = values;
    }
    if (mode.contains("description") && mode["description"].is_string()) {
        mode["description"] = mode["description"].get<std::string>() +
                              "\n本嵌套入口没有后台子代理后端:background 不可用(派工前预检以 "
                              "[background_unavailable] 稳定拒绝),auto 等价前台。";
    }
}

}  // namespace

bool AgentFaceIsReadOnly(
    const std::function<std::optional<CustomAgentMaterial>(const std::string&)>& resolver,
    const ToolRegistry& registry, const std::string& agent_type) {
    std::string lowered = agent_type;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 阶段 4:resolver 优先——Catalog 驱动,user/project 层盖掉内置名时按
    // 覆盖定义的 allow 表算,不再认死两枚内置名。小写 explore 与派发口
    // 同一条归一规矩:原名查不到再按 Explore 查一次(Catalog 里登的是
    // 带大写那枚;项目层自己造了小写 explore 则按它算)。
    std::optional<CustomAgentMaterial> material;
    if (resolver) {
        material = resolver(agent_type);
        if (!material.has_value() && agent_type == "explore") {
            material = resolver("Explore");
        }
        if (!material.has_value()) {
            return false;  // 名字解析不出(不在 Catalog/unavailable):派不出,拒
        }
        if (material->builtin) {
            // 码内内置两枚:旧答案。Explore 的只读是 explore_registry 整表
            // 只读这一运行时事实,registry 查档反而证不出来(测试表多为
            // 未知档),照旧按角色事实答。
            return lowered == "explore";
        }
    } else {
        // 没挂解析口(旧调用方/单测):退回内置两枚的旧口径。
        if (lowered == "explore") {
            return true;  // 内置只读代理:explore_registry 整表只读
        }
        if (lowered == "general-purpose") {
            return false;  // 默认子代理与 main 同工具面,含写盘与命令
        }
        return false;  // 认不得就拒
    }
    const std::vector<std::string>& allow = material->definition.tools.allow;
    if (allow.empty()) {
        return false;  // 空 allow = 不裁,继承全工具面(契约:空白名单不是只读)
    }
    for (const std::string& tool_name : allow) {
        const ToolRegistration* registration = registry.RegistrationOf(tool_name);
        if (registration == nullptr) {
            return false;  // 主表查无此名(可能是 MCP/插件工具):静态证明不了,拒
        }
        if (registration->effect_class != EffectClass::ReadOnlyLocal &&
            registration->effect_class != EffectClass::ReadOnlyRemote) {
            return false;  // 白名单里混进写盘/命令/未知档:工具面不算只读
        }
    }
    return true;
}

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
            // 日志目录进观察边界(P2-5):用户把 LUBANCODE_DEBUG_SUBAGENT 指进
            // 项目可搜目录时,子代理的 search 默认不再读回自己的日志。
            ObservationBoundary::Instance().AddExcludedDir(*dir);
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
            // 传输账(P0-0 四本时钟之一):每枚 SSE 帧都算传输活。token 流量
            // 只刷 transport_revision,绝不刷 meaningful progress——"流在动"
            // 与"任务在推进"是两码事(单子 §2.2 缺口 2)。
            ledger_.RecordTransportActivity(task_);
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
            ledger_.RecordRequestOutcome(task_, true, std::string());
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
            // 本次尝试的收场账:稳定错误码进时钟(诊断/agent_watch 用,不带
            // 错误正文)。是否重试由恢复环决定,Retrying 相位另行记账。
            ledger_.RecordRequestOutcome(task_, false, api::ReasonCodeOfError(error));
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
    // P0-3:共享派工状态立账——台账/治理/线程表/closing 归协调器,本类挂
    // 引擎回调与 main 身份的 handle。后台任务的 scoped agent 工具持
    // weak_ptr 进来,本类先亡则派工口稳定报 session_closed,不悬垂。
    coordinator_ = std::make_shared<AgentTaskCoordinator>();
    coordinator_->SetFacadeTool(this);
    coordinator_->SetEngine([this](const AgentDispatchRequest& request) {
        // 连败账随调用方的 handle 走(main 那枚常驻这里;每只任务那枚随
        // 私有表,任务内单回合、单线程)。
        return ExecuteDispatch(request, request.fail_account != nullptr ? *request.fail_account : main_handle_);
    });
    AgentRunIdentity main_identity;  // task_id=0/depth=0 = main
    main_handle_ = AgentDispatchHandle(coordinator_, main_identity, nullptr);
}

AgentTool::~AgentTool() {
    // 退出兜底(cpr 并发挂死单):先拒新派工、广播取消,再给每只后台线程
    // 一枚有界 join 窗口(规矩原样迁进协调器,见 JoinAllBounded 注释)。
    coordinator_->RequestClose();
    coordinator_->JoinAllBounded();
}

bool AgentTool::BackgroundBackendAvailable(const std::shared_ptr<const SubagentDispatchEnv>& env) const {
    if (env != nullptr) {
        // 嵌套:无 UI 的树只认冻结工厂;有 UI 的嵌套(前台任务的孩子)还能
        // 借会话工厂——与 LaunchBackground 的材料分路同一张表。
        return env->backend_factory != nullptr || (!env->headless && detached_backend_factory_ != nullptr);
    }
    return detached_backend_factory_ != nullptr;
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
                 "交给子代理的完整任务说明。LubanCode 每只子代理进场时都没有父会话上下文。把它当作刚走进"
                 "屋子的聪明同事来交代:说清想办成什么、为何要办,已经查明或排除了什么,并给出必要的文件、"
                 "行号、错误、命令、边界与报告要求。绝不要把理解也甩给子代理,别只写‘根据发现修复问题’。"
                 "推荐按背景、任务、报告排布,普通自包含任务句也合法。宿主不解析 Markdown,不补造栏目,不重写"
                 "正文;只校验字符串、非空、NUL 与 32 KiB 总帽。");
    properties["prompt"] = prompt_prop;

    // 步数预算不出 schema(规格"现场四"收尾):默认 0 = 不限步是产品判断——
    // 限步不是常态,不该摆在模型每次派工都要过一遍的参数表里。解析层仍收
    // 这两个键(见 execute 的入参双读),手写 JSON、老脚本照旧能用。

    nlohmann::json type_prop = nlohmann::json::object();
    type_prop["type"] = "string";
    // 阶段 4·动态 schema:说明先走静态文案(查表,与从前逐字节一致),再按
    // 类型清单缓存追加"当前可派的类型"。清单从 AgentCatalog 拉(内置+自
    // 定义,各带一句 description),回合边界翻新、平时读缓存——input_schema
    // 每次构造请求都跑,不许拖磁盘扫描(性能口径见 SetAgentTypesProvider)。
    // 没配清单源(单测/旧调用方)= 一个字不追加,schema 与从前零 diff。
    std::string type_description = ToolText(
        "agent", "param.agent_type",
        "子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作(默认);或 /agents 清单里的自定义 "
        "Agent 名(各自带工具边界、预装技能与预算,清单以 /agents 实时输出为准)。");
    const std::vector<AgentTypeInfo> agent_types = CachedAgentTypes();
    if (!agent_types.empty()) {
        type_description += "\n当前可派的类型(按 description 挑人;清单以 /agents 实时输出为准):";
        for (const AgentTypeInfo& info : agent_types) {
            type_description += "\n" + AgentTypeListingLine(info);
        }
    }
    type_prop["description"] = std::move(type_description);
    properties["agent_type"] = type_prop;

    // 执行模式按当前入口生成(派工单 §2.4):本入口没有后台子代理后端时,
    // background 不进枚举、说明写明不可用与改法——能力快照不再把没路的
    // 模式摆出来,调用方不必撞了运行时才知道。
    const bool background_available = detached_backend_factory_ != nullptr;
    nlohmann::json mode_prop = nlohmann::json::object();
    mode_prop["type"] = "string";
    if (background_available) {
        mode_prop["enum"] = nlohmann::json::array({"auto", "foreground", "background"});
    } else {
        mode_prop["enum"] = nlohmann::json::array({"auto", "foreground"});
    }
    std::string mode_description =
        ToolText("agent", "param.execution_mode",
                 "执行模式,缺省 auto。auto:交互会话里默认后台独立跑(结论完成后自动交回主对话,"
                 "主对话可继续干别的)——不要习惯性写 foreground,只有下一步非等这份结果不可才显式写;"
                 "管道/单发场景 auto 等价前台(阻塞等结论)。"
                 "background:立刻返回任务编号,后台独立跑;background 任务不能弹权限确认,"
                 "未预先放行的操作会被拒绝。foreground:本次调用阻塞等子代理结论。"
                 "旧参数 run_in_background 仍认(true=background,false=foreground);"
                 "两者都给时,显式(非 auto)的 execution_mode 优先。");
    if (!background_available) {
        mode_description +=
            "\n本入口未配置后台子代理后端:background 不可用(派工前预检以 [background_unavailable] "
            "稳定拒绝),auto 等价前台;请用 foreground 或不传。";
    }
    mode_prop["description"] = std::move(mode_description);
    properties["execution_mode"] = mode_prop;

    nlohmann::json background_prop = nlohmann::json::object();
    background_prop["type"] = "boolean";
    std::string background_description =
        ToolText("agent", "param.run_in_background",
                 "(兼容旧参)是否放到会话后台运行:true 等价 execution_mode=background,"
                 "false 等价 foreground。新调用建议用 execution_mode。");
    if (!background_available) {
        background_description +=
            "\n本入口没有后台后端:传 true 会被 [background_unavailable] 稳定拒绝,请用 false。";
    }
    background_prop["description"] = std::move(background_description);
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

// 类型清单缓存(阶段 4):首次使用经 provider 现拉一份快照,之后只读;
// SetHooks(回合边界)与 SetAgentTypesProvider 翻新。前台主线程与后台任务
// 的派工壳都会到这(input_schema 每请求构造),锁短持有——provider 在锁内
// 跑,生产装配里是一次 Catalog 扫描,一回合至多一遍。
std::vector<AgentTypeInfo> AgentTool::CachedAgentTypes() const {
    if (!agent_types_provider_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(agent_types_cache_.mutex);
    if (!agent_types_cache_.loaded) {
        agent_types_cache_.types = agent_types_provider_();
        agent_types_cache_.loaded = true;
    }
    return agent_types_cache_.types;
}

Tool::Result AgentTool::execute(const nlohmann::json& input) {
    // P0-3:execute 只是 main 那枚 handle 的入口——真规则在协调器与
    // ExecuteDispatch。参数连败账随 main_handle_(回合边界 SetHooks 清零)。
    return main_handle_.Dispatch(input);
}

Tool::Result AgentTool::ExecuteDispatch(const AgentDispatchRequest& dispatch, AgentDispatchHandle& fail_account) {
    const nlohmann::json& input = dispatch.input;
    const std::shared_ptr<const SubagentDispatchEnv>& env = dispatch.env;
    const AgentRunIdentity& caller = dispatch.caller;
    const bool headless = env != nullptr && env->headless;

    // 参数错的统一出口(缺 title 无限重试拖死主循环单):文案必须写明哪个
    // 字段必填、示例长什么样,模型一遍就能补上;同一回合内同一原因连败到
    // kParamFailLimit 次,第 3 次起明拒收场——不再无限喂重试,主循环不被
    // 拖死。换一个错误原因各自重新起算;入参一旦过检(本函数尾段)或宿主
    // 新回合 SetHooks,计数清零,不记仇。拒绝文案是发给模型看的,不走
    // cli/i18n(那边只管界面上给人看的文案),跟本函数其余错误文案同一规矩。
    const auto reject = [&fail_account](const std::string& cause, const std::string& message) -> Result {
        if (fail_account.param_fail_cause() == cause) {
            fail_account.set_param_fail_streak(fail_account.param_fail_streak() + 1);
        } else {
            fail_account.set_param_fail_cause(cause);
            fail_account.set_param_fail_streak(1);
        }
        if (fail_account.param_fail_streak() < kParamFailLimit) {
            return {message, true};
        }
        return {"[agent 工具连败保险] 本回合内 agent 工具已第 " +
                    std::to_string(fail_account.param_fail_streak()) +
                    " 次因同一参数错误被拒(" + cause +
                    "),本次调用直接拒绝:同样的入参再重试也不会成功。出路二选一:1) 按下面的参数要求把入参"
                    "改对后再调用,参数合规的调用照常受理;2) 不再委托子代理,直接在当前对话里完成任务,并向"
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

    DispatchRequest request;
    if (!input.contains("prompt") || !input.at("prompt").is_string()) {
        return reject("缺少必填参数 prompt",
                      "prompt 是必填字符串。把背景、任务和报告要求写进一段自包含说明；普通的一句话任务也"
                      "合法。示例:prompt=\"检查 src 里的取消链，报告文件、行号和风险\"。");
    }
    request.spec = std::make_shared<const agent::AgentTaskSpec>(
        agent::MakeAgentTaskSpec(title, input.at("prompt").get<std::string>()));
    if (const std::string error = agent::ValidateAgentTaskSpec(*request.spec); !error.empty()) {
        return reject("prompt 不合法:" + error,
                      error + "。prompt 只须是一段自包含任务说明；宿主不要求固定章节，也不解析 Markdown。");
    }
    request.task_input_text = request.spec->instructions;
    request.title = request.spec->title;

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
    // agent_type 派发校验(阶段 4:写死两枚内置名的白名单换净成 AgentCatalog
    // ——单子 §6.2/6.3):接了解析口就一律问它,"查得到即可派"。Catalog 里
    // 码内注册的两枚带 builtin 记号,走阶段 4 之前的内置快路(行为一字不
    // 动);user/project 层显式覆盖内置名的按覆盖定义走自定义路(跨层覆盖
    // 由此真正生效)。查不到报"没有这名,看 /agents"。没接解析口(旧调用
    // 方/单测)退回旧口径:只认两枚内置名。
    std::optional<CustomAgentMaterial> custom;
    if (custom_agent_resolver_) {
        custom = custom_agent_resolver_(agent_type);
        if (!custom.has_value()) {
            return reject("agent_type 取值不合法",
                          "没有名叫 \"" + agent_type + "\" 的可派 Agent。可用类型看 /agents 清单;定义解析有错"
                          "先跑 /agent doctor " + agent_type +
                          " 查诊断。确认名字拼写后重试;不挑类型就不传 agent_type,默认 general-purpose"
                          "(多步操作)或 \"Explore\"(只读搜索分析)。");
        }
        if (custom->builtin) {
            custom.reset();  // 码内内置两枚:内置快路,自定义派发材料不掺和
        }
    } else if (agent_type != "general-purpose" && agent_type != "Explore") {
        return reject("agent_type 取值不合法",
                      "agent_type 只认 general-purpose 或 Explore(本入口未接自定义 Agent 目录)。示例:"
                      "agent_type=\"Explore\"(只读搜索分析);不确定就不传,默认 general-purpose。");
    }
    request.agent_type = agent_type;

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
    request.background =
        mode_explicit ? mode_background : input.value("run_in_background", background_by_default_);

    // capability preflight(派工单 §2.4):后台后端没配就在派工口稳定拒绝——
    // 带稳定错误码、当前入口、可用模式与改法,不等任务注册后才报;此口在
    // 隔离房创建、Resolver、backend 构造之前,backend 零调用、worktree 零
    // 创建。auto 落到前台(没有 background_by_default_ 或显式 foreground)
    // 的调用不受影响。
    if (request.background && !BackgroundBackendAvailable(env)) {
        return {BackgroundUnavailableText(env != nullptr), true};
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
    if (isolation == "worktree" && env != nullptr && env->parent_in_isolation) {
        // 嵌套开房首版稳定拒(单子 §11.3 规则 3):父已在隔离房,子再开房
        // 得先裁决"从父 HEAD 还是从父未提交工作树开分支",不偷开半间房。
        return {"父任务已在隔离 worktree 里,嵌套再开房暂不支持(nested_worktree_not_supported):"
                "请在父房内直接干活,或回主 checkout 重新派工。",
                true};
    }
    request.isolate = isolation == "worktree";

    // 入参双读(命名规范第二批):预算类键都不出 schema(见 input_schema 的
    // 说明——模型见字段就填,索性不给),但解析层照旧收:手写 JSON、老脚本、
    // 测试都还走这条路。新名 max_steps_per_turn 优先,旧名 max_turns 兼容;
    // 两者同现取新名。没给就用 default_max_steps_per_turn_(配置来的)。
    // 成本刹车(P2-6)同批:max_time_secs(墙钟硬线,秒)、max_tokens(累计
    // token 硬线,完整输入+输出)、budget_soft_percent(软线百分比,1~100,
    // 缺省 80;0 = 只留硬闸不催办)。解析次序(阶段 3 起 YAML 一并归
    // AgentProfileResolver):入参显式 > 自定义 Agent YAML 的 runtime 字段
    //(P2-1:max_steps_per_turn 真能落到派出预算)> 配置默认。
    SubagentBudget budget;
    std::string budget_deprecation_note;  // §5.3:旧预算键给了就随结果带一行提示
    budget.max_steps_per_turn = default_max_steps_per_turn_;
    // 任务总 turn 的宿主默认(turn 预算单 §4.2):配置 subagent.default_max_
    // turns(0 = 不限)。自定义 Agent 在下面 Resolver 里按
    // runtime.max_turns 压过它;模型可见的 JSON 不暴露这枚(§9.1:模型
    // 不决定预算)。
    budget.max_turns = default_max_turns_;
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
        // 弃用提示(turn 预算单 §5.3,P1-0):两枚键都是 legacy per-run step
        // 语义。max_turns 历史上是 max_steps_per_turn 的旧别名,不能无版本
        // 直接改成任务总 turn——先报弃用,要求改走 typed API(宿主 override)
        // 或 Agent YAML 的 runtime.max_turns;到明确破坏版本再改义。提示随
        // 结果带回(手写脚本作者看得见),并落一行日志。
        budget_deprecation_note =
            "[弃用提示] JSON 入参 " + budget_key + " 是待移除的单轮旧限制(每个 input round 各自上限,"
            "续投/钩子续跑会重领额度);它不是任务总 turn 预算。要限任务总量请走 Agent YAML 的"
            " runtime.max_turns 或宿主 typed 派工,别再写这枚键。";
        platform::LogSink::Instance().Warn(
            "agent_tool", "[弃用] JSON 入参 " + budget_key +
                              " 是 legacy per-run step 限制;任务总预算改走 runtime.max_turns / typed API");
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
    request.budget = budget;
    request.budget_deprecation_note = std::move(budget_deprecation_note);

    // ---- 阶段 3:统一解析(自定义 Agent)---------------------------------------
    // 父上下文 + 定义 -> ResolvedAgentProfile 的合并只许发生在
    // AgentProfileResolver 一处(单子 §6.1:一条业务规矩只许有一处权威)。
    // 活材料:皮(agent_profile_,SetAgentProfile 时已剥掉 main 的活字段)、
    // 配置默认步数、窗口、环境账与父有效工具面。P0-3 起环境账分两路:main
    // 直派读会话活账(执行在 main 线程);嵌套派工吃冻结快照(env->
    // resolve_environment,父任务派出时刻定格),不读会话当下的权限档。
    // 结构化错误(权限越宽、缺依赖、allow 越权)在此明拒——不是入参错,
    // 不进连败账。
    std::optional<agent::AgentProfileResolveEnvironment> environment;
    if (custom.has_value()) {
        if (env != nullptr && env->resolve_environment.has_value()) {
            environment = env->resolve_environment;
        } else if (resolve_environment_) {
            environment = resolve_environment_();
        }
    }
    std::optional<agent::ResolvedAgentProfile> resolved_storage;
    if (custom.has_value()) {
        // 父有效工具面:嵌套用父任务的表(env->base_registry,收窄投影);
        // main 直派用子表 + 延迟过滤。父没有的工具,子定义长不出来(单子
        // §11.1"只做交集")。
        const ToolRegistry* face_source =
            env != nullptr && env->base_registry != nullptr ? env->base_registry : &sub_registry_;
        std::vector<std::string> parent_tool_names;
        parent_tool_names.reserve(face_source->All().size());
        for (const auto& tool : face_source->All()) {
            if (env != nullptr || tool_filter_ == nullptr || tool_filter_(*tool)) {
                parent_tool_names.push_back(tool->name());
            }
        }
        agent::AgentDispatchOverrides overrides;
        if (budget_arg != nullptr) {
            overrides.max_steps_per_turn = budget.max_steps_per_turn;  // 入参显式压过 YAML
        }
        resolved_storage = agent::ResolveAgentProfile(agent::BuildSubagentResolveRequest(
            custom->definition, agent_profile_, std::move(parent_tool_names),
            default_max_steps_per_turn_, default_max_turns_, context_window_tokens_, environment, overrides));
        if (!resolved_storage->ok()) {
            return {"自定义 Agent \"" + agent_type + "\" 解析不过,已拒发(定义或环境有错,重试同样的入参"
                    "不会成功;先 /agent doctor " + agent_type + " 看诊断):\n" +
                        agent::FormatResolutionIssues(resolved_storage->issues),
                    true};
        }
        // 步数预算的权威账在解析器里(入参 > YAML > 配置默认),派发链照抄。
        request.budget.max_steps_per_turn = resolved_storage->profile.runtime.max_steps_per_turn;
        // 任务总 turn 同一只有权威账(turn 预算单 P0-3:Resolver 合并预算与
        // 来源,两路同源):override(只可收窄)> 定义 runtime.max_turns >
        // 配置默认。派发链照抄,不再各算一套。
        request.budget.max_turns = resolved_storage->turn_budget.max_turns;
    }
    request.resolved = resolved_storage;
    const agent::ResolvedAgentProfile* resolved =
        resolved_storage.has_value() ? &*resolved_storage : nullptr;

    // 权限收窄执法(阶段 4 接线):Resolver 校验过"不许放宽"(越宽在派发口
    // 明拒),"收窄生效"在这半截——子定义档比父会话档严时(父 yolo 子
    // confirm),把确认下限带进 RunTask,子代理循环里 needs_confirm 的工具
    // 真把确认拉回。父档经环境账现读(嵌套用冻结账);没递环境账(旧调用
    // 方/单测)按"没账可查"跳过——与技能/MCP 查账同一骨气,不报错,也不放宽。
    std::optional<agent::AgentPermissionMode> permission_floor;
    if (resolved != nullptr && environment.has_value() &&
        agent::AgentPermissionModeRank(resolved->permission) <
            agent::AgentPermissionModeRank(environment->parent_permission)) {
        permission_floor = resolved->permission;
    }
    request.permission_floor = permission_floor;
    request.custom = custom;

    // 派工瞬间冻结实际调用者目录。嵌套任务必须认父快照 effective_cwd，
    // 不能回读随后可能已被 /worktree 改过的 AgentTool::cwd_。
    request.caller_cwd = env != nullptr && !env->effective_cwd.empty() ? env->effective_cwd : cwd_;
    if (request.isolate) {
        request.caller_base = lubancode::cli::FreezeWorktreeBase(Utf8ToPath(request.caller_cwd), git_runner_);
        if (request.caller_base->commit.empty()) {
            return {"isolation=worktree 冻结调用者 HEAD 失败(不在可用 git 仓库里?): " + request.caller_cwd, true};
        }
    }

    // 派工前查单(2.3):只解析 prompt 里明确的 todos/*.todo。当前 HEAD 中
    // 状态已实现/已销且没有未勾批次时，在建房、注册和请求后端前稳定拒绝。
    if (const auto completed = CompletedTodoDispatchError(
            request.task_input_text, Utf8ToPath(request.caller_cwd), git_runner_);
        completed.has_value()) {
        return {*completed, true};
    }

    // 入参过了检:连败账翻篇——改对了就不记仇,后续调用从引导文案重新起。
    fail_account.set_param_fail_cause(std::string());
    fail_account.set_param_fail_streak(0);

    // 基表:Explore 走只读表(main 资产,嵌套共享无妨——只读工具无状态);
    // general-purpose/自定义在嵌套路用父任务的生效表(收窄投影),main 直派
    // 用子表。
    ToolRegistry* task_registry = nullptr;
    if (agent_type == "Explore" && explore_registry_ != nullptr) {
        task_registry = explore_registry_;
    } else if (env != nullptr && env->base_registry != nullptr) {
        task_registry = env->base_registry;
    } else {
        task_registry = &sub_registry_;
    }
    (void)headless;
    if (request.background) {
        return LaunchBackground(request, *task_registry, caller, env);
    }
    return ExecuteForeground(request, *task_registry, caller, env);
}

Tool::Result AgentTool::ExecuteForeground(const DispatchRequest& request, ToolRegistry& task_registry,
                                          const AgentRunIdentity& caller,
                                          const std::shared_ptr<const SubagentDispatchEnv>& env) {
    const std::string& agent_type = request.agent_type;
    const SubagentBudget& budget = request.budget;
    const CustomAgentMaterial* custom = request.custom.has_value() ? &*request.custom : nullptr;
    const agent::ResolvedAgentProfile* resolved =
        request.resolved.has_value() ? &*request.resolved : nullptr;
    const bool headless = env != nullptr && env->headless;
    // isolation=worktree:建房、锁房、工具表套 base_dir 包装、隔离范围压栈,
    // 跑完收工(干净删房,有活留房附路径)。cwd 一根指头都不动。嵌套且父
    // 已在房里的,派工口已拒(见 ExecuteDispatch 的 nested_worktree 分支)。
    std::optional<lubancode::cli::AgentWorktree> room;
    std::unique_ptr<ToolRegistry> isolated_registry;
    std::optional<ScopedIsolation> scope_guard;
    std::optional<IsolationScope> scope_storage;
    if (request.isolate) {
        Result setup_error;
        room = SetupIsolationRoom(request.caller_cwd, request.caller_base, git_runner_, setup_error);
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
    // P0-2 起 lineage 与 admission 在同一笔注册事务里落账。
    AgentTaskSnapshot snapshot;
    snapshot.agent_type = agent_type;
    snapshot.title = request.title;
    snapshot.prompt = request.task_input_text;
    snapshot.effective_cwd = scope_storage.has_value() ? scope_storage->base_dir : request.caller_cwd;
    snapshot.spec = request.spec;
    snapshot.parent_task_id = caller.task_id;
    snapshot.foreground = true;
    snapshot.delivery_target = TaskDeliveryTarget::ForegroundCaller;
    snapshot.step_limit = budget.max_steps_per_turn;  // 派出时预算进快照(规格"现场四")
    snapshot.turn_limit = budget.max_turns;  // 任务总 turn 派出即冻结(注册事务写死进 turn_account)
    snapshot.wall_limit_secs = budget.max_wall_secs;
    snapshot.token_limit = budget.max_total_tokens;
    snapshot.state = AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    snapshot.delivered = true;
    // 隔离基线进快照(派工单 §三):TaskSnapshot 持久化基线提交,恢复/重试/
    // 嵌套派工对账都认这一枚,不再各算各的。
    if (room.has_value()) {
        snapshot.isolation_branch = room->branch;
        snapshot.isolation_base_ref = room->base_ref;
        snapshot.isolation_base_commit = room->base_commit;
    }
    std::string admission_error;
    const std::shared_ptr<TaskRecord> task = coordinator_->ledger().TryRegisterChild(
        std::move(snapshot), caller.depth + 1, coordinator_->governance(), &admission_error);
    if (task == nullptr) {
        return {admission_error, true};
    }

    // 前台分两路(P0-3/§11.2):main 直派走老路——活 hooks 转发、父轮取消链、
    // UI 确认都在;无 UI 的嵌套路(后台父任务的孩子)拿父的冻结材料:
    // 放行账继承根任务派出时的定格,确认一律查账,不弹终端。
    const Hooks hooks = hooks_;
    const Hooks* foreground_hooks = headless ? nullptr : &hooks;
    std::shared_ptr<lubancode::hooks::DetachedHookSession> headless_hooks;
    std::shared_ptr<const BackgroundPermissionLedger> headless_permissions;
    if (headless) {
        headless_permissions = env->background_permissions;
        if (env->hook_dispatcher != nullptr && !env->hook_dispatcher->Empty()) {
            headless_hooks = std::make_shared<lubancode::hooks::DetachedHookSession>(
                env->hook_dispatcher, env->hook_dispatcher->context());
        }
    }
    // P0-2/P1-2 轨迹:前台派工在父线程申请子账——此刻父桥活着,父子边挂
    // 得上。嵌套(headless)路现在也申请:parent_run_id 传派工者自己的
    // agent_run_id(caller.agent_run_id),不再让 SpawnSubagent 把
    // relations.parent_run_id 落回 main——它的父亲是父任务的 run(单子
    // §12.3 第一条)。caller.agent_run_id 空(父任务自己没开轨迹账,或
    // main 直派)时,SpawnSubagent 按空串落回本场 main_run_id,行为与从前
    // main 直派一致。
    std::unique_ptr<runtime::TrajectorySubagentBridge> trajectory;
    if (hooks.trajectory_spawn) {
        trajectory =
            hooks.trajectory_spawn(agent_type + ": " + task->snapshot.prompt.substr(0, 120), caller.agent_run_id);
    }
    if (trajectory != nullptr) {
        // 回填自己的 run id(P1-2):这只任务若再往下派孩子,RunTask 顶部的
        // ScopedDispatchIdentity 从这份快照投影身份——必须先写好才能被
        // 正确继承(单一写者:注册后只有这里改这个字段一次)。
        std::lock_guard<std::mutex> lock(coordinator_->ledger().mutex);
        task->snapshot.agent_run_id = trajectory->run_id();
        coordinator_->ledger().Touch();
    }
    // 嵌套前台的 backend:与父共用同一份 detached 材料(父阻塞等它,无并发;
    // profile 用冻结的 provider/model)。main 直派照旧用主回合那条。
    std::optional<DetachedRequestBackend> headless_backend_storage;
    api::Backend* run_backend = &backend_;
    const DetachedAgentBackend* run_detached = nullptr;
    if (headless) {
        if (env->detached_shared == nullptr) {
            return {"嵌套前台派工缺少冻结后端材料,已拒发(重新派工或改走后台)。", true};
        }
        headless_backend_storage.emplace(*env->detached_shared);
        run_backend = &*headless_backend_storage;
        run_detached = env->detached_shared.get();
    }
    Result result = RunTask(*run_backend, effective_registry, task->snapshot.prompt, agent_type, budget,
                            foreground_hooks, task,
                            /*detached=*/run_detached,
                            /*prepared_system_prompt=*/nullptr,
                            scope_storage.has_value() ? &*scope_storage : nullptr,
                            /*background_hooks=*/headless_hooks,
                            /*background_permissions=*/headless_permissions,
                            custom, resolved, request.permission_floor, std::move(trajectory), env);
    if (room.has_value()) {
        const auto finish = FinishIsolationRoom(*room, git_runner_);
        result.AppendText(finish.note);
        result.AppendText(room->caller_note);
        // 房态进快照(派工单 §五):清理时机面板/详情看得见,回传路径是否
        // 仍有效有账可查。
        {
            std::lock_guard<std::mutex> lock(coordinator_->ledger().mutex);
            task->snapshot.worktree_removed = finish.removed;
            task->snapshot.worktree_awaiting_review = finish.awaiting_review;
            coordinator_->ledger().Touch();
        }
    }
    // 收尾入账:未送达的介入消息逐条列原文记进结果文本,不无声遗失;面板
    // x 停掉(task->cancel)与父轮 ESC 打断(hooks.cancel)都算取消;嵌套路
    // 没有父轮 ESC,只看自己的取消链。
    result.AppendText(TaskLedger::UndeliveredInboxNote(task));
    // §5.3 弃用提示:手写 JSON 给了旧预算键,随结果带回(空串 = 没用旧键,
    // AppendText 对空串零输出)。
    if (!request.budget_deprecation_note.empty()) {
        result.AppendText(request.budget_deprecation_note);
    }
    coordinator_->ledger().FinalizeFromToolResult(
        task, result.content,
        task->cancel.load(std::memory_order_acquire) ||
            (foreground_hooks != nullptr && foreground_hooks->cancel != nullptr &&
             foreground_hooks->cancel->load(std::memory_order_acquire)));
    return result;
}

Tool::Result AgentTool::LaunchBackground(const DispatchRequest& request, ToolRegistry& task_registry,
                                         const AgentRunIdentity& caller,
                                         const std::shared_ptr<const SubagentDispatchEnv>& env) {
    const std::string& agent_type = request.agent_type;
    const SubagentBudget& budget = request.budget;
    const CustomAgentMaterial* custom = request.custom.has_value() ? &*request.custom : nullptr;
    const agent::ResolvedAgentProfile* resolved =
        request.resolved.has_value() ? &*request.resolved : nullptr;
    const bool nested = env != nullptr;
    const bool headless = nested && env->headless;
    // 后端/表的材料分两路:main 线程上的派工(main 直派,或前台任务的
    // 孩子在 main 线程派)读会话工厂,安全;无 UI 的嵌套树(后台父任务的
    // 后代)吃冻结材料——backend_factory 每只孩子一份独立 client,值在
    // 祖先派出当口定格,任务树中途 /model 换档不影响在跑的树。
    std::function<DetachedAgentBackend()> backend_source;
    if (nested && env->backend_factory) {
        backend_source = env->backend_factory;
    } else if (!nested && detached_backend_factory_) {
        backend_source = detached_backend_factory_;
    } else if (!headless && detached_backend_factory_) {
        backend_source = detached_backend_factory_;
    } else if (!nested) {
        // 执行口兜底(派工单 §二):正常该在 ExecuteDispatch 的 preflight 就
        // 拦下;走到这里是装配中途工厂被拆——同一套稳定文案,不换说法。
        return {BackgroundUnavailableText(/*nested=*/false), true};
    } else {
        return {"[background_unavailable] 嵌套后台派工没有可用的冻结后端工厂,已拒发:请由当前代理直接完成,"
                "或改派前台任务(execution_mode=foreground)。",
                true};
    }
    const std::function<std::unique_ptr<ToolRegistry>()> registry_source =
        nested && env->registry_factory ? env->registry_factory : detached_registry_factory_;
    // isolation=worktree:派工线程里把房建好、锁上,建不成同步报错——后台
    // 任务没人可问,失败要立刻回给模型。房信息带进线程,收工清理。嵌套且
    // 父已在房里的,派工口已拒(nested_worktree_not_supported)。
    std::optional<lubancode::cli::AgentWorktree> room;
    if (request.isolate) {
        Result setup_error;
        room = SetupIsolationRoom(request.caller_cwd, request.caller_base, git_runner_, setup_error);
        if (!room.has_value()) {
            return setup_error;
        }
    }

    // 已收尾的 std::thread 若一直不 join,系统线程句柄会跟着会话一路攒。
    // 收柄对账按自家任务号查(病灶一的规矩,P0-3 迁进协调器)。
    coordinator_->ReapSettledThreads();
    {
        // 全局并发槽的同步先手检查:满了明报,不等注册事务里那笔硬账——
        // 后端 client 还没白造。
        if (coordinator_->ledger().RunningCount() >=
            static_cast<std::size_t>(coordinator_->governance().max_active)) {
            return {"后台子代理已跑满 " + std::to_string(coordinator_->governance().max_active) +
                        " 路,请等一项收尾后再开",
                    true};
        }
    }

    auto detached = std::make_shared<DetachedAgentBackend>();
    std::unique_ptr<ToolRegistry> detached_registry;
    try {
        *detached = backend_source();
        detached_registry = registry_source ? registry_source() : nullptr;
    } catch (const std::exception& error) {
        return {"后台子代理初始化失败: " + std::string(error.what()), true};
    } catch (...) {
        return {"后台子代理初始化失败: 未知错误", true};
    }
    if (!detached->backend) {
        return {"后台子代理后端创建失败", true};
    }

    AgentTaskSnapshot snapshot;
    snapshot.agent_type = agent_type;
    snapshot.title = request.title;
    snapshot.prompt = request.task_input_text;
    snapshot.effective_cwd = room.has_value() ? PathToUtf8(room->room_path) : request.caller_cwd;
    snapshot.spec = request.spec;
    snapshot.parent_task_id = caller.task_id;
    // 送达去处(单子 §7.1):根任务进 main 回合上下文;嵌套任务进直接父的
    // mailbox——main 不跨级提走,子代理才拿得到自己派出去的结果。
    snapshot.delivery_target = caller.task_id == 0 ? TaskDeliveryTarget::MainTurnContext
                                                   : TaskDeliveryTarget::ParentTaskInbox;
    snapshot.step_limit = budget.max_steps_per_turn;  // 派出时预算进快照(规格"现场四")
    snapshot.turn_limit = budget.max_turns;  // 任务总 turn 派出即冻结(注册事务写死进 turn_account)
    snapshot.wall_limit_secs = budget.max_wall_secs;
    snapshot.token_limit = budget.max_total_tokens;
    snapshot.state = AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    // 隔离基线进快照(派工单 §三):前后台同一枚账。
    if (room.has_value()) {
        snapshot.isolation_branch = room->branch;
        snapshot.isolation_base_ref = room->base_ref;
        snapshot.isolation_base_commit = room->base_commit;
    }
    std::string admission_error;
    const std::shared_ptr<TaskRecord> task = coordinator_->ledger().TryRegisterChild(
        std::move(snapshot), caller.depth + 1, coordinator_->governance(), &admission_error);
    if (task == nullptr) {
        return {admission_error, true};
    }

    const int id = task->snapshot.id;
    const std::string prompt = task->snapshot.prompt;
    // 病十(批三):四段开关从皮(AgentProfile)来,子代理默认与主代理同段。
    // 自定义 Agent(P2-2)另换人格段并预装技能;阶段 2 起选 Prompt Profile
    // 与能力推导,阶段 3 起三笔决议从 ResolvedAgentProfile 来(见
    // BuildSubagentPromptOptions)。
    agent::PromptOptions prompt_options = BuildSubagentPromptOptions(
        task->snapshot.effective_cwd, agent_type, prompts_dir_, project_prompts_dir_, project_instructions_,
        skills_segment_, agent_profile_, custom, resolved, package_profile_roots_);
    std::string system_prompt = agent::AssembleSystemPrompt(prompt_options);
    if (custom != nullptr) {
        system_prompt += AppendPreloadedSkills(custom->definition.skills_preload, custom->preloaded_skills);
    }
    system_prompt += "\n\n这是后台任务。调用文件与搜索工具时以运行环境段里的工作目录为准;"
                     "不要依赖进程当前目录,它可能随主会话切换。";
    system_prompt = agent::WithModelInstructions(system_prompt, detached->model_instructions);
    if (resolved == nullptr || resolved->soul) {
        system_prompt = agent::WithSoul(system_prompt, detached->soul);
    }
    // 后台注册表:工厂没给(旧调用方/单测)就把调用方的表逐枚转发包一份,
    // 避免任务线程直用调用方的活表。scoped agent 工具在 RunTask 里按这只
    // 任务的 identity/环境替换挂上(缺口 A 的修复落点)。
    if (detached_registry == nullptr) {
        detached_registry = std::make_unique<ToolRegistry>();
        for (const auto& tool : task_registry.All()) {
            detached_registry->Register(std::make_unique<ForwardingTool>(*tool));
        }
    }
    ToolRegistry* registry = detached_registry.get();
    // hooks 会话:派工线程里造好(拷一份只读策略快照,含信任/禁用账)再带
    // 进线程——后台线程不碰 dispatcher 账本与定义表,记录只投递,主会话
    // 安全点归并(hooks/detached.hpp 的线程规矩)。嵌套路用冻结的
    // dispatcher 指针,不读会话活 hooks。
    lubancode::hooks::HookDispatcher* hook_dispatcher =
        env != nullptr && env->hook_dispatcher != nullptr ? env->hook_dispatcher : hooks_.hook_dispatcher;
    std::shared_ptr<lubancode::hooks::DetachedHookSession> background_hooks;
    if (hook_dispatcher != nullptr && !hook_dispatcher->Empty()) {
        background_hooks =
            std::make_shared<lubancode::hooks::DetachedHookSession>(hook_dispatcher, hook_dispatcher->context());
    }
    // 放行账快照(修"后台审批不查放行账"):无 UI 的嵌套树原样继承祖先的
    // 冻结账(单子不变量 7:只能吃派出时已有的放行账);main 线程上的派工
    // 在派工线程定格源。源没配 = 空账,后台照旧全拒。
    std::shared_ptr<const BackgroundPermissionLedger> background_permissions;
    if (headless && env->background_permissions != nullptr) {
        background_permissions = env->background_permissions;
    } else if (background_permission_source_) {
        background_permissions =
            std::make_shared<BackgroundPermissionLedger>(background_permission_source_());
    }
    // P0-2/P1-2 轨迹:main 直派的后台派工在派工线程申请子账(spawn 钩子
    // 引用的父桥此刻活着);子账随线程走,收口在 RunTask 里办。嵌套路
    // (parent 是某只子代理,不论它自己前台/后台)现在也申请——parent_run_id
    // 传 caller.agent_run_id,不冒充 main(单子 §12.3 第一条)。caller 空
    // (main 直派,或父任务自己没开轨迹账)时按空串落回本场 main_run_id。
    std::unique_ptr<runtime::TrajectorySubagentBridge> trajectory;
    if (hooks_.trajectory_spawn) {
        trajectory = hooks_.trajectory_spawn(agent_type + ": " + prompt.substr(0, 120), caller.agent_run_id);
    }
    if (trajectory != nullptr) {
        // 回填自己的 run id(P1-2,与前台路同一规矩):写在起线程之前——
        // std::thread 构造自带 happens-before,线程内 RunTask 读到的是这次
        // 写入之后的值,不需要额外同步。
        std::lock_guard<std::mutex> lock(coordinator_->ledger().mutex);
        task->snapshot.agent_run_id = trajectory->run_id();
        coordinator_->ledger().Touch();
    }
    // 孩子的派工环境:后台任务开跑即冻结——嵌套任务原样继承祖先环境的材料
    // (backend 工厂/放行账/dispatcher/解析账),main 直派按会话当下活账
    // 填(此刻在 main 线程,读活账安全);detached_shared 指向本任务自己的
    // 材料,它的前台孩子与它共用(它阻塞等孩子,无并发)。由 RunTask 挂到
    // 它的 scoped agent。
    auto child_env = std::make_shared<SubagentDispatchEnv>();
    if (nested) {
        *child_env = *env;
        child_env->backend_factory = backend_source;
        child_env->registry_factory = registry_source;
    } else {
        child_env->registry_factory = detached_registry_factory_;
        child_env->background_permissions = background_permissions;
        child_env->hook_dispatcher = hooks_.hook_dispatcher;
        if (resolve_environment_) {
            child_env->resolve_environment = resolve_environment_();
        }
        child_env->backend_factory = backend_source;
    }
    child_env->detached_shared = detached;
    child_env->base_registry = nullptr;   // RunTask 里按这只任务自己的生效表填
    child_env->parent_in_isolation = room.has_value() || (env != nullptr && env->parent_in_isolation);
    child_env->effective_cwd = task->snapshot.effective_cwd;
    child_env->headless = true;
    // 隔离基线附言(派工单 §三):room 马上 move 进任务线程,给启动回执的
    // 那份先拷出来。
    const std::string isolation_caller_note = room.has_value() ? room->caller_note : std::string();
    coordinator_->TrackThread(
        id, std::thread([this, task, registry, prompt, agent_type, budget,
                                custom_copy = request.custom, resolved_copy = request.resolved,
                                permission_floor = request.permission_floor,
                                detached, system_prompt = std::move(system_prompt),
                                detached_registry = std::move(detached_registry),
                                room = std::move(room), background_hooks,
                                background_permissions, trajectory = std::move(trajectory),
                                child_env]() mutable {
            (void)detached_registry;  // 让独立工具表活到线程收尾
            // isolation=worktree:线程里包表、压隔离范围,收工清理。包装表按
            // 引用持源表工具,声明在源表之后,析构反序先亡,引用不悬垂。
            std::unique_ptr<ToolRegistry> isolated_registry;
            std::optional<ScopedIsolation> scope_guard;
            std::optional<IsolationScope> scope_storage;
            if (room.has_value()) {
                scope_storage =
                    IsolationScope{room->name, PathToUtf8(room->room_path), PathToUtf8(room->repo_root)};
                isolated_registry = BuildIsolatedRegistry(*registry, *scope_storage);
                scope_guard.emplace(*scope_storage);
            }
            ToolRegistry& effective_registry = isolated_registry != nullptr ? *isolated_registry : *registry;
            DetachedRequestBackend backend(*detached);
            Result result;
            try {
                result = RunTask(backend, effective_registry, prompt, agent_type, budget, nullptr, task,
                                 detached.get(), &system_prompt,
                                 scope_storage.has_value() ? &*scope_storage : nullptr, background_hooks,
                                 background_permissions, custom_copy.has_value() ? &*custom_copy : nullptr,
                                 resolved_copy.has_value() ? &*resolved_copy : nullptr, permission_floor,
                                 std::move(trajectory), child_env);
            } catch (const std::exception& error) {
                result = {"子代理执行失败: " + std::string(error.what()), true};
            } catch (...) {
                result = {"子代理执行失败: 未知错误", true};
            }
            if (room.has_value()) {
                const auto finish = FinishIsolationRoom(*room, git_runner_);
                result.AppendText(finish.note);
                result.AppendText(room->caller_note);
                {
                    std::lock_guard<std::mutex> lock(coordinator_->ledger().mutex);
                    task->snapshot.worktree_removed = finish.removed;
                    task->snapshot.worktree_awaiting_review = finish.awaiting_review;
                    coordinator_->ledger().Touch();
                }
            }
            // 收尾前点一遍没送达的介入消息:任务都要结束了,排着的信没有下一个
            // 轮次边界可等——逐条列原文记进结果文本,不无声遗失。
            result.AppendText(TaskLedger::UndeliveredInboxNote(task));
            coordinator_->ledger().FinalizeFromToolResult(task, result.content,
                                                          task->cancel.load(std::memory_order_acquire));
            // 结果归父(P0-4):嵌套后台任务的完成进直接父 mailbox,唤醒等孩子
            // 的父;main 根任务照旧由主回合 DrainCompletionNotices 取。
            coordinator_->ledger().DeliverChildCompletion(task);
        }));

    // §5.3 弃用提示:手写 JSON 给了旧预算键,随启动回执带回(空 = 没用)。
    // 隔离基线附言(派工单 §三)一并随回执亮明:后台任务的房在派工线程建
    // 好,调用方当场该知道基线与未提交改动的边界。
    std::string acceptance = "后台子代理 #" + std::to_string(id) + " (" + agent_type +
                             ") 已启动。主会话可以继续;完成结果会在后续回合送达。";
    if (!isolation_caller_note.empty()) {
        acceptance += isolation_caller_note;
    }
    if (!request.budget_deprecation_note.empty()) {
        acceptance += "\n" + request.budget_deprecation_note;
    }
    return {acceptance, false};
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
                                const CustomAgentMaterial* custom,
                                const agent::ResolvedAgentProfile* resolved,
                                std::optional<agent::AgentPermissionMode> permission_floor,
                                std::unique_ptr<runtime::TrajectorySubagentBridge> trajectory,
                                const std::shared_ptr<const SubagentDispatchEnv>& env) {
    // 派工治理(P0-2 起):admission(并发槽/深度/父子门)在注册事务
    // (TryRegisterChild)里判过——深度沿台账 lineage,不再用全局原子猜;
    // 活跃数即台账活态计数,任务终态即退槽。这里不再占槽。
    // TLS 执行身份(P0-3):这只任务在当前线程上跑,它派的孩子按这层身份
    // 记 lineage;外层任务的身份在收场时还原(前台嵌套链不串层)。
    std::optional<ScopedDispatchIdentity> dispatch_identity;
    if (task != nullptr) {
        dispatch_identity.emplace(IdentityOfSnapshot(task->snapshot));
    }

    // 每任务私有 todo + scoped agent(P0-3/P0-4):todo_write 换成本任务独占
    // 实例;"agent" 先剥掉、身份与环境算好后换成本任务专属的薄壳——前台/
    // 后台任务的私有表都从这挂递归资格,资格由 lineage 深度与父工具面执法
    //(单子缺口 A 的落点)。源表既没 todo_write 也没 agent 时直用源表。
    TaskScopedRegistry scoped_registry = BuildTaskScopedRegistry(task_registry);
    ToolRegistry& effective_registry =
        scoped_registry.registry != nullptr ? *scoped_registry.registry : task_registry;

    // ---- 孩子的派工环境(P0-3 冻结快照)------------------------------------
    // 本任务的孩子从这里派生环境:嵌套任务原样继承祖先的冻结材料(backend
    // 工厂/放行账/dispatcher/解析账),基表换成自己的生效表;main 直派的前
    // 台任务(env 为空,此刻在 main 线程)按会话活账现填——孩子的派工也在
    // main 线程发生,读活账安全。后台任务的 headless 一路向下真:孩子们的
    // 确认一律查放行账,不弹终端(单子 §11.2)。
    auto child_env = std::make_shared<SubagentDispatchEnv>();
    if (env != nullptr) {
        *child_env = *env;
    } else {
        child_env->registry_factory = detached_registry_factory_;
        // 嵌套后台孩子要独立 client:会话接了冻结工厂源就用冻结的;没接
        //(旧调用方/单测)回落活工厂——此路孩子的派工只发生在 main 线程,
        // 与旧行为一致。
        if (frozen_backend_spawner_source_) {
            child_env->backend_factory = frozen_backend_spawner_source_();
        } else {
            child_env->backend_factory = detached_backend_factory_;
        }
        child_env->hook_dispatcher = hooks_.hook_dispatcher;
        if (resolve_environment_) {
            child_env->resolve_environment = resolve_environment_();
        }
        child_env->headless = false;
    }
    child_env->base_registry = &effective_registry;
    child_env->parent_in_isolation = isolation_scope != nullptr || (env != nullptr && env->parent_in_isolation);
    child_env->effective_cwd = task != nullptr && !task->snapshot.effective_cwd.empty()
                                   ? task->snapshot.effective_cwd
                                   : (isolation_scope != nullptr ? isolation_scope->base_dir : cwd_);
    if (detached != nullptr) {
        // 后台/嵌套前台任务:孩子的前台调用与自己的请求共用这份材料(自己
        // 阻塞等孩子,无并发;main 直派的前台任务没有 detached,走主回合)。
        // detached_shared 留给嵌套前台孩子用,这里借 shared_ptr 不改所有权:
        // DetachedAgentBackend 由调用方持有,此处只临时借引用——用 non-owning
        // aliasing 构造会悬垂,改为在 LaunchBackground 侧填(那边有 shared)。
    }
    // scoped agent 挂载(第二段):资格门 = 父工具面含 agent + 非 Explore +
    // 自定义 allow 不含则不挂 + lineage 深度还有余量。深度满就不挂——工具
    // 面先说实话,不在 admission 口让模型撞墙(单子 §6.4 末行)。
    if (task != nullptr && scoped_registry.had_agent && agent_type != "Explore") {
        bool custom_allows = true;
        if (resolved != nullptr && !resolved->effective_tools.empty() &&
            std::find(resolved->effective_tools.begin(), resolved->effective_tools.end(), "agent") ==
                resolved->effective_tools.end()) {
            custom_allows = false;  // 定义收窄了工具面:父有也不给
        }
        const int child_depth = task->snapshot.depth + 1;
        if (custom_allows && child_depth <= coordinator_->governance().max_depth) {
            scoped_registry.registry->Register(std::make_unique<AgentDispatchTool>(AgentDispatchHandle(
                coordinator_, IdentityOfSnapshot(task->snapshot), child_env)));
            // scoped agent_message(P1-1 §一):与 agent 同一道资格门——这只
            // 任务能派孩子才有孩子可传话,窄实例只认自己的 task_id 为
            // caller,execute() 里逐条核对目标的 parent_task_id(单子 §9.3
            // "首版只放直接孩子")。main 那份不受此门(main 不经这条 RunTask
            // 路径,main_registry_ 装配时直挂 caller_task_id=0 的无限定实例)。
            scoped_registry.registry->Register(std::make_unique<AgentMessageTool>(this, task->snapshot.id));
            // scoped agent_watch(监督器单 P1-0):同一道资格门——能派孩子
            // 才有孩子可看。窄实例只看直接孩子(lineage 鉴权在工具里执法),
            // 无 diagnostic 档(那只给 main)。main 那份由 tool_runtime 装配
            // 时直挂 caller_task_id=0。
            scoped_registry.registry->Register(std::make_unique<AgentWatchTool>(this, task->snapshot.id));
        }
    }

    // tool_search:延迟工具索引段按"此刻的 loaded 集合"现算,拼在子代理系统
    // 提示末尾。子代理运行中途自己 tool_search 挂载了新工具,这段索引不会
    // 跟着刷新(系统提示构造后定死)——但 tools 数组每轮现拼,挂载照样生效,
    // 索引段只是稍显陈旧,无害。
    // 病十(批三):mcp/web/lsp/platforms 四段开关从皮(AgentProfile)来——
    // 子代理默认与主代理同段(裁决:补,写明补),不再是 BuildSystemPrompt
    // 薄壳的"四段不传"。阶段 2 起自定义 Agent 另走 Prompt Profile 与能力
    // 推导(见 BuildSubagentPromptOptions)。
    std::string system_prompt;
    if (prepared_system_prompt != nullptr) {
        system_prompt = *prepared_system_prompt;
    } else {
        agent::PromptOptions prompt_options = BuildSubagentPromptOptions(
            task != nullptr && !task->snapshot.effective_cwd.empty() ? task->snapshot.effective_cwd : cwd_,
            agent_type, prompts_dir_, project_prompts_dir_, project_instructions_,
            skills_segment_, agent_profile_, custom, resolved, package_profile_roots_);
        system_prompt = agent::WithDeferredToolsIndex(
            agent::AssembleSystemPrompt(prompt_options),
            agent_type == "Explore" ? std::string()
                                      : (deferred_index_provider_ ? deferred_index_provider_() : std::string()));
        if (custom != nullptr) {
            system_prompt += AppendPreloadedSkills(custom->definition.skills_preload, custom->preloaded_skills);
        }
    }
    if (isolation_scope != nullptr || (env != nullptr && env->parent_in_isolation)) {
        system_prompt += "\n\n本次任务运行在隔离的 git worktree 里。相对路径一律以运行环境段里的工作目录"
                         "为基准(包装层会自动解析);主 checkout 只读——写入、命令"
                         "工作目录、git 改道指回主树的操作都会被拦。改动留在房内,收工自会处置。";
    }
    // 每次 execute() 都是全新的、空历史的子代理——没有跨调用的状态。
    // 长任务的今天,子代理复用主 compact(CompactTurnPartitioned)与压力通报,
    // 在"工具结果攒完、请求未发"的安全点把旧探索压成检查点式存档。
    const std::string task_model = detached != nullptr && !detached->request_profile.model.empty()
                                       ? detached->request_profile.model
                                       : model_;
    // 运行策略与 main 同一份(规格根因一):输出上限、字符安全网、续跑
    // 次数从 runtime_profile_ 继承,步数用派出时的预算。成本刹车(P2-6):
    // 时间/token 硬线与软线百分比一并落进运行档案,AgentLoop 在步顶执法。
    // main 的 budget_soft_percent 默认 0(不催),子代理派发一律带软线。
    // model 走皮上的 request 档案(批四·病十一其一:运行档案不再另存一份)。
    // 阶段 3:自定义 Agent 的运行档案与请求档案从 ResolvedAgentProfile 来
    //(Resolver 已按"入参 > YAML > 父值"合并完,含四枚预算字段与模型角色);
    // 内置两枚与旧调用路径照旧从 runtime_profile_/agent_profile_ 派生,一字
    // 不动。成本三线(wall/token/软线)是派发参数不是 YAML 字段,这里叠加。
    agent::AgentRuntimeProfile task_profile =
        resolved != nullptr ? resolved->profile.runtime : runtime_profile_;
    task_profile.max_steps_per_turn = budget.max_steps_per_turn;  // 与 Resolver 同一笔账
    task_profile.max_wall_secs = budget.max_wall_secs;
    task_profile.max_total_tokens = budget.max_total_tokens;
    task_profile.budget_soft_percent = budget.soft_percent;
    if (resolved == nullptr && context_window_tokens_ > 0) {
        task_profile.context_window_tokens = context_window_tokens_;
    }
    // 活度账 + 诊断日志的包装后端:子代理的每次模型请求都从这里过。必须
    // 在 sub_agent 之前声明(它引用的寿命盖过 loop);上下文压缩那一路
    //(CompactTurnPartitioned)仍用原 backend,不混进任务的阶段账。
    std::optional<TraceBackend> traced_storage;
    if (task != nullptr) {
        traced_storage.emplace(backend, coordinator_->ledger(), task);
    }
    api::Backend& loop_backend = traced_storage.has_value() ? *traced_storage : backend;
    agent::AgentProfile task_agent_profile = resolved != nullptr ? resolved->profile : agent_profile_;
    task_agent_profile.runtime = std::move(task_profile);
    task_agent_profile.system_prompt = system_prompt;
    // Token 账本单 A1:子代理(前台 RunTask 与后台 detached 任务共用这一
    // 处)的请求用途是 subagent_turn,不是从 agent_profile_/resolved->profile
    // 继承来的主会话 MainTurn——显式覆盖。resolved_prompt_base 也一并清空:
    // 子代理系统提示的拼装次序(部分路径把延迟索引/魂/模型指令直接烤进
    // system_prompt 文本,不走 profile 三层后叠)与主会话不同,继承来的
    // 底账文本对不上这里的 system_prompt,AgentLoop 的 sync 闸本会自动
    // 退回旧路,这里显式清是让"没有 manifest"这件事在源头就说清楚,不靠
    // 隐式的文本比对兜底。
    task_agent_profile.purpose = accounting::RequestPurpose::SubagentTurn;
    task_agent_profile.resolved_prompt_base.reset();
    if (detached != nullptr) {
        task_agent_profile.provider = detached->provider;
        task_agent_profile.request = detached->request_profile;
    }
    if (task_agent_profile.request.model.empty()) {
        task_agent_profile.request.model = task_model;
    }
    // 工具可见性(病十三的方向):谓词与拒绝文案写进皮。Explore 的只读
    // 白名单是角色限制;自定义 Agent 的 tools.allow/deny 同是角色限制
    //(P2-1:library-reviewer 只给 read_file/search,写工具确实看不见)——
    // 阶段 3 起谓词由 AgentProfileResolver 装进 resolved->profile,这里整份
    // 照抄,不再手工重拼;其余角色沿用装配层灌进来的过滤(tool_search)。
    if (agent_type == "Explore") {
        task_agent_profile.tool_filter = [](const Tool& tool) { return ExploreAllows(tool); };
        // 角色限制明说(规格:错误说明写"角色限制"):模型撞到这堵墙时,
        // 文案写清限制来自只读角色,并给出角色内的替代去路。
        task_agent_profile.tool_filter_denial =
            "此工具不在 Explore 角色的只读白名单内(角色限制):请改用只读工具(read_file/search/web_fetch/"
            "web_search/lsp)完成调查;确需写入,把改动建议写进结论交回主代理执行。";
    } else if (resolved != nullptr) {
        // Resolver 已按 allow/deny 装好谓词;allow/deny 全空时旧语义是挂一只
        // 全放行谓词(自定义 Agent 不吃装配层的延迟过滤)——照旧,一字不动。
        if (resolved->profile.tool_filter != nullptr) {
            task_agent_profile.tool_filter = resolved->profile.tool_filter;
            task_agent_profile.tool_filter_denial = resolved->profile.tool_filter_denial;
        } else {
            task_agent_profile.tool_filter = [](const Tool&) { return true; };
        }
    } else if (detached == nullptr && tool_filter_) {
        task_agent_profile.tool_filter = tool_filter_;
    }
    // 动态工具 P1(通用 ProxyReference):子代理的代理引用接线。resolver
    // 用子侧那只(装配层 SetToolRefResolver 灌的,独立 ledger——main 铸的
    // ref 到子代理的账里就是 unknown_tool_ref,不串);没灌(legacy/
    // disabled)则显式清空,防 resolved/主皮拷贝把别家的账带进来。
    // 执行资格:内置/后台子代理吃装配层给的子侧策略;自定义 Agent 的
    // allow/deny 谓词本身就是它的执行资格(单子 §5.5"effective policy"),
    // 拿来当 policy,拒绝文案沿用其角色限制说明。Explore 一律不开:它的
    // 只读表没有 tool_search,铸不出合法 ref,开了只会给"借来的 ref 穿
    // 只读边界"留门。
    task_agent_profile.tool_ref_resolver = tool_ref_resolver_;
    task_agent_profile.tool_execution_policy = sub_execution_policy_;
    task_agent_profile.tool_execution_denial = sub_execution_denial_;
    if (agent_type == "Explore") {
        task_agent_profile.tool_ref_resolver = nullptr;
        task_agent_profile.tool_execution_policy = nullptr;
        task_agent_profile.tool_execution_denial.clear();
    } else if (resolved != nullptr && resolved->profile.tool_filter != nullptr) {
        task_agent_profile.tool_execution_policy = resolved->profile.tool_filter;
        task_agent_profile.tool_execution_denial =
            std::string(tools::kErrToolRefNotAllowed) + "|" +
            (resolved->profile.tool_filter_denial.empty()
                 ? std::string("此工具不在该自定义 Agent 的 tools 允许面内,不得重试同一调用。")
                 : resolved->profile.tool_filter_denial);
    }
    // 阶段 2:prompt.soul: off 的自定义 Agent 不带魂(契约 §4.2——Soul 只
    // 许继承或关,不许在 Agent 文件里另塞正文)。前台任务的魂从皮
    //(AgentProfile::soul,请求期由 AgentLoop 注入)继承,这里只关不添
    //(resolved->profile 的魂在 Resolver 侧本就清空,关即不注)。
    if (resolved != nullptr && !resolved->soul) {
        task_agent_profile.soul.clear();
    }
    agent::Agent sub_agent(loop_backend, effective_registry, std::move(task_agent_profile));
    // 子代理的项目记忆召回(存储 v2 P0-3 §6.2):派工当刻检索一次,整段
    // 冻结下发——子代理不自动扫整库;child_run_id 进快照事件的
    // relations.child_run_id,父账说得清发给了哪只孩子。provider 没设
    //(旧调用方)就不注入,行为不变。
    if (turn_context_provider_) {
        sub_agent.SetTurnContext(turn_context_provider_(
            prompt, trajectory != nullptr ? trajectory->run_id() : std::string()));
    }
    // 接线(批四·病十二):压力钩与收件口整份进 AgentWiring。
    agent::AgentWiring sub_wiring;
    if (context_window_tokens_ > 0) {
        sub_wiring.on_context_pressure = [this, &sub_agent, &backend, &task_model, task](
                                             const agent::ContextPressure& pressure) {
            if (pressure.phase != agent::ContextPressure::Phase::PreRequest || !pressure.projected_overflow) {
                return;  // AfterHardTrim/PreflightExceeded 是纯通报:前者安全网丢的东西压缩救不回,
                         // 后者是最终闸的三项账(§4.4 可观测事件),这里不动作。
            }
            agent::CompactOptions options;  // 子代理没有守恒待办,双账只做结构校验
            // 与主会话同一条双账路(四分区单·阶段 2-4):turn 分区 map +
            // final reduce 出 UserContract/WorkState,新历史 [双账][热区原文]。
            if (const auto compacted = agent::CompactTurnPartitioned(
                    backend, task_model, sub_agent.History(), options,
                    sub_agent.context().structural_options());
                compacted.has_value()) {
                sub_agent.ReplaceHistory(compacted->new_history);
                if (task != nullptr) {
                    // 消息账记一枚压缩检查点:查看态里看得到"前情进存档"的
                    // 边界,不是只剩最终一句。
                    std::string archive_text;
                    for (const auto& block : compacted->archive.content) {
                        if (const auto* text_block = std::get_if<api::TextBlock>(&block)) {
                            archive_text += text_block->text;
                        }
                    }
                    std::lock_guard<std::mutex> lock(ledger().mutex);
                    AgentTaskEvent event;
                    event.kind = AgentTaskEventKind::CompactCheckpoint;
                    event.text = std::move(archive_text);
                    ledger().AppendEventLocked(task, std::move(event));
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
            TaskMailboxKind kind = TaskMailboxKind::UserSteering;
            {
                std::lock_guard<std::mutex> inbox_lock(task->inbox_mutex);
                for (auto& item : task->inbox) {
                    if (!item.delivered) {
                        text = item.text;
                        source = item.source;
                        kind = item.kind;
                        item.delivered = true;
                        break;
                    }
                }
            }
            if (text.empty()) {
                return std::nullopt;
            }
            // 取走一条就 Touch:面板 queued 数当即归零递减。
            ledger().Touch();
            // 消息账:轮次边界注入的介入记 steering_message——先放
            // inbox_mutex 再拿台账锁,与 SendMessage(先台账锁后 inbox_mutex)
            // 不同时持两锁,锁序不冲。ChildCompletion 项的正文已带"外来资料"
            // 来路声明(TaskLedger::FormatChildCompletion),不再包 steering 壳。
            {
                std::lock_guard<std::mutex> tasks_lock(ledger().mutex);
                AgentTaskEvent event;
                event.kind = AgentTaskEventKind::SteeringMessage;
                event.text = text;
                ledger().AppendEventLocked(task, std::move(event));
                // inbox 安全送达 = 执行活(P0-0 四本时钟之 execution):等下一
                // 轮的静默从这里重新起算。
                ledger().RecordInboxDeliveredLocked(task);
            }
            api::Message message;
            message.role = api::Role::User;
            // 介入文本可能带坏串(跨会话传话/外部投递),进子代理历史前洗掉;
            // 子任务完成文本出自己代理,同洗——外来资料一律过编码关口。
            message.content.push_back(api::TextBlock{
                kind == TaskMailboxKind::ChildCompletion
                    ? platform::SanitizeExternalText(text)
                    : FormatInboxDelivery(platform::SanitizeExternalText(text), source)});
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
    // P0-2 轨迹路:给了子代理轨迹桥的,子代理的工具事件落子账(独立
    // JSONL),不再旁听进父账——父子文件只传边界引用(§3.5)。
    if (trajectory != nullptr) {
        runtime::TrajectoryTurnBridge& child_bridge = trajectory->turn_bridge();
        turn_wiring.boundary_recorder = &child_bridge;
        turn_wiring.on_tool_trace = [&child_bridge](const agent::ToolTraceEvent& event) {
            child_bridge.OnToolTrace(event);
        };
        turn_wiring.on_tool_results_committed = [&child_bridge](const std::string& batch_id,
                                                               const api::Message& results) {
            child_bridge.OnToolResultsCommitted(batch_id, results);
        };
    } else if (foreground_hooks != nullptr && foreground_hooks->on_tool_trace) {
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
            std::lock_guard<std::mutex> lock(ledger().mutex);
            AgentTaskEvent event;
            event.kind = AgentTaskEventKind::UserMessage;
            event.text = prompt;
            ledger().AppendEventLocked(task, std::move(event));
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
            ledger().Touch();
        } else if (now - task->last_activity_touch >= std::chrono::seconds(1)) {
            task->last_activity_touch = now;
            ledger().Touch();
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
                    std::lock_guard<std::mutex> lock(ledger().mutex);
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
                        // 监督相位(P0-0):token 流量只算传输活(TraceBackend 已
                        // 刷 transport),这里只翻相位。
                        ledger().RecordStageLocked(task, agent::AgentSupervisionStage::StreamingText);
                    } else if (event.item_kind == runtime::ItemKind::Thinking) {
                        task->pending_reasoning += event.text;  // 思考也入账,查看态与 main 同款折叠
                        task->activity.reasoning_bytes = task->pending_reasoning.size();
                        ++task->content_revision;
                        touch_activity(AgentTaskActivity::Stage::Thinking);
                        ledger().RecordStageLocked(task, agent::AgentSupervisionStage::StreamingThinking);
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
                        std::lock_guard<std::mutex> lock(ledger().mutex);
                        // 先把已流出的正文/思考切成事件,再记工具发起——"助手文字 ->
                        // 工具卡"的时序不许倒(规格 transcript 单测第 1 条)。
                        ledger().FlushPendingTextLocked(task);
                        AgentTaskEvent ledger_event;
                        ledger_event.kind = AgentTaskEventKind::ToolStart;
                        ledger_event.tool_name = tool_name;
                        ledger_event.input_json = tool_input.dump();
                        ledger().AppendEventLocked(task, std::move(ledger_event));
                        task->snapshot.tool_calls.push_back(
                            AgentTaskToolCall{tool_name, tool_input.dump(), std::string(), false, false, tool_use_id});
                        task->activity.stage = AgentTaskActivity::Stage::Tool;
                        task->activity.tool_name = tool_name;
                        task->activity.last_tool_name = tool_name;  // 收口不清:坞行"上次 <工具>"常驻(P2-1)
                        task->activity.tool_started = std::chrono::steady_clock::now();
                        ++task->content_revision;
                        // 执行账 + RunningTool 相位(P0-0):工具起跑刷 execution,
                        // 静默尺子(120s 软线)从这里起算。
                        ledger().RecordToolStartedLocked(task);
                        ledger().Touch();
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
                    // 适配器 Finish 的 Cancelled 兜底(P1-1 起不再裸丢):工具
                    // 已越过执行边界(ItemStarted 落过账)却被取消——副作用
                    // 是否落地无从证实。按"结果不明"收口:消息账补一张明确的
                    // 卡,通知请用户/父代理核对,绝不自动重跑(单子 §8.3)。
                    if (event.outcome == runtime::Outcome::Cancelled) {
                        const auto open_it = open_tools->find(event.item_id);
                        if (open_it != open_tools->end() && task != nullptr) {
                            const auto [cancelled_use_id, cancelled_tool] = open_it->second;
                            open_tools->erase(open_it);
                            std::lock_guard<std::mutex> lock(ledger().mutex);
                            ledger().RecordToolIndeterminateLocked(task, cancelled_tool, cancelled_use_id);
                            ledger().Touch();
                        } else {
                            open_tools->erase(event.item_id);
                        }
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
                    std::lock_guard<std::mutex> lock(ledger().mutex);
                    ledger().FlushPendingTextLocked(task);  // 工具结果前若有残余正文,先入账
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
                    ledger().AppendEventLocked(task, std::move(ledger_event));
                    // 工具收口:阶段退回 None;工具名即时清,不拿旧名字接着报秒。
                    task->activity.stage = AgentTaskActivity::Stage::None;
                    task->activity.tool_name.clear();
                    ++task->content_revision;
                    // meaningful progress(P0-0):工具结果散列做指纹——结果与
                    // 上一笔不同才算真进展;相同则指纹不动(空转计数由轮次
                    // 边界累计,单子 §6.3)。
                    ledger().RecordToolCompletedLocked(
                        task, agent::FingerprintOfParts("tool:" + tool_name, result_text));
                    ledger().Touch();
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
                    report.provider_response_id = event.payload.value("provider_response_id", std::string());
                    report.reported_by_provider = event.payload.value("reported_by_provider", false);
                    report.model = event.payload.value("model", std::string());
                    report.cache_epoch = event.payload.value("cache_epoch", 1);
                    report.epoch_break_reason = event.payload.value("epoch_break_reason", std::string());
                    report.prefix_append_only = event.payload.value("prefix_append_only", true);
                    const bool reported = event.payload.value("reported", report.reported());
                    if (task != nullptr) {
                        std::lock_guard<std::mutex> lock(ledger().mutex);
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
                        ledger().Touch();
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
        // ---- 进展合同与恢复账接线(监督器单 P0-0/P0-1)----------------------
        // 完整 assistant 消息提交进 history 是 meaningful progress 的候选:指
        // 纹变了刷进展,没变计空转轮(单子 §6.3——thinking/text 的 token 只算
        // 传输活,提交后才算一次)。请求尝试的起跑/重试从恢复环回流台账:重试
        // 那一拍把半截显示账回滚到本请求起跑的锚,不拼两段正文(单子 §8.3)。
        turn_wiring.on_assistant_message_ready = [this, task](const api::Message& message) {
            ledger().RecordAssistantMessage(task, AssistantMessageFingerprint(message));
        };
        turn_wiring.on_request_attempt = [this, task](const api::ModelRequestAttempt& attempt,
                                                      api::RequestAttemptPhase phase) {
            switch (phase) {
                case api::RequestAttemptPhase::Started:
                    ledger().RecordRequestStarted(task, attempt.attempt, attempt.history_commit_hash);
                    break;
                case api::RequestAttemptPhase::Retrying:
                    ledger().RecordRequestRetry(task, attempt.attempt, attempt.error_code);
                    break;
                case api::RequestAttemptPhase::Succeeded:
                    // 收场账由 TraceBackend 的 RecordRequestOutcome 记(那里有
                    // 错误本体),这里不双记。
                    break;
                case api::RequestAttemptPhase::Exhausted:
                    // P1-1 通知"用尽":重试链打完仍没成。恢复账(次数/稳定码)
                    // 也在这一笔里收口(单子 §十"恢复用尽"一条通知)。
                    ledger().RecordRecoveryExhausted(task, attempt.error_code);
                    break;
            }
        };
        if (foreground_hooks != nullptr) {
            // 权限收窄执法(阶段 4):子定义档比父会话档严时,确认回调换成
            // 宿主的"带下限"口——yolo/auto 的免问在里头被 min(会话档,
            // 下限) 并掉,该问就真把确认拉回(单子"执行"账:自定义 Agent
            // 取消与超时之外,权限收窄也是运行期要真生效的一笔)。宿主没接
            // floored 口(旧调用方)或档不比父严时,原样转发,行为不变。
            if (permission_floor.has_value() && foreground_hooks->on_tool_confirm_floored) {
                auto floored = foreground_hooks->on_tool_confirm_floored;
                const agent::AgentPermissionMode floor = *permission_floor;
                turn_wiring.on_tool_confirm = [floored, floor](const std::string& tool_use_id,
                                                               const std::string& name,
                                                               const nlohmann::json& input) {
                    return floored(tool_use_id, name, input, floor);
                };
            } else {
                turn_wiring.on_tool_confirm = foreground_hooks->on_tool_confirm;
            }
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
                        ledger().PushPermissionDenialNotice("后台 #" + std::to_string(task_id) + " 请求 " + name +
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
                ledger().PushPermissionDenialNotice(std::move(notice));
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

    // ---- 写前作用域闸(AGENTS.md 作用域单 P0,§7.6)-------------------------
    // 每只子代理自持一份已见指纹账:任务结束即弃,不继承父 Agent 的确认,
    // 也不与兄弟任务共享。Resolver 与主会话同一只。基线(root->cwd,拼在
    // 系统提示里的那截)按"内容逐字节对上"预登记——对不上的(搬房/外部
    // 改动/截断)首写重新注入。project_instructions 被 Agent 定义 omit 时
    // 提示里没有那截串,基线自然不登记:首写即拦、规则照注(不能静默绕
    // 过仓库规矩)。没接 resolver(旧调用方/单测)= 不过闸,行为照旧。
    if (instruction_resolver_ != nullptr) {
        auto task_scope_state = std::make_shared<InstructionScopeState>();
        MarkBaselineSeen(*instruction_resolver_, *task_scope_state, Utf8ToPath(cwd_), project_instructions_);
        turn_wiring.on_scope_gate = BuildScopeGateCallback(instruction_resolver_, std::move(task_scope_state));
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

    // 墙钟与监督登记(监督器单 P0-2 迁移):从前每任务起一根看门狗线程,
    // 100 只并发就是 100 根轮询线——现在统一登记进会话级监督器,同一根线
    // 落锤。语义一字不动:到点置 wall_stop(取消链收进停止信号——绝不置
    // task->cancel,那会被收成"用户中止");宽限期内任务线程仍没报终态
    // (所有超时全失效、后端不理取消的绝境),直接把台账翻成
    // Failed/WallClockTimeout。任务自带时间预算(P2-6 max_time_secs)时取
    // 更紧的那根:引擎侧软停(步顶查)先到,看门狗只在引擎停不下来的绝境
    // 落锤。健康拍(四本时钟的软线判)同样由监督器驱动,这只任务起跑即登。
    int effective_wall_secs = wall_clock_timeout_secs_;
    if (budget.max_wall_secs > 0 &&
        (effective_wall_secs <= 0 || budget.max_wall_secs < effective_wall_secs)) {
        effective_wall_secs = budget.max_wall_secs;
    }
    if (task != nullptr) {
        task->wall_stop.store(false, std::memory_order_release);
        task->wall_clock_fired.store(false, std::memory_order_release);
        task->no_progress_fired.store(false, std::memory_order_release);
        task->finalized.store(false, std::memory_order_release);
        task->force_finalized = false;
        coordinator_->supervisor().WatchTask(task);
        if (effective_wall_secs > 0) {
            coordinator_->supervisor().ArmWallClock(task, effective_wall_secs, wall_clock_grace_secs_);
        }
    }
    // hooks 第四五步:SubagentStart + 上下文切换。前台子代理在宿主主线程
    // 里同步跑,dispatcher 上下文换成这只子代理的(agent_id/agent_type),
    // 转发过来的工具事件发的 stdin JSON 就带子代理身份;跑完还原。后台
    // 子代理走只读快照会话:线程里真跑钩子、记录只投递。
    // P0-2:父子身份统一从台账 lineage 来(单子 §12.1)——嵌套任务(前后台
    // 都是)的 parent_agent_id/parent_task_id 指直接父,不再一律写 null;
    // main 派出时 parent_task_id=0、parent_agent_id 空。root/depth/task_spec_
    // hash/execution_mode 一并入账,hook 与 tool trace 认同一本账。
    const int hook_task_id = task != nullptr ? task->snapshot.id : 0;
    const int hook_parent_task_id = task != nullptr ? task->snapshot.parent_task_id : 0;
    const int hook_root_task_id = task != nullptr ? task->snapshot.root_task_id : 0;
    const int hook_depth = task != nullptr ? task->snapshot.depth : 1;
    const std::string hook_spec_hash =
        task != nullptr && task->snapshot.spec != nullptr ? agent::TaskSpecHash(*task->snapshot.spec)
                                                          : std::string();
    lubancode::hooks::HookDispatcher* sub_hook_dispatcher =
        foreground_hooks != nullptr ? foreground_hooks->hook_dispatcher : nullptr;
    std::optional<lubancode::hooks::HookContext> parent_hook_context;
    if (sub_hook_dispatcher != nullptr && !sub_hook_dispatcher->Empty()) {
        parent_hook_context = sub_hook_dispatcher->context();
        lubancode::hooks::HookContext sub_context = *parent_hook_context;
        sub_context.agent_id = std::to_string(hook_task_id);
        sub_context.agent_type = agent_type;
        // parent_agent_id:嵌套任务指直接父任务的 agent id;main 触发时取
        // 外层上下文的 agent id(通常为空 = main)。
        sub_context.parent_agent_id = hook_parent_task_id != 0
                                          ? std::optional<std::string>(std::to_string(hook_parent_task_id))
                                          : parent_hook_context->agent_id;

        if (sub_hook_dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStart)) {
            lubancode::hooks::HookPayload start;
            start.event = lubancode::hooks::HookEvent::SubagentStart;
            start.fields["agent_id"] = *sub_context.agent_id;
            start.fields["agent_type"] = agent_type;
            start.fields["parent_agent_id"] =
                sub_context.parent_agent_id.has_value() ? nlohmann::json(*sub_context.parent_agent_id)
                                                        : nlohmann::json();
            start.fields["task_id"] = hook_task_id;
            start.fields["parent_task_id"] = hook_parent_task_id;
            start.fields["root_task_id"] = hook_root_task_id;
            start.fields["depth"] = hook_depth;
            if (!hook_spec_hash.empty()) {
                start.fields["task_spec_hash"] = hook_spec_hash;
            }
            start.fields["execution_mode"] = detached != nullptr ? "background" : "foreground";
            // 任务级 turn 预算(turn 预算单 §11.2):Start 只报上限——账还没开
            // 跑,attempted/completed 归 Stop。字段只读,hook 不能回写预算。
            start.fields["turn_limit"] = task != nullptr ? task->snapshot.turn_limit : 0;
            sub_hook_dispatcher->EmitWith(lubancode::hooks::HookEvent::SubagentStart, start, sub_context);
        }
        sub_hook_dispatcher->UpdateContext(std::move(sub_context));
    }
    // 后台子代理身份:写进快照会话自己的上下文(须在第一次 Emit 之前)。
    if (background_hooks != nullptr && !background_hooks->Empty()) {
        lubancode::hooks::HookContext sub_context = background_hooks->context();
        sub_context.agent_id = std::to_string(hook_task_id);
        sub_context.agent_type = agent_type;
        // 后台不再一律写 parent null(单子缺口 E):嵌套任务指直接父。
        sub_context.parent_agent_id = hook_parent_task_id != 0
                                          ? std::optional<std::string>(std::to_string(hook_parent_task_id))
                                          : std::nullopt;
        sub_context.turn_id = "bgtask_" + sub_context.agent_id.value_or(std::string("0"));
        background_hooks->context() = sub_context;
        if (background_hooks->HasHandlersFor(lubancode::hooks::HookEvent::SubagentStart)) {
            lubancode::hooks::HookPayload start;
            start.event = lubancode::hooks::HookEvent::SubagentStart;
            start.fields["agent_id"] = *sub_context.agent_id;
            start.fields["agent_type"] = agent_type;
            start.fields["parent_agent_id"] = sub_context.parent_agent_id.has_value()
                                                  ? nlohmann::json(*sub_context.parent_agent_id)
                                                  : nlohmann::json();
            start.fields["task_id"] = hook_task_id;
            start.fields["parent_task_id"] = hook_parent_task_id;
            start.fields["root_task_id"] = hook_root_task_id;
            start.fields["depth"] = hook_depth;
            if (!hook_spec_hash.empty()) {
                start.fields["task_spec_hash"] = hook_spec_hash;
            }
            start.fields["execution_mode"] = "background";
            // 任务级 turn 预算(§11.2,与前台同款):Start 只报上限。
            start.fields["turn_limit"] = task != nullptr ? task->snapshot.turn_limit : 0;
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
    // 任务级 turn 预算门(turn 预算单 P0-1/P0-2):真账在 TaskRecord 的
    // turn_account(注册时按快照 turn_limit 冻结),这里只装一枚 task-
    // scoped adapter 递给引擎——初始 Run、mailbox 续投、孩子回流、Stop 钩子
    // 续跑全都从同一本账走原子准入,谁也不许重领。预算 owner 不在
    // AgentLoop(它正是"每 Run 重置"漏洞的根,设计单 §16.8),也不在
    // DriveReport(那是请求结束后的账)。进台账的旧路(task 为空的测试
    // 直调)不装门,行为与从前一字不差。
    std::optional<agent::ModelTurnBudgetGate> turn_budget_gate;
    if (task != nullptr) {
        agent::ModelTurnBudgetGate gate;
        gate.try_reserve = [this, task]() { return ledger().TryReserveModelTurn(task); };
        gate.commit_sent = [this, task](const agent::ModelTurnPermit& permit) {
            return ledger().CommitModelTurnSent(task, permit);
        };
        gate.abort_before_send = [this, task](const agent::ModelTurnPermit& permit) {
            ledger().AbortModelTurnBeforeSend(task, permit);
        };
        gate.mark_completed = [this, task](const agent::ModelTurnPermit& permit) {
            ledger().MarkModelTurnCompleted(task, permit);
        };
        gate.snapshot = [this, task]() { return ledger().ModelTurnSnapshot(task); };
        gate.claim_turn_nudge = [this, task]() { return ledger().ClaimModelTurnNudge(task); };
        turn_budget_gate = std::move(gate);
        turn_wiring.turn_budget = &*turn_budget_gate;
    }
    // 续投源(规格第五节"排到了却没送"):一轮 Run 正常收口后与 SendTaskMessage
    // 做原子交接——inbox 空封账进终态;有未送项拼成新一轮用户输入续跑一轮,
    // 续跑失败按批退回(RestoreDrainedInbox)。
    int settled_steps = 0;  // 已刷进台账的步数账(on_round_settled 累计)
    agent::DriveOptions drive_options;
    drive_options.cancel = cancel;
    drive_options.wall_clock_fired = [task]() {
        return task != nullptr && task->wall_clock_fired.load(std::memory_order_acquire);
    };
    // 任务级 turn 预算的轮间闸与投影口(turn 预算单 P0-2):一轮正常收口后
    // harness 先问剩余额度,尽了就不领续投批、不进 WaitingChildren(§7.3/
    // §7.4);DriveReport 顺带带走一份任务 turn 账的收口投影(与台账逐笔
    // 一致,对账用)。没进台账的旧路不装,行为不变。
    if (task != nullptr) {
        drive_options.turn_budget_exhausted = [this, task]() {
            return ledger().ModelTurnSnapshot(task).Exhausted();
        };
        drive_options.turn_budget_snapshot = [this, task]() { return ledger().ModelTurnSnapshot(task); };
    }
    drive_options.on_round_settled = [this, task, &settled_steps](const agent::RunOutcome& outcome) {
        // 直接记账:步数来自 RunOutcome(循环内按模型请求累计),不靠 usage
        // 回调猜——面板与终态摘要看到的 steps_used 同一笔账。顺带把这轮流
        // 到一半的正文/思考封进消息账(轮次边界)。
        settled_steps += outcome.steps_used;
        if (task != nullptr) {
            std::lock_guard<std::mutex> lock(ledger().mutex);
            ledger().FlushPendingTextLocked(task);
            task->snapshot.steps_used = settled_steps;
            ledger().Touch();
        }
    };
    if (task != nullptr) {
        drive_options.continuation = [this, task]() -> std::optional<agent::ContinuationBatch> {
            // 续投源(规格第五节"排到了却没送"+ P0-4 WaitingChildren):一轮
            // Run 正常收口后与 mailbox 做原子交接——有未送项(介入 + 子任务
            // 完成)拼成新一轮输入续跑;inbox 空且还有活孩子,不封账也不发
            // 无意义请求,进 WaitingChildren 等条件变量(孩子终态/新信/取消
            // 都会唤醒),这段等待不占 provider 请求、不烧 token(单子 §8.1);
            // inbox 空且孩子清零才封账收口。
            for (;;) {
                bool sealed = false;
                DrainedInbox drained = ledger().SealOrContinueInbox(task, sealed);
                if (!drained.indices.empty()) {
                    std::string continuation;
                    for (std::size_t i = 0; i < drained.texts.size(); ++i) {
                        if (!continuation.empty()) {
                            continuation += "\n\n";
                        }
                        // ChildCompletion 项的正文自带"外来资料"来路声明,不再
                        // 包 steering 壳;介入照旧 FormatInboxDelivery。
                        continuation += i < drained.kinds.size() &&
                                                drained.kinds[i] == TaskMailboxKind::ChildCompletion
                                            ? drained.texts[i]
                                            : FormatInboxDelivery(drained.texts[i], drained.sources[i]);
                    }
                    // 直接子任务名册(单子 §9.3/§13.3):每次续投这只子代理都是
                    // 一次"轮次边界"(与 main 每条用户消息前重算名册同一时机)
                    // ——附上此刻它自己直接孩子的最新快照,不塞孙辈。task 为
                    // 空(旧调用方/单测直调)不会走到这个 continuation 分支。
                    continuation += ledger().RunningTasksRoster(task->snapshot.id);
                    // 消息账:介入按收到次序记 steering_message——"main/用户
                    // 何时补了话"在查看态里看得见落点,不沉进黑洞(规格
                    // transcript 单测第 3 条)。cancel 已置位的短路归 harness
                    //(领批后先查再跑)。
                    {
                        std::lock_guard<std::mutex> lock(ledger().mutex);
                        for (const auto& text : drained.texts) {
                            AgentTaskEvent event;
                            event.kind = AgentTaskEventKind::SteeringMessage;
                            event.text = text;
                            ledger().AppendEventLocked(task, std::move(event));
                        }
                    }
                    agent::ContinuationBatch batch;
                    batch.input = std::move(continuation);
                    batch.restore = [this, task, drained = std::move(drained)]() mutable {
                        // 续投失败按批退回:介入信退未送,子任务的 delivered
                        // 一并退——"读出来了不等于送达了"(单子 §9.1)。
                        ledger().RestoreDrainedInbox(task, drained);
                    };
                    return batch;
                }
                if (sealed) {
                    return std::nullopt;  // 没信也没活孩子:封账收口
                }
                // 任务 turn 预算已尽(turn 预算单 §7.4):不进 WaitingChildren
                // 死等一个注定无法吸收的孩子结果——按封账返回,收树与终态
                // 归 RunTask 收尾块(补判旗 + 取消块)。
                if (ledger().ModelTurnSnapshot(task).Exhausted()) {
                    return std::nullopt;
                }
                // WaitingChildren:面板明写"等 N 只子任务",醒来再查一遍。
                ledger().SetLiveTaskState(task, AgentTaskState::WaitingChildren);
                ledger().RecordStage(task, agent::AgentSupervisionStage::WaitingChildren);
                ledger().WaitForKeyChange(task);
                ledger().SetLiveTaskState(task, AgentTaskState::Running);
                ledger().RecordStage(task, agent::AgentSupervisionStage::Preparing);
                if (task->cancel.load(std::memory_order_acquire) || task->force_finalized) {
                    // 取消/强收唤醒:不再续投,交 harness 按原因收账。
                    return std::nullopt;
                }
            }
        };
    }
    // 首轮 user message 只放父代理写下的派工原文。cwd、角色与隔离规则归
    // system 运行环境；两层各守本分，宿主不包信封、不重排正文。
    api::Message initial_input;
    initial_input.role = api::Role::User;
    initial_input.content.push_back(api::TextBlock{prompt});
    // P0-2 轨迹:子账开卷(turn.started + input.received 先于首请求)。
    if (trajectory != nullptr) {
        trajectory->turn_bridge().BeginTurn("turn-1", "external_user");
        trajectory->turn_bridge().RecordInput(initial_input);
    }
    agent::DriveReport drive = agent::DriveTurn(sub_agent, turn_wiring, std::move(initial_input), drive_options);
    // 取消链收口(合流前的次序:join 在 Stop 续跑环之前;合并旗 Stop 时
    // 置真,续跑轮拿到即收——与旧行为一致)。
    cancel_chain.Stop();
    // 边界耗尽的补判(turn 预算单 §7.4):续投源没领到批、任务 turn 账却
    // 已尽、活孩子还在——continuation 那头不进 WaitingChildren 死等
    //(见上),这里把旗补上,好让分型按 TurnLimitExhausted 收口、下面的
    // 取消块去收树。mailbox 恰好封账、孩子清零的自然完成不进这半截——
    // 末枚额度上交出结论不算预算耗尽。
    if (task != nullptr && !drive.hit_turn_limit && ledger().ModelTurnSnapshot(task).Exhausted() &&
        ledger().AliveChildCount(task->snapshot.id) > 0) {
        drive.hit_turn_limit = true;
    }
    // Completing(P0-4 状态机):模型已交最终文本、活孩子清零,正在收口——
    // 面板据此从"运行中/等子任务"翻成"收口中";终态由 Finalize 落。
    if (task != nullptr) {
        ledger().SetLiveTaskState(task, AgentTaskState::Completing);
        ledger().RecordStage(task, agent::AgentSupervisionStage::Completing);
    }

    // ---- 父任务 turn 预算尽时的收树(turn 预算单 §7.4)--------------------
    // 硬线先到,父没有资格再花模型 turn 整合:向下取消 attached children
    // (各发停止信号,收尾按 ParentCancelled 分型),不再死等一个注定无法
    // 吸收的结果;已完成但未送达的 child outcome 留进未送达清单(收场报告
    // 照列,UndeliveredInboxNote 兜底),不 reparent、不越级交 main 冒充父
    // 已整合。与递归派工单"父不得甩手先走"共同裁决:预算硬线先到时收树
    // 并如实交部分结果。非 turn 耗尽的收场不走这半截(老行为)。
    if (task != nullptr && drive.hit_turn_limit) {
        for (const int child_id : ledger().ChildTaskIds(task->snapshot.id)) {
            ledger().CancelTask(child_id);
        }
    }

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
                std::lock_guard<std::mutex> lock(ledger().mutex);
                task->snapshot.steps_used += continuation.steps_used;
                ledger().Touch();
            }
        };
        // 续跑轮的 turn 账投影(与 DriveOptions 同一口径):真账共一本,
        // DriveReport 刷新用。
        if (task != nullptr) {
            stop_options.turn_budget_snapshot = [this, task]() { return ledger().ModelTurnSnapshot(task); };
        }
        if (stop_hooks_on_foreground) {
            const lubancode::hooks::HookContext sub_context = sub_hook_dispatcher->context();
            stop_options.emit = [this, task, sub_hook_dispatcher, sub_context](bool stop_hook_active,
                                                                               const std::string& last_text) {
                lubancode::hooks::HookPayload stop;
                stop.event = lubancode::hooks::HookEvent::SubagentStop;
                stop.fields["agent_id"] = sub_context.agent_id.value_or(std::string());
                stop.fields["agent_type"] = sub_context.agent_type.value_or(std::string());
                stop.fields["agent_transcript_path"] = std::string();  // 子代理历史不落独立文件,如实留空
                stop.fields["last_assistant_message"] = last_text;
                stop.fields["stop_hook_active"] = stop_hook_active;
                // 任务级 turn 账(§11.2):Stop 时从唯一真账现读——attempted 是
                // 消费数,completed 另列;额度尽时给 budget_exhausted_reason。
                // 只读投影,hook 不能回写。
                if (task != nullptr) {
                    const agent::ModelTurnBudgetSnapshot turns = ledger().ModelTurnSnapshot(task);
                    stop.fields["turn_limit"] = turns.limit;
                    stop.fields["turns_attempted"] = turns.attempted;
                    stop.fields["turns_completed"] = turns.completed;
                    if (turns.Exhausted()) {
                        stop.fields["budget_exhausted_reason"] = "turn_limit";
                    }
                }
                return sub_hook_dispatcher->EmitWith(lubancode::hooks::HookEvent::SubagentStop, stop, sub_context);
            };
        } else {
            stop_options.emit = [this, task, background_hooks](bool stop_hook_active, const std::string& last_text) {
                lubancode::hooks::HookPayload stop;
                stop.event = lubancode::hooks::HookEvent::SubagentStop;
                stop.fields["agent_id"] = background_hooks->context().agent_id.value_or(std::string());
                stop.fields["agent_type"] = background_hooks->context().agent_type.value_or(std::string());
                stop.fields["agent_transcript_path"] = std::string();
                stop.fields["last_assistant_message"] = last_text;
                stop.fields["stop_hook_active"] = stop_hook_active;
                if (task != nullptr) {
                    const agent::ModelTurnBudgetSnapshot turns = ledger().ModelTurnSnapshot(task);
                    stop.fields["turn_limit"] = turns.limit;
                    stop.fields["turns_attempted"] = turns.attempted;
                    stop.fields["turns_completed"] = turns.completed;
                    if (turns.Exhausted()) {
                        stop.fields["budget_exhausted_reason"] = "turn_limit";
                    }
                }
                return background_hooks->Emit(lubancode::hooks::HookEvent::SubagentStop, stop);
            };
        }
        // 续跑轮的步数/预算/打断账由 RunStopContinuation 直接并进 drive
        //(harness 只并增量,主账不重算)。
        agent::RunStopContinuation(sub_agent, turn_wiring, stop_options, drive);
        if (task != nullptr) {
            std::lock_guard<std::mutex> lock(ledger().mutex);
            task->snapshot.steps_used = drive.steps_used;
            ledger().Touch();
        }
    }

    // P0-2 轨迹:子账收口——turn 终态、run 终态、关柄(§8.3);前台任务
    // 把子账 terminal hash 回填父桥(父侧 agent 调用的执行终态引用它,
    // §3.5)。后台任务的 hash 留在账本注册表,P0-3 的 session verifier
    // 跨文件核对。
    if (trajectory != nullptr) {
        const bool ok = drive.ok;
        const bool cancelled = drive.cancelled;
        trajectory->turn_bridge().EndTurn(ok, cancelled, drive.ok ? std::string() : drive.error);
        const std::string terminal_hash =
            trajectory->Finish(ok, ok ? "done" : (cancelled ? "cancelled" : "failed"));
        if (foreground_hooks != nullptr && foreground_hooks->trajectory_child_finished) {
            foreground_hooks->trajectory_child_finished(trajectory->run_id(), terminal_hash);
        }
    }

    // ---- 收场分型(批三:harness 的 ClassifyTurnEnd,一份)----------------
    TaskOutcome task_outcome;
    task_outcome.step_limit = budget.max_steps_per_turn;
    task_outcome.steps_used = drive.steps_used;
    task_outcome.stop_reason = drive.stop_reason;
    task_outcome.wall_limit_secs = budget.max_wall_secs;
    task_outcome.token_limit = budget.max_total_tokens;
    // 任务级 turn 账(turn 预算单 §8.2):从唯一真账(任务记录的
    // turn_account)收口投影,attempted/completed 分账;与 legacy 的
    // steps_used/step_limit 并存但不混写成同一根"生效硬线"。
    if (task != nullptr) {
        const agent::ModelTurnBudgetSnapshot turns = ledger().ModelTurnSnapshot(task);
        task_outcome.turn_limit = turns.limit;
        task_outcome.turns_reserved = turns.reserved;
        task_outcome.turns_attempted = turns.attempted;
        task_outcome.turns_completed = turns.completed;
    }
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
        std::lock_guard<std::mutex> lock(ledger().mutex);
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
    // 任务级 turn 闸(turn 预算单):初始/续投/Stop 钩子合计的那本账尽了,
    // 分型按 TurnLimitExhausted 收口,带部分结果走。
    endgame.turn_budget_exhausted = drive.hit_turn_limit;
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
        case agent::TurnVerdict::Reason::TurnLimit:
            // 任务级 turn 预算用满(turn 预算单 §8.1/§8.3):整项任务的逻辑
            // 模型请求总数到帽(初始/续投/孩子回流/Stop 钩子共那本账)。
            // 部分结果照常带走;另有未送达的父消息/孩子结果由
            // UndeliveredInboxNote 在收尾账注里列明,不冒充已整合。
            task_outcome.status = TaskOutcomeStatus::BudgetExhausted;
            task_outcome.reason = TaskOutcomeReason::TurnLimitExhausted;
            task_outcome.message = "模型 turn 预算已用满(" + std::to_string(task_outcome.turns_attempted) +
                                   "/" + std::to_string(task_outcome.turn_limit) + " 次请求,完整返回 " +
                                   std::to_string(task_outcome.turns_completed) + " 次)";
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
    // 空转收口改判(监督器单 P0-2):监督器发的停止信号(no_progress_fired)
    // 不是用户的手,UserStop 不许冒名——改按 NoMeaningfulProgress 分型,
    // 检查点/部分结果照常带走(现场不丢的红线)。
    if (task != nullptr && task->no_progress_fired.load(std::memory_order_acquire) &&
        task_outcome.reason == TaskOutcomeReason::UserStop) {
        task_outcome.status = TaskOutcomeStatus::Failed;
        task_outcome.reason = TaskOutcomeReason::NoMeaningfulProgress;
        task_outcome.message =
            "连续多个完整轮次无任何可验证进展(指纹不变),宿主提醒后仍无进展,按空转收口";
        task_outcome.partial_result = partial;
        run_result = {"子代理空转收口: " + task_outcome.message + "\n" + ComposeOutcomeText(task_outcome), true};
    }
    if (task != nullptr) {
        std::lock_guard<std::mutex> lock(ledger().mutex);
        // 看门狗已强制收账(任务线程绝境下晚归):台账保持强制收账那份,
        // 这里只补一条"晚归"事件留痕,不再翻状态/结果。
        if (task->force_finalized) {
            AgentTaskEvent late_event;
            late_event.kind = AgentTaskEventKind::Failure;
            late_event.text = lubancode::cli::tr("agent_outcome.wall_clock_late");
            ledger().AppendEventLocked(task, std::move(late_event));
            return run_result;
        }
        // 消息账收口:残余正文先封卷,再记终局事件——completion 带最终结论
        // 全文,failure 带短因与部分结果(规格"现场三"事件表)。
        ledger().FlushPendingTextLocked(task);
        AgentTaskEvent final_event;
        if (task_outcome.status == TaskOutcomeStatus::Completed) {
            final_event.kind = AgentTaskEventKind::Completion;
            final_event.text = text;
        } else {
            final_event.kind = AgentTaskEventKind::Failure;
            final_event.text =
                task_outcome.message + (partial.empty() ? std::string() : "\n" + partial);
        }
        ledger().AppendEventLocked(task, std::move(final_event));
        task->snapshot.outcome = std::move(task_outcome);
        task->activity = AgentTaskActivity{};  // 终态不再带阶段文案(活度账清空)
    }
    return run_result;
}

// 同级派工的薄壳(P0-3 重写,见 agent_tool.hpp 的 AgentDispatchTool 注释)。
AgentDispatchTool::AgentDispatchTool(AgentTool& target)
    : handle_(target.main_dispatch_handle()) {}

std::string AgentDispatchTool::name() const { return "agent"; }
std::string AgentDispatchTool::description() const {
    // schema/描述与主路同源(协调器上的门面指针只读转发);门面不在
    //(协调器亡)退一句静态说明——壳挂着的注册表此刻也活到头了。
    if (const Tool* facade = handle_.facade_tool(); facade != nullptr) {
        return facade->description();
    }
    return "把独立任务委托给子代理。";
}
nlohmann::json AgentDispatchTool::input_schema() const {
    const Tool* facade = handle_.facade_tool();
    if (facade == nullptr) {
        return nlohmann::json::object();
    }
    nlohmann::json schema = facade->input_schema();
    // 按当前入口修后台可见性(派工单 §二):嵌套壳的环境没有后台工厂时,
    // background 从枚举摘掉——模型看得见的选项与 preflight/执行口同一本账。
    // main 那枚壳(env 为空)不修,schema 与门面逐字节一致(旧测试钉的形状)。
    const std::shared_ptr<const SubagentDispatchEnv>& env = handle_.env();
    const AgentTool* agent_facade = dynamic_cast<const AgentTool*>(facade);
    if (env != nullptr && agent_facade != nullptr && !agent_facade->BackgroundBackendAvailable(env)) {
        DropBackgroundFromSchema(schema);
    }
    return schema;
}
tools::Tool::Result AgentDispatchTool::execute(const nlohmann::json& input) { return handle_.Dispatch(input); }
// 取消旗透传:壳不许洗 context(AgentTool 侧另有自己的 CancelChain,外层
// 旗照旧经 Hooks.cancel 汇进去,这里只是不让链路在壳上断)。
tools::Tool::Result AgentDispatchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    // 取消链在子代理侧自成一体(CancelChain 并根),外层旗不另开旁路;
    // 与旧转发壳同款语义:调用直通。
    (void)context;
    return handle_.Dispatch(input);
}

}  // namespace lubancode::tools
