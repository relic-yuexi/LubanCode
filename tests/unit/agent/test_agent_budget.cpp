// 子代理成本刹车与自定义 Agent 身份(真机实测 P2-1/P2-2/P2-6):
//   - 纯函数:BudgetSoftLine/CrossesBudgetSoftLine 的软线派生与三线判定;
//   - 软线催办:token 硬线跨过 80% 先注入"请基于现有证据收尾",硬线断了
//     才收场,部分结果与缘由一并带回,不静默丢;
//   - 自定义 Agent:身份按 resolved name 记(Dock 不再冒名 Explore)、
//     tools.allow 收窄工具面、skills.preload 预装进系统提示、
//     runtime.max_steps_per_turn 真能落到派出预算;
//   - 超预算不丢工作树:isolation=worktree 的任务撞硬线,房与产物保留。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/runtime_profile.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "platform/process.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(test_agent_tool.cpp 同款,只留本册要用的账)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;
    // 每次请求前人为拖的毫秒数(时间硬线测试用:把"跑久了"挤进一秒预算)。
    int per_request_delay_ms = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        if (per_request_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(per_request_delay_ms));
        }
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
    FakeTool(std::string name, tools::Tool::Result result)
        : name_(std::move(name)), result_(std::move(result)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        return result_;
    }

    int call_count = 0;

private:
    std::string name_;
    tools::Tool::Result result_;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                            api::Usage usage = api::Usage{}) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", usage},
    };
}

// 一份最小可派发的自定义 Agent 材料。
tools::CustomAgentMaterial ReviewerMaterial() {
    tools::CustomAgentMaterial material;
    material.definition.name = "library-reviewer";
    material.definition.description = "审查图书馆项目的代码与测试。";
    material.definition.tools.allow = {"read_file", "search"};
    return material;
}

// 真 git 临时仓库(test_agent_isolation.cpp 同款,worktree 保留测试的基准)。
struct GitRepo {
    std::filesystem::path root;

    GitRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_agentbudget_" +
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

// 在某次请求的 messages 里找含指定片段的文本块。
bool RequestHasText(const api::Request& request, const std::string& needle) {
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text != nullptr && text->text.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool RequestOffersTool(const api::Request& request, const std::string& tool_name) {
    for (const auto& tool : request.tools) {
        if (tool.name == tool_name) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数:软线派生与三线判定
// ---------------------------------------------------------------------------

TEST_CASE("预算软线纯函数:BudgetSoftLine 派生与零值语义") {
    CHECK(agent::BudgetSoftLine(0, 80) == 0);    // 无硬线 = 无软线
    CHECK(agent::BudgetSoftLine(100, 0) == 0);   // 不催办 = 无软线
    CHECK(agent::BudgetSoftLine(100, 80) == 80); // 常规派生
    CHECK(agent::BudgetSoftLine(10, 80) == 8);
    CHECK(agent::BudgetSoftLine(1, 80) == 1);    // 预算再小,软线也是个正数
    CHECK(agent::BudgetSoftLine(250, 80) == 200);
}

TEST_CASE("预算软线纯函数:CrossesBudgetSoftLine 三线任一过线即真") {
    using agent::CrossesBudgetSoftLine;
    // 不催办(percent=0):过线也不真。
    CHECK_FALSE(CrossesBudgetSoftLine(99, 100, 0, 0, 0, 0, 0));
    // 步数线:8/10 过 80% 软线。
    CHECK(CrossesBudgetSoftLine(8, 10, 0, 0, 0, 0, 80));
    CHECK_FALSE(CrossesBudgetSoftLine(7, 10, 0, 0, 0, 0, 80));
    // token 线:220/250 过 200 软线。
    CHECK(CrossesBudgetSoftLine(0, 0, 220, 250, 0, 0, 80));
    CHECK_FALSE(CrossesBudgetSoftLine(0, 0, 199, 250, 0, 0, 80));
    // 墙钟线:81s/100s 过 80s 软线(毫秒口径)。
    CHECK(CrossesBudgetSoftLine(0, 0, 0, 0, 81000, 100000, 80));
    CHECK_FALSE(CrossesBudgetSoftLine(0, 0, 0, 0, 79000, 100000, 80));
    // 未设的线不参与:只设步数时 token/墙钟再大也不触发。
    CHECK_FALSE(CrossesBudgetSoftLine(3, 10, 999999, 0, 999999, 0, 80));
}

// ---------------------------------------------------------------------------
// P2-6:软线催办 + 硬线返回部分结果与缘由
// ---------------------------------------------------------------------------

TEST_CASE("成本刹车:token 软线先催'请基于现有证据收尾',硬线断返回部分结果与缘由") {
    FakeBackend backend;
    // 每步 usage = 完整输入 100 + 输出 10 = 110 token。max_tokens=250:
    //   第 2 步后累计 220,跨过软线 200(80%)→ 第 3 次请求带催办;
    //   第 3 步后累计 330,硬线 250 断 → 收场,第 4 次请求不再发。
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("t_budget", "fake_tool", api::Usage{100, 10, 0, 0}));
    }
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"查到三个入口", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "预算任务";
    input["prompt"] = "查";
    input["max_tokens"] = 250;
    const tools::Tool::Result result = agent_tool.execute(input);

    // 硬线断在第 3 步后:三次请求,第三次带软线催办。
    REQUIRE(backend.captured_requests.size() == 3);
    CHECK_FALSE(RequestHasText(backend.captured_requests[0], "请基于现有证据收尾"));
    CHECK_FALSE(RequestHasText(backend.captured_requests[1], "请基于现有证据收尾"));
    CHECK(RequestHasText(backend.captured_requests[2], "请基于现有证据收尾"));
    CHECK(RequestHasText(backend.captured_requests[2], "[系统提醒]"));

    // 收场:budget_exhausted,缘由写明 token 线断,部分结果带回。
    CHECK(result.is_error);
    CHECK(result.content.find("[budget_exhausted]") == 0);
    CHECK(result.content.find("token 预算已用满") != std::string::npos);
    CHECK(result.content.find("250") != std::string::npos);
    CHECK(result.content.find("查到三个入口") != std::string::npos);  // 检查点不丢

    // 台账:终态、短因、预算账。
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::BudgetExhausted);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::TokenBudgetExhausted);
    CHECK(snapshots[0].outcome.token_limit == 250);
    CHECK(snapshots[0].token_limit == 250);  // Dock 明细行读的快照字段
    CHECK(snapshots[0].steps_used == 3);
    CHECK(snapshots[0].outcome.partial_result.find("查到三个入口") != std::string::npos);
}

