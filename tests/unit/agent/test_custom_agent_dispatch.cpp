// 自定义 Agent 与 Prompt Profile 单·阶段 4(AgentTool 接入收尾):
//   - Catalog 派发校验:写死的两枚内置名白名单换净——查得到即可派,查不到
//     报"没有这名,看 /agents";码内内置两枚(builtin 记号)走内置快路,
//     user/project 层覆盖内置名的按定义走自定义路(契约 §6.2 跨层覆盖);
//   - 动态 schema:agent_type 说明列"当前可派的类型"(名字+一句描述),
//     清单缓存一回合一翻新,没配清单源零追加(schema 与从前零 diff);
//   - 自定义 Agent 四件对账:后台派出、x 取消、agent_message 送达、结果
//     回收——与内置路逐件同款;工具面与说明面(阶段 2 Capabilities)在
//     后台路同样收窄;
//   - worktree isolation:YAML 缺省与入参显式都真开房,写不出房,预算断
//     不丢房(0.26.83 先例的自定义 Agent 变体);
//   - 权限收窄执法:父 yolo 子 confirm 时,子代理循环里真把确认拉回
//     (Resolver 已校验"不许放宽",这半截是"收窄生效")。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_profile_resolver.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "platform/process.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(test_agent_budget.cpp 同款手艺)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, tools::Tool::Result result, bool confirm = false)
        : name_(std::move(name)), result_(std::move(result)), confirm_(confirm) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return confirm_; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return result_;
    }

    int call_count = 0;

private:
    std::string name_;
    tools::Tool::Result result_;
    bool confirm_;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 可阻塞的后台假后端(test_agent_inbox.cpp 的 GateBackend 同款):第一请求
// 挂住等闸,放闸后吐一次 tool_use;第二请求吐文本结论。
struct GateBackendState {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool release = false;
    std::vector<api::Request> captured;
};

class GateBackend : public api::Backend {
public:
    explicit GateBackend(std::shared_ptr<GateBackendState> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->captured.push_back(request);
            state_->started = true;
            state_->cv.notify_all();
            const bool released = state_->cv.wait_for(lock, std::chrono::seconds(5), [&]() {
                return state_->release || (cancel != nullptr && cancel->load(std::memory_order_acquire));
            });
            if (!released) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "gate timeout", 0});
            }
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
        }
        std::size_t call_index;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            call_index = state_->captured.size() - 1;
        }
        const std::vector<api::StreamEvent> events =
            call_index == 0 ? ToolUseScript("t1", "probe") : TextScript("结论已就绪");
        for (const auto& event : events) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<GateBackendState> state_;
};

void WaitStarted(const std::shared_ptr<GateBackendState>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::seconds(2), [&]() { return state->started; });
}

void OpenGate(const std::shared_ptr<GateBackendState>& state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->cv.notify_all();
}

void WaitIdle(tools::AgentTool& agent_tool) {
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 一份可派的自定义 Agent 材料(名字/描述/白名单可按用例覆写)。
tools::CustomAgentMaterial ReviewerMaterial() {
    tools::CustomAgentMaterial material;
    material.definition.name = "library-reviewer";
    material.definition.description = "审查图书馆项目的代码与测试。";
    material.definition.tools.allow = {"read_file", "search"};
    return material;
}

// Catalog 式解析口:认一名"library-reviewer"(自定义),码内两枚置 builtin。
std::function<std::optional<tools::CustomAgentMaterial>(const std::string&)> CatalogResolver(
    tools::CustomAgentMaterial custom) {
    return [custom = std::move(custom)](
               const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
        if (name == custom.definition.name) {
            return custom;
        }
        if (name == "general-purpose" || name == "Explore") {
            tools::CustomAgentMaterial builtin;
            builtin.definition.name = name;
            builtin.builtin = true;
            return builtin;
        }
        return std::nullopt;
    };
}

bool RequestOffersTool(const api::Request& request, const std::string& tool_name) {
    for (const auto& tool : request.tools) {
        if (tool.name == tool_name) {
            return true;
        }
    }
    return false;
}

// 某次请求里全部工具结果块的拼接(拒绝回执对账用)。
std::string DumpToolResults(const api::Request& request) {
    std::string out;
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                out += result->content + "\n";
            }
        }
    }
    return out;
}