TEST_CASE("成本刹车:时间硬线真断——引擎步顶收场,缘由写明是时间线") {
    FakeBackend backend;
    backend.per_request_delay_ms = 700;  // 两步就把 1 秒预算烧过线
    backend.scripts = {
        ToolUseScript("t_wall", "fake_tool", api::Usage{100, 10, 0, 0}),
        ToolUseScript("t_wall", "fake_tool", api::Usage{100, 10, 0, 0}),
        ToolUseScript("t_wall", "fake_tool", api::Usage{100, 10, 0, 0}),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"时间线检查点", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "限时任务";
    input["prompt"] = "查";
    input["max_time_secs"] = 1;  // 第 2 步顶上墙钟已过 1.4s,断线收场
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("[budget_exhausted]") == 0);
    CHECK(result.content.find("时间预算已用满") != std::string::npos);
    CHECK(result.content.find("时间线检查点") != std::string::npos);  // 部分结果不丢
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::BudgetExhausted);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::TimeBudgetExhausted);
    CHECK(snapshots[0].steps_used == 2);  // 第 3 步没再发
    CHECK(backend.captured_requests.size() == 2);
}

TEST_CASE("成本刹车:max_time_secs 参数落账,不设错的线不误伤正常收尾") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t_wall", "fake_tool", api::Usage{100, 10, 0, 0}),
        ToolUseScript("t_wall", "fake_tool", api::Usage{100, 10, 0, 0}),
        TextScript("结论:时间线内查完"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"时间线检查点", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "限时任务";
    input["prompt"] = "查";
    input["max_time_secs"] = 3600;  // 一小时:本测试跑不爆,验证的是参数落账与不误伤
    const tools::Tool::Result result = agent_tool.execute(input);

    // 预算没断:正常收尾,结论原样交回。
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("时间线内查完") != std::string::npos);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].wall_limit_secs == 3600);
    CHECK(snapshots[0].outcome.wall_limit_secs == 3600);
    CHECK(snapshots[0].state == tools::AgentTaskState::Done);
    CHECK(backend.captured_requests.size() == 3);  // 没被时间线误伤
}

TEST_CASE("成本刹车:入参校验——时间/token/软线类型与取值错一律明拒") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    struct Case {
        const char* key;
        nlohmann::json value;
        const char* expect;
    };
    const std::vector<Case> cases = {
        {"max_time_secs", "180", "max_time_secs 得是整数秒"},
        {"max_time_secs", -5, "max_time_secs 不能是负数"},
        {"max_tokens", "5000", "max_tokens 得是整数"},
        {"max_tokens", -1, "max_tokens 不能是负数"},
        {"budget_soft_percent", "80", "budget_soft_percent 得是 0~100"},
        {"budget_soft_percent", 101, "budget_soft_percent 只收 0~100"},
    };
    for (const auto& item : cases) {
        nlohmann::json input;
        input["title"] = "校验任务";
        input["prompt"] = "查";
        input[item.key] = item.value;
        const tools::Tool::Result result = agent_tool.execute(input);
        CHECK(result.is_error);
        CHECK(result.content.find(item.expect) != std::string::npos);
    }
    CHECK(backend.captured_requests.empty());  // 参数错不发请求
}

// ---------------------------------------------------------------------------
// P2-2:自定义 Agent 身份与派发
// ---------------------------------------------------------------------------

TEST_CASE("自定义 Agent:身份按 resolved name 记,不冒名 Explore;工具面按 allow 收窄") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论:审查完毕")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"写了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.skills_preload = {"library-domain"};
    material.preloaded_skills = {"领域约束:借阅上限 5 本。"};
    agent_tool.SetCustomAgentResolver(
        [material](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            return name == material.definition.name ? std::optional<tools::CustomAgentMaterial>(material)
                                                    : std::nullopt;
        });

    nlohmann::json input;
    input["title"] = "审查任务";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("审查完毕") != std::string::npos);

    // 身份:Dock 拼名(Entries 的 entry.name = agent_type + " #N")读台账,
    // 这里是 resolved name,不再是 Explore。
    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].agent_type == "library-reviewer");
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].agent_type == "library-reviewer");

    // 系统提示:人格带名字与描述,预装技能正文进提示。
    REQUIRE(backend.captured_requests.size() == 1);
    const api::Request& request = backend.captured_requests[0];
    CHECK(request.system.find("你是 library-reviewer 子代理") != std::string::npos);
    CHECK(request.system.find("审查图书馆项目的代码与测试") != std::string::npos);
    CHECK(request.system.find("预装技能 library-domain") != std::string::npos);
    CHECK(request.system.find("领域约束:借阅上限 5 本") != std::string::npos);

    // 工具面:allow 名单外(write_file)模型看不见——只读靠 tools.allow。
    CHECK(RequestOffersTool(request, "read_file"));
    CHECK(RequestOffersTool(request, "search"));
    CHECK_FALSE(RequestOffersTool(request, "write_file"));
}