// 真 git 临时仓库(test_agent_budget.cpp 同款,isolation 测试的基准)。
struct GitRepo {
    std::filesystem::path root;

    GitRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_customagent_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "repo";
        std::filesystem::create_directories(root);
        RunGit({"init", "-q", "-b", "main"});
        RunGit({"config", "user.email", "test@example.com"});
        RunGit({"config", "user.name", "Test"});
        std::ofstream(root / "seed.txt") << "seed\n";
        RunGit({"add", "."});
        RunGit({"commit", "-q", "-m", "init"});
    }
    ~GitRepo() {
        std::error_code ec;
        std::filesystem::remove_all(root.parent_path(), ec);
    }

    platform::ProcessResult RunGit(std::vector<std::string> args) const {
        std::vector<std::string> argv = {"git", "-C", PathToUtf8(root)};
        argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
        return platform::RunProcess(argv, 60000);
    }

    static std::string PathToUtf8(const std::filesystem::path& path) {
        const std::u8string u8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

    std::vector<std::filesystem::path> AgentRooms() const {
        std::vector<std::filesystem::path> rooms;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root / ".lubancode" / "worktrees", ec)) {
            rooms.push_back(entry.path());
        }
        return rooms;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 阶段 4·任务一:Catalog 派发校验换净
// ---------------------------------------------------------------------------

TEST_CASE("Catalog 派发校验:查得到即可派,查不到报'没有这名,看 /agents'") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论:后台无名排查")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetCustomAgentResolver(CatalogResolver(ReviewerMaterial()));

    // 查不到:新口径报"没有这名",指路 /agents 与 doctor;不发请求。
    nlohmann::json unknown;
    unknown["title"] = "无名任务";
    unknown["prompt"] = "查";
    unknown["agent_type"] = "no-such-agent";
    const tools::Tool::Result unknown_result = agent_tool.execute(unknown);
    CHECK(unknown_result.is_error);
    CHECK(unknown_result.content.find("没有名叫 \"no-such-agent\"") != std::string::npos);
    CHECK(unknown_result.content.find("/agents") != std::string::npos);
    CHECK(unknown_result.content.find("/agent doctor") != std::string::npos);
    CHECK(backend.captured_requests.empty());

    // 查得到(自定义):照派。
    nlohmann::json input;
    input["title"] = "审查任务";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("后台无名排查") != std::string::npos);
}

TEST_CASE("Catalog 派发校验:码内内置两枚带 builtin 记号,走内置快路一字不动") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论:照旧")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"写了", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetCustomAgentResolver(CatalogResolver(ReviewerMaterial()));

    // general-purpose:Catalog 查到码内定义(builtin),走内置快路——生成
    // persona 查表、全工具面(不按 allow 裁,内置定义本就没有 allow)。
    nlohmann::json input;
    input["title"] = "默认任务";
    input["prompt"] = "查";
    input["agent_type"] = "general-purpose";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("你是 general-purpose 子代理") != std::string::npos);
    CHECK(RequestOffersTool(backend.captured_requests[0], "write_file"));  // 全工具面

    // Explore:同样走内置快路(explore 人格照旧;worktree 拒绝口径照旧)。
    backend.captured_requests.clear();
    backend.scripts = {TextScript("结论:只读排查")};
    nlohmann::json explore_input;
    explore_input["title"] = "只读任务";
    explore_input["prompt"] = "查";
    explore_input["agent_type"] = "Explore";
    const tools::Tool::Result explore_result = agent_tool.execute(explore_input);
    CHECK_FALSE(explore_result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("只读") != std::string::npos);

    nlohmann::json worktree_input = explore_input;
    worktree_input["isolation"] = "worktree";
    const tools::Tool::Result worktree_result = agent_tool.execute(worktree_input);
    CHECK(worktree_result.is_error);
    CHECK(worktree_result.content.find("用不上 worktree 隔离") != std::string::npos);
}