TEST_CASE("自定义 Agent:点名 Prompt Profile——core 走五层,裁掉的工具不配文案") {
    // 用户层放一份 browser-tester 的 core 覆盖:用户 Profile 层要压过内置
    // Profile 层(契约 §6.2),生成的 persona 让位。
    const std::filesystem::path user_prompts =
        std::filesystem::temp_directory_path() /
        ("lubancode_agent_profile_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(user_prompts / "profiles" / "browser-tester" / "core");
    {
        std::ofstream out(user_prompts / "profiles" / "browser-tester" / "core" / "10-identity.md",
                          std::ios::binary);
        out << "用户层的 Profile 身份:专管网页查验。";
    }

    FakeBackend backend;
    backend.scripts = {TextScript("结论:查验完毕")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"命中", false}));
    sub_registry.Register(std::make_unique<FakeTool>("web_fetch", tools::Tool::Result{"网页", false}));
    sub_registry.Register(std::make_unique<FakeTool>("run_command", tools::Tool::Result{"跑完了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("todo_write", tools::Tool::Result{"记了", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetPromptsDir(user_prompts.string());
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.prompt.profile = "browser-tester";
    material.definition.tools.allow = {"read_file", "search", "web_fetch"};  // 裁掉 shell/todo/委派
    agent_tool.SetCustomAgentResolver(
        [material](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            return name == material.definition.name ? std::optional<tools::CustomAgentMaterial>(material)
                                                    : std::nullopt;
        });

    nlohmann::json input;
    input["title"] = "查验任务";
    input["prompt"] = "查";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);

    REQUIRE(backend.captured_requests.size() == 1);
    const api::Request& request = backend.captured_requests[0];
    // core 来自用户 Profile 层;生成的 persona(名字+描述)让位。
    CHECK(request.system.find("用户层的 Profile 身份:专管网页查验") != std::string::npos);
    CHECK(request.system.find("你是 library-reviewer 子代理") == std::string::npos);
    // web 工具在表里:web feature 装的是内置 Profile 版文案(用户层没盖它)。
    CHECK(request.system.find("联网查证(网页查验向)") != std::string::npos);
    // 只给有效能力配说明(单子 §5.4):按各模块正文里的特征句认位——
    // files 在(read_file);run_command/todo_write/agent 被裁,shell/todo/
    // delegation 文案一个字不装。
    CHECK(request.system.find("读文件用 read_file") != std::string::npos);
    CHECK(request.system.find("跑命令用 run_command") == std::string::npos);
    CHECK(request.system.find("先调用 todo_write 列一份清单") == std::string::npos);
    CHECK(request.system.find("优先用 agent 工具委托给子代理") == std::string::npos);
    // 工具面与文案同源:run_command 模型也看不见。
    CHECK_FALSE(RequestOffersTool(request, "run_command"));
    CHECK(RequestOffersTool(request, "web_fetch"));

    // 删掉用户层覆盖,再派一次:稳稳退回内置 Profile 层(验收线)。
    std::error_code ec;
    std::filesystem::remove(user_prompts / "profiles" / "browser-tester" / "core" / "10-identity.md", ec);
    backend.captured_requests.clear();
    backend.scripts = {TextScript("结论:再验一次")};
    const tools::Tool::Result again = agent_tool.execute(input);
    CHECK_FALSE(again.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("你是 browser-tester,一只专管网页查验的子代理") !=
          std::string::npos);  // 内置 Profile 的身份模块
    CHECK(backend.captured_requests[0].system.find("用户层的 Profile 身份") == std::string::npos);

    std::filesystem::remove_all(user_prompts, ec);
}

TEST_CASE("自定义 Agent:不点名 Profile——生成 persona 与恒在四件套照旧") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论:照旧")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"内容", false}));
    sub_registry.Register(std::make_unique<FakeTool>("run_command", tools::Tool::Result{"跑完了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("todo_write", tools::Tool::Result{"记了", false}));
    sub_registry.Register(std::make_unique<FakeTool>("agent", tools::Tool::Result{"派了", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.tools.allow.clear();  // 全继承:四件套的说明都该在
    agent_tool.SetCustomAgentResolver(
        [material](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            return name == material.definition.name ? std::optional<tools::CustomAgentMaterial>(material)
                                                    : std::nullopt;
        });

    nlohmann::json input;
    input["title"] = "照旧任务";
    input["prompt"] = "审";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    const api::Request& request = backend.captured_requests[0];
    CHECK(request.system.find("你是 library-reviewer 子代理") != std::string::npos);
    CHECK(request.system.find("读文件用 read_file") != std::string::npos);        // files
    CHECK(request.system.find("跑命令用 run_command") != std::string::npos);      // shell
    CHECK(request.system.find("先调用 todo_write 列一份清单") != std::string::npos);  // todo
    CHECK(request.system.find("优先用 agent 工具委托给子代理") != std::string::npos);  // delegation
}

TEST_CASE("自定义 Agent:runtime.max_steps_per_turn 落到派出预算,真能压住循环") {
    FakeBackend backend;
    for (int i = 0; i < 6; ++i) {
        backend.scripts.push_back(ToolUseScript("t_loop", "read_file"));
    }
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"又读了一遍", false}));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"写了", false}));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");  // 配置默认 0 = 不限步
    tools::CustomAgentMaterial material = ReviewerMaterial();
    material.definition.max_steps_per_turn = 3;  // YAML runtime.max_steps_per_turn
    agent_tool.SetCustomAgentResolver(
        [material](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            return name == material.definition.name ? std::optional<tools::CustomAgentMaterial>(material)
                                                    : std::nullopt;
        });

    nlohmann::json input;
    input["title"] = "压预算";
    input["prompt"] = "死循环吧";
    input["agent_type"] = "library-reviewer";
    const tools::Tool::Result result = agent_tool.execute(input);

    // YAML 的 3 步压住了循环:第 4 次请求没发出去。
    CHECK(result.is_error);
    CHECK(result.content.find("[budget_exhausted]") == 0);
    CHECK(result.content.find("3/3 步") != std::string::npos);
    CHECK(backend.captured_requests.size() == 3);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].step_limit == 3);
    CHECK(snapshots[0].steps_used == 3);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::StepLimitExhausted);

    // 入参显式压过 YAML:同一定义给 2,入参给 5,按 5 算。
    FakeBackend backend2;
    for (int i = 0; i < 6; ++i) {
        backend2.scripts.push_back(ToolUseScript("t_loop", "read_file"));
    }
    tools::ToolRegistry sub_registry2;
    sub_registry2.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"又读了一遍", false}));
    tools::AgentTool agent_tool2(backend2, sub_registry2, "/work/dir");
    agent_tool2.SetCustomAgentResolver(
        [material](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            return name == material.definition.name ? std::optional<tools::CustomAgentMaterial>(material)
                                                    : std::nullopt;
        });
    nlohmann::json input2;
    input2["title"] = "压预算二";
    input2["prompt"] = "死循环吧";
    input2["agent_type"] = "library-reviewer";
    input2["max_steps_per_turn"] = 5;
    const tools::Tool::Result result2 = agent_tool2.execute(input2);
    CHECK(result2.is_error);
    CHECK(backend2.captured_requests.size() == 5);  // 入参 5 压过 YAML 3
}