TEST_CASE("Catalog 派发校验:user/project 层覆盖内置名,按定义走自定义路") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论:按覆盖定义跑")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"写了", false}));

    // 项目层放了一份 general-purpose.yaml:改了描述、裁了工具面(builtin=
    // false)。派 "general-purpose" 应按这份定义走,不再吃码内快路。
    tools::CustomAgentMaterial override_material;
    override_material.definition.name = "general-purpose";
    override_material.definition.description = "项目层收紧过的默认子代理:只许读与搜。";
    override_material.definition.tools.allow = {"read_file", "search"};

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(override_material)));

    nlohmann::json input;
    input["title"] = "覆盖任务";
    input["prompt"] = "查";
    input["agent_type"] = "general-purpose";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    const api::Request& request = backend.captured_requests[0];
    CHECK(request.system.find("项目层收紧过的默认子代理") != std::string::npos);  // 覆盖描述进 persona
    CHECK(RequestOffersTool(request, "read_file"));
    CHECK_FALSE(RequestOffersTool(request, "write_file"));  // 覆盖 allow 生效
}

// ---------------------------------------------------------------------------
// 阶段 4·任务二:动态 schema(agent_type 说明列当前可派的类型)
// ---------------------------------------------------------------------------

TEST_CASE("动态 schema:类型清单进 agent_type 说明;没配清单源零追加") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    const tools::AgentTool plain_tool(backend, sub_registry, "/work/dir");

    // 没配清单源:说明与静态文案一致,一个字不追加(零 diff 的那条底线)。
    const nlohmann::json plain_schema = plain_tool.input_schema();
    const std::string plain_description = plain_schema["properties"]["agent_type"]["description"];
    CHECK(plain_description.find("当前可派的类型") == std::string::npos);

    // 配了清单源:内置两枚 + 自定义都列,各带一句描述;换行压平。
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    int provider_calls = 0;
    agent_tool.SetAgentTypesProvider([&provider_calls]() -> std::vector<tools::AgentTypeInfo> {
        ++provider_calls;
        return {
            {"Explore", "快速搜索、阅读并分析代码库的只读代理。"},
            {"general-purpose", "搜索、分析并完成多步任务;默认子代理类型。"},
            {"library-reviewer", "审查图书馆项目的代码与测试。\n第二行也会被压平成一句。"},
        };
    });
    const nlohmann::json schema = agent_tool.input_schema();
    const std::string description = schema["properties"]["agent_type"]["description"];
    CHECK(description.find("当前可派的类型") != std::string::npos);
    CHECK(description.find("- Explore: 快速搜索、阅读并分析代码库的只读代理。") != std::string::npos);
    CHECK(description.find("- general-purpose: 搜索、分析并完成多步任务;默认子代理类型。") !=
          std::string::npos);
    CHECK(description.find("- library-reviewer: 审查图书馆项目的代码与测试。 第二行也会被压平成一句。") !=
          std::string::npos);
    CHECK(description.find('\n') != std::string::npos);  // 多行清单,不是一坨

    // 性能口径:清单读缓存——连构造多回 schema,清单源只被调一次。
    for (int i = 0; i < 5; ++i) {
        (void)agent_tool.input_schema();
    }
    CHECK(provider_calls == 1);

    // 回合边界(SetHooks)翻新:下一回 schema 重新拉清单。
    agent_tool.SetHooks(tools::AgentTool::Hooks{});
    (void)agent_tool.input_schema();
    CHECK(provider_calls == 2);
}

// ---------------------------------------------------------------------------
// 阶段 4·任务三:自定义 Agent 的后台/取消/消息/结果回收(逐件对账)
// ---------------------------------------------------------------------------

TEST_CASE("自定义 Agent 后台路:派出、工具面与说明面收窄、预装技能、结果回收") {
    FakeBackend foreground_backend;  // 前台路一枚请求都不该发
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"写了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("run_command", tools::Tool::Result{"跑完了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("todo_write", tools::Tool::Result{"记了", false}));

    auto state = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(state);
        return detached;
    });

    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.skills_preload = {"library-domain"};
    material.preloaded_skills = {"领域约束:借阅上限 5 本。"};
    agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));

    nlohmann::json input;
    input["title"] = "后台审查";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    input["execution_mode"] = "background";
    const tools::Tool::Result launch = agent_tool.execute(input);
    CHECK_FALSE(launch.is_error);
    CHECK(launch.content.find("library-reviewer") != std::string::npos);  // 启动回执带身份

    WaitStarted(state);
    OpenGate(state);
    WaitIdle(agent_tool);
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 台账:身份是 resolved name,状态完成。
    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].agent_type == "library-reviewer");
    CHECK(summaries[0].state == tools::AgentTaskState::Done);

    // 后台请求:工具面按 allow 收窄;说明面(Capabilities)同步收窄——
    // 裁掉的工具不进 tools 数组,也不配 feature 文案;预装技能正文进提示。
    std::vector<api::Request> captured;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        captured = state->captured;
    }
    REQUIRE(captured.size() == 2);
    const api::Request& first = captured[0];
    CHECK(RequestOffersTool(first, "read_file"));
    CHECK(RequestOffersTool(first, "search"));
    CHECK_FALSE(RequestOffersTool(first, "write_file"));
    CHECK_FALSE(RequestOffersTool(first, "run_command"));
    CHECK_FALSE(RequestOffersTool(first, "todo_write"));
    CHECK(first.system.find("你是 library-reviewer 子代理") != std::string::npos);
    CHECK(first.system.find("审查图书馆项目的代码与测试") != std::string::npos);
    CHECK(first.system.find("预装技能 library-domain") != std::string::npos);
    CHECK(first.system.find("领域约束:借阅上限 5 本") != std::string::npos);
    CHECK(first.system.find("读文件用 read_file") != std::string::npos);            // files 在
    CHECK(first.system.find("跑命令用 run_command") == std::string::npos);          // shell 裁了
    CHECK(first.system.find("先调用 todo_write 列一份清单") == std::string::npos);  // todo 裁了

    // 结果回收:完成通知可排走,正文带结论;排走后 delivered 置位。
    CHECK(agent_tool.HasUndeliveredCompletions());
    const std::string notice = agent_tool.DrainCompletionNotices();
    CHECK(notice.find("library-reviewer") != std::string::npos);
    CHECK(notice.find("结论已就绪") != std::string::npos);
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());
    CHECK(foreground_backend.captured_requests.empty());  // 后台路不借前台 backend
}

TEST_CASE("自定义 Agent 取消:面板 x(CancelTask)贯通到后台任务线程") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));

    auto state = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(state);
        return detached;
    });
    agent_tool.SetCustomAgentResolver(CatalogResolver(ReviewerMaterial()));

    nlohmann::json input;
    input["title"] = "取消审查";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    input["execution_mode"] = "background";
    CHECK_FALSE(agent_tool.execute(input).is_error);
    WaitStarted(state);

    CHECK(agent_tool.CancelTask(1));  // x:只此一只,取消旗进任务线程
    OpenGate(state);
    WaitIdle(agent_tool);

    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].state == tools::AgentTaskState::Cancelled);
}

TEST_CASE("自定义 Agent 消息:agent_message 排进 inbox,工具收尾后的下一请求送达") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));
    sub_registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{"probe ok", false}));

    auto state = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(state);
        return detached;
    });
    // probe 也要在 allow 里,工具边界内的介入路径才走得通。
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.tools.allow = {"read_file", "search", "probe"};
    agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));

    nlohmann::json input;
    input["title"] = "传话审查";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    input["execution_mode"] = "background";
    CHECK_FALSE(agent_tool.execute(input).is_error);
    WaitStarted(state);

    CHECK(agent_tool.SendTaskMessage(1, "只读,不要修改") == tools::TaskMessageStatus::Queued);
    CHECK(agent_tool.PendingTaskMessages(1).size() == 1);
    OpenGate(state);
    WaitIdle(agent_tool);

    std::vector<api::Request> captured;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        captured = state->captured;
    }
    REQUIRE(captured.size() == 2);
    std::string second_dump;
    for (const auto& message : captured[1].messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                second_dump += text->text + "\n";
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                second_dump += result->content + "\n";
            }
        }
    }
    CHECK(second_dump.find("只读,不要修改") != std::string::npos);       // 送达
    CHECK(second_dump.find("[主会话用户介入]") != std::string::npos);  // 按介入记
    CHECK(agent_tool.PendingTaskMessages(1).empty());                  // 全送完
}