TEST_CASE("自定义 Agent:名字不在清单明拒并指路 /agents;没接解析口维持旧口径") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetCustomAgentResolver(
        [](const std::string&) -> std::optional<tools::CustomAgentMaterial> { return std::nullopt; });

    nlohmann::json input;
    input["title"] = "点名任务";
    input["prompt"] = "查";
    input["agent_type"] = "no-such-agent";
    const tools::Tool::Result result = agent_tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("no-such-agent") != std::string::npos);
    CHECK(result.content.find("/agents") != std::string::npos);
    CHECK(result.content.find("/agent doctor") != std::string::npos);
    CHECK(backend.captured_requests.empty());

    // 没接解析口(旧调用方):非内置类型仍按旧文案拒。
    tools::AgentTool old_tool(backend, sub_registry, "/work/dir");
    nlohmann::json old_input;
    old_input["title"] = "点名任务";
    old_input["prompt"] = "查";
    old_input["agent_type"] = "library-reviewer";
    const tools::Tool::Result old_result = old_tool.execute(old_input);
    CHECK(old_result.is_error);
    CHECK(old_result.content.find("只认 general-purpose 或 Explore") != std::string::npos);
}

// ---------------------------------------------------------------------------
// P2-6:超预算不丢工作树
// ---------------------------------------------------------------------------

TEST_CASE("成本刹车:撞硬线的隔离子代理,工作树与产物保留不丢") {
    GitRepo repo;
    FakeBackend backend;
    // 第一步写文件进房,后续死循环调读——步数硬线 2 断在半路。
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

    tools::AgentTool agent_tool(backend, sub_registry, GitRepo::PathToUtf8(repo.root));
    nlohmann::json input;
    input["title"] = "隔离任务";
    input["prompt"] = "写";
    input["isolation"] = "worktree";
    input["max_steps_per_turn"] = 2;
    const tools::Tool::Result result = agent_tool.execute(input);

    // 预算断了:缘由明说,部分结果(最后工具)带回。
    CHECK(result.is_error);
    CHECK(result.content.find("[budget_exhausted]") != std::string::npos);
    CHECK(result.content.find("2/2 步") != std::string::npos);
    // 工作树没丢:房因有活保留,产物在房里,结果附房路径。
    CHECK(result.content.find("已保留") != std::string::npos);
    const auto rooms = repo.AgentRooms();
    REQUIRE(rooms.size() == 1);
    CHECK(std::filesystem::exists(rooms[0] / "out.txt"));
    CHECK(result.content.find(GitRepo::PathToUtf8(rooms[0].filename())) != std::string::npos);
}