// ---------------------------------------------------------------------------
// 阶段 4·任务四:自定义 Agent 的 worktree isolation
// ---------------------------------------------------------------------------

TEST_CASE("自定义 Agent worktree:YAML 缺省与入参显式都真开房,写不出房,预算断不丢房") {
    GitRepo repo;
    FakeBackend backend;
    // 第一步写文件进房,后续死循环调写——步数硬线 2 断在半路。
    backend.scripts = {
        {api::MessageStart{"msg", "model"},
         api::ToolUseStart{0, "t1", "write_file"},
         api::ToolUseInputDelta{0, "{\"path\":\"out.txt\",\"content\":\"hello\"}"},
         api::ContentBlockDone{0},
         api::MessageDone{"tool_use", api::Usage{100, 10, 0, 0}}},
        ToolUseScript("t2", "write_file"),
        ToolUseScript("t3", "write_file"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<tools::WriteFileTool>());
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));

    // YAML runtime.isolation: worktree——入参不传 isolation 也该真开房。
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.tools.allow = {"write_file", "search"};
    material.definition.isolation = "worktree";
    material.definition.max_steps_per_turn = 2;

    tools::AgentTool agent_tool(backend, sub_registry, GitRepo::PathToUtf8(repo.root));
    agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));

    nlohmann::json input;
    input["title"] = "隔房任务";
    input["prompt"] = "写";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);

    // 预算断了(YAML 的 2 步):缘由明说;房因有活保留,产物在房里。
    CHECK(result.is_error);
    CHECK(result.content.find("2/2 步") != std::string::npos);
    CHECK(result.content.find("已保留") != std::string::npos);
    const auto rooms = repo.AgentRooms();
    REQUIRE(rooms.size() == 1);
    CHECK(std::filesystem::exists(rooms[0] / "out.txt"));           // 写进了房
    CHECK_FALSE(std::filesystem::exists(repo.root / "out.txt"));    // 主 checkout 没碰
    CHECK(result.content.find(GitRepo::PathToUtf8(rooms[0].filename())) != std::string::npos);

    // 入参显式 isolation 压过 YAML 同值;显式 none 也压得过去(缺省档语义)。
    FakeBackend backend2;
    backend2.scripts = {TextScript("结论:不隔离")};
    tools::AgentTool agent_tool2(backend2, sub_registry, GitRepo::PathToUtf8(repo.root));
    tools::CustomAgentMaterial material2 = ReviewerMaterial();
    material2.definition.tools.allow = {"write_file", "search"};
    material2.definition.isolation = "worktree";
    agent_tool2.SetCustomAgentResolver(CatalogResolver(std::move(material2)));
    nlohmann::json input2;
    input2["title"] = "不隔任务";
    input2["prompt"] = "查";
    input2["agent_type"] = "library-reviewer";
    input2["isolation"] = "none";
    const tools::Tool::Result result2 = agent_tool2.execute(input2);
    CHECK_FALSE(result2.is_error);
    const auto rooms_after = repo.AgentRooms();
    CHECK(rooms_after.size() == 1);  // 没开新房
}

// ---------------------------------------------------------------------------
// 阶段 4·任务五:权限收窄执法(父 yolo 子 confirm,确认真拉回)
// ---------------------------------------------------------------------------

TEST_CASE("权限收窄执法:父 yolo 子 confirm,needs_confirm 的工具走带下限的确认口") {
    FakeBackend backend;
    // 第一步调需确认的工具,第二步给结论(被拒也照常收场)。
    backend.scripts = {ToolUseScript("t1", "sensitive"), TextScript("结论:受阻如实报")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    auto sensitive = std::make_unique<FakeTool>("sensitive", tools::Tool::Result{"动了", false},
                                                /*confirm=*/true);
    FakeTool* sensitive_ptr = sensitive.get();
    sub_registry.Register(std::move(sensitive));

    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.tools.allow = {"read_file", "sensitive"};
    material.definition.permissions_mode = "confirm";

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetResolveEnvironment([]() -> agent::AgentProfileResolveEnvironment {
        agent::AgentProfileResolveEnvironment env;
        env.parent_permission = agent::AgentPermissionMode::Yolo;  // 父会话开着 yolo
        return env;
    });
    agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));

    int floored_calls = 0;
    agent::AgentPermissionMode seen_floor = agent::AgentPermissionMode::Yolo;
    int plain_calls = 0;
    tools::AgentTool::Hooks hooks;
    hooks.on_tool_confirm = [&plain_calls](const std::string&, const std::string&,
                                           const nlohmann::json&) { ++plain_calls; return true; };
    hooks.on_tool_confirm_floored =
        [&floored_calls, &seen_floor](const std::string&, const std::string& name, const nlohmann::json&,
                                      agent::AgentPermissionMode floor) {
            ++floored_calls;
            seen_floor = floor;
            CHECK(name == "sensitive");
            return false;  // 宿主按 min(会话,下限) 裁定后拒了
        };
    agent_tool.SetHooks(std::move(hooks));

    nlohmann::json input;
    input["title"] = "越线任务";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);

    // 确认真拉回了:走的是带下限的口,下限是定义声明的 confirm;免问不再免。
    CHECK(floored_calls == 1);
    CHECK(seen_floor == agent::AgentPermissionMode::Default);
    CHECK(plain_calls == 0);                 // 旧口没被走到
    CHECK(sensitive_ptr->call_count == 0);   // 拒了:工具没执行
    REQUIRE(backend.captured_requests.size() == 2);
    CHECK(DumpToolResults(backend.captured_requests[1]).find("拒绝") !=
          std::string::npos);  // 模型看到如实的拒绝回执
}

TEST_CASE("权限求交执法:自定义 Agent 总携带有效档;父档没账时按保守默认") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>(
        "sensitive", tools::Tool::Result{"动了", false}, /*confirm=*/true));

    // inherit(=同父 yolo):不比父严,确认走旧口原样转发。
    {
        FakeBackend backend;
        backend.scripts = {ToolUseScript("t1", "sensitive"), TextScript("结论:照常")};
        tools::CustomAgentMaterial material = ReviewerMaterial();
        material.definition.tools.allow = {"read_file", "sensitive"};  // permissions_mode 空 = inherit
        tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
        agent_tool.SetResolveEnvironment([]() -> agent::AgentProfileResolveEnvironment {
            agent::AgentProfileResolveEnvironment env;
            env.parent_permission = agent::AgentPermissionMode::Yolo;
            return env;
        });
        agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));
        int floored_calls = 0;
        int plain_calls = 0;
        tools::AgentTool::Hooks hooks;
        hooks.on_tool_confirm = [&plain_calls](const std::string&, const std::string&,
                                               const nlohmann::json&) { ++plain_calls; return true; };
        hooks.on_tool_confirm_floored =
            [&floored_calls](const std::string&, const std::string&, const nlohmann::json&,
                             agent::AgentPermissionMode) { ++floored_calls; return true; };
        agent_tool.SetHooks(std::move(hooks));
        nlohmann::json input;
        input["title"] = "同档任务";
        input["prompt"] = "审";
        input["agent_type"] = "library-reviewer";
        CHECK_FALSE(agent_tool.execute(input).is_error);
        CHECK(floored_calls == 1);
        CHECK(plain_calls == 0);
    }
    // 没递环境账(旧调用方):"没账可查"跳过执法,确认照旧口。
    {
        FakeBackend backend;
        backend.scripts = {ToolUseScript("t1", "sensitive"), TextScript("结论:照常")};
        tools::CustomAgentMaterial material = ReviewerMaterial();
        material.definition.tools.allow = {"read_file", "sensitive"};
        material.definition.permissions_mode = "confirm";
        tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
        agent_tool.SetCustomAgentResolver(CatalogResolver(std::move(material)));
        int plain_calls = 0;
        tools::AgentTool::Hooks hooks;
        hooks.on_tool_confirm = [&plain_calls](const std::string&, const std::string&,
                                               const nlohmann::json&) { ++plain_calls; return true; };
        agent_tool.SetHooks(std::move(hooks));
        nlohmann::json input;
        input["title"] = "无账任务";
        input["prompt"] = "审";
        input["agent_type"] = "library-reviewer";
        CHECK_FALSE(agent_tool.execute(input).is_error);
        CHECK(plain_calls == 1);
    }
}
