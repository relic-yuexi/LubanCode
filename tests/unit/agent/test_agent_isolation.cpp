// agent 工具的 isolation=worktree(0.27.x 子代理隔离):真 git 临时仓库 +
// FakeBackend 脚本,验证:写落进房、主会话 cwd 分毫不动;干净房自动删、
// 有活房保留且结果附房路径;并行两个隔离子代理互不踩脚;跑着时房上着锁;
// 入参校验。房务自由函数(CreateAgentWorktree/CleanStaleAgentWorktrees)在
// test_worktree.cpp 钉。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/worktree.hpp"
#include "platform/process.hpp"
#include "tools/agent_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
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

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 真 git 临时仓库:git init + 一次提交,给 worktree add 一个能用的基准。
struct GitRepo {
    std::filesystem::path root;

    GitRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_agentiso_" +
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

    platform::ProcessResult RunGitAt(const std::filesystem::path& cwd, std::vector<std::string> args) const {
        std::vector<std::string> argv = {"git", "-C", PathToUtf8(cwd)};
        argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
        return platform::RunProcess(argv, 60000);
    }

    platform::ProcessResult RunGit(std::vector<std::string> args) const { return RunGitAt(root, std::move(args)); }

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

TEST_CASE("agent isolation: 写进房、主 cwd 不动、有活房保留并附路径") {
    GitRepo repo;
    const std::filesystem::path cwd_before = std::filesystem::current_path();

    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1", "write_file", "{\"path\":\"out.txt\",\"content\":\"hello\"}"),
        TextScript("结论:写好了"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<tools::WriteFileTool>());

    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "写个文件"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    // 进程 cwd 分毫不动(子代理是线程,绝不能 chdir)
    CHECK(std::filesystem::current_path() == cwd_before);
    // 相对路径写进了某个 agent- 房,房因有活保留,结果附路径
    // (TEMP 环境变量可能是 8.3 短名、git 输出是长名,同一目录两种写法,
    // 全路径逐字比对会瞎——按唯一房名对。)
    REQUIRE(result.content.find("已保留") != std::string::npos);
    const auto rooms = repo.AgentRooms();
    REQUIRE(rooms.size() == 1);
    const std::string room_name = PathToUtf8(rooms[0].filename());
    CHECK(room_name.starts_with("agent-"));
    CHECK(std::filesystem::exists(rooms[0] / "out.txt"));
    CHECK(result.content.find(room_name) != std::string::npos);
    // 系统提示里告知隔离房
    REQUIRE(backend.captured_requests.size() == 2);
    CHECK(backend.captured_requests[0].system.find("隔离的 git worktree") != std::string::npos);
    CHECK(backend.captured_requests[0].system.find(room_name) != std::string::npos);
    CHECK(backend.captured_requests[0].system.find("- 工作目录:") ==
          backend.captured_requests[0].system.rfind("- 工作目录:"));
    REQUIRE_FALSE(backend.captured_requests[0].messages.empty());
    REQUIRE_FALSE(backend.captured_requests[0].messages.front().content.empty());
    const auto* first_text = std::get_if<api::TextBlock>(&backend.captured_requests[0].messages.front().content[0]);
    REQUIRE(first_text != nullptr);
    CHECK(first_text->text == "写个文件");
    // 收工房已解锁
    CHECK_FALSE(cli::ListWorktrees(repo.root).empty());
    bool locked_agent_room = false;
    for (const auto& entry : cli::ListWorktrees(repo.root)) {
        if (entry.locked && entry.path.filename().string().starts_with("agent-")) {
            locked_agent_room = true;
        }
    }
    CHECK_FALSE(locked_agent_room);
}

TEST_CASE("agent isolation: 干净房与分支自动删") {
    GitRepo repo;
    FakeBackend backend;
    backend.scripts = {TextScript("结论:只是看了看")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<tools::WriteFileTool>());

    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "看看"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("已保留") == std::string::npos);  // 没留房
    CHECK(repo.AgentRooms().empty());                           // 房删了
    // 分支也删了:worktree/agent-* 一条不剩
    const auto branches = repo.RunGit({"branch", "--list", "worktree/agent-*"});
    CHECK(branches.exit_code == 0);
    CHECK(branches.output.empty());
}

TEST_CASE("agent isolation: 并行两个隔离子代理各住各的房,互不踩脚") {
    GitRepo repo;
    const std::filesystem::path cwd_before = std::filesystem::current_path();

    auto run_one = [&repo](const std::string& filename) {
        FakeBackend backend;
        backend.scripts = {
            ToolUseScript("t1", "write_file", "{\"path\":\"" + filename + "\",\"content\":\"x\"}"),
            TextScript("done"),
        };
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<tools::WriteFileTool>());
        tools::AgentTool agent_tool(backend, registry, PathToUtf8(repo.root));
        return agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "写"}, {"isolation", "worktree"}});
    };

    tools::Tool::Result result_a;
    tools::Tool::Result result_b;
    std::thread thread_a([&]() { result_a = run_one("a.txt"); });
    std::thread thread_b([&]() { result_b = run_one("b.txt"); });
    thread_a.join();
    thread_b.join();

    CHECK_FALSE(result_a.is_error);
    CHECK_FALSE(result_b.is_error);
    CHECK(std::filesystem::current_path() == cwd_before);
    // 两间不同的房,各写各的文件
    const auto rooms = repo.AgentRooms();
    REQUIRE(rooms.size() == 2);
    CHECK(rooms[0] != rooms[1]);
    int files_found = 0;
    for (const auto& room : rooms) {
        if (std::filesystem::exists(room / "a.txt") || std::filesystem::exists(room / "b.txt")) {
            ++files_found;
        }
    }
    CHECK(files_found == 2);
}

namespace {

// 房务探针:在子代理跑动期间看一眼 git 元数据,记下有没有 agent- 房上着锁。
class LockProbeTool : public tools::Tool {
public:
    explicit LockProbeTool(std::filesystem::path repo_root) : repo_root_(std::move(repo_root)) {}

    std::string name() const override { return "lock_probe"; }
    std::string description() const override { return "test probe"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override {
        std::error_code ec;
        const std::filesystem::path admin_root = repo_root_ / ".git" / "worktrees";
        for (const auto& entry : std::filesystem::directory_iterator(admin_root, ec)) {
            if (!entry.path().filename().string().starts_with("agent-")) {
                continue;
            }
            if (std::filesystem::exists(entry.path() / "locked", ec)) {
                saw_locked_room = true;
            }
        }
        ++calls;
        return {"probed", false};
    }

    std::atomic<int> calls{0};
    std::atomic<bool> saw_locked_room{false};

private:
    std::filesystem::path repo_root_;
};

}  // namespace

TEST_CASE("agent isolation: 代理跑着时房上着锁,收工解锁") {
    GitRepo repo;
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1", "lock_probe", "{}"),
        TextScript("done"),
    };
    auto probe = std::make_unique<LockProbeTool>(repo.root);
    LockProbeTool* probe_ptr = probe.get();
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::move(probe));

    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "探"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    REQUIRE(probe_ptr->calls.load() == 1);
    CHECK(probe_ptr->saw_locked_room.load());  // 跑动期间:locked 文件在
    // 收工:探针没改文件,房干净,连同分支自动删掉(锁也随房消)
    CHECK(repo.AgentRooms().empty());
    bool still_locked = false;
    for (const auto& entry : cli::ListWorktrees(repo.root)) {
        if (entry.locked && PathToUtf8(entry.path.filename()).starts_with("agent-")) {
            still_locked = true;
        }
    }
    CHECK_FALSE(still_locked);
}

TEST_CASE("agent isolation: 入参校验") {
    GitRepo repo;
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));

    CHECK(agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "x"}, {"isolation", "both"}}).is_error);
    CHECK(agent_tool
              .execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "x"}, {"isolation", "worktree"}, {"agent_type", "Explore"}})
              .is_error);
    // 不在 git 仓库里:建不了房,报错而不是崩
    tools::AgentTool outside(backend, sub_registry, PathToUtf8(std::filesystem::temp_directory_path()));
    const auto failed = outside.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "x"}, {"isolation", "worktree"}});
    CHECK(failed.is_error);
    CHECK(failed.content.find("git 仓库") != std::string::npos);
}

namespace {

std::string TrimGitOutput(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ')) {
        --end;
    }
    return text.substr(0, end);
}

}  // namespace

TEST_CASE("agent isolation: 基线=派工瞬间的调用者 HEAD,领先远端不漂(派工单 §三)") {
    GitRepo repo;
    // origin/main 钉在第一笔,本地再进一笔(未推)——旧实现从 origin/main
    // 起树会读不到 second.txt。
    const std::string first = TrimGitOutput(repo.RunGit({"rev-parse", "HEAD"}).output);
    REQUIRE(repo.RunGit({"update-ref", "refs/remotes/origin/main", first}).exit_code == 0);
    REQUIRE(repo.RunGit({"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main"}).exit_code == 0);
    std::ofstream(repo.root / "second.txt") << "second\n";
    REQUIRE(repo.RunGit({"add", "."}).exit_code == 0);
    REQUIRE(repo.RunGit({"commit", "-q", "-m", "second"}).exit_code == 0);
    const std::string head = TrimGitOutput(repo.RunGit({"rev-parse", "HEAD"}).output);

    // 房里的子代理真读得到新代码:脚本第一拍用 read_file 读 second.txt。
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1", "read_file", "{\"path\":\"second.txt\"}"),
        TextScript("结论:读到 second"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<tools::ReadFileTool>());
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const tools::Tool::Result result = agent_tool.execute(
        nlohmann::json{{"title", "基线验证"}, {"prompt", "读 second.txt"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("second") != std::string::npos);  // 新代码在房里
    // 基线账随 TaskSnapshot 持久化:恢复/重试/嵌套对账认这一枚。
    const auto detail = agent_tool.TaskDetail(1);
    REQUIRE(detail.has_value());
    CHECK(detail->isolation_base_commit == head);
    CHECK(detail->isolation_base_ref == "main");
    CHECK_FALSE(detail->isolation_branch.empty());
    CHECK(detail->isolation_branch.rfind("worktree/agent-", 0) == 0);
    CHECK(detail->worktree_removed);            // 干净无提交:收工照旧清理
    CHECK_FALSE(detail->worktree_awaiting_review);
    // 结果文本带基线附言,调用方看得见起树基准。
    CHECK(result.content.find("[隔离基线] base=main@" + head) != std::string::npos);
}

TEST_CASE("agent isolation: 调用者未提交改动明示,不悄悄丢(派工单 §3.4)") {
    GitRepo repo;
    std::ofstream(repo.root / "wip.txt") << "uncommitted\n";  // 未提交改动

    FakeBackend backend;
    backend.scripts = {TextScript("结论:只是看了看")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const tools::Tool::Result result = agent_tool.execute(
        nlohmann::json{{"title", "脏树派工"}, {"prompt", "看看"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    // 明示:基线附言写清"未提交改动不在房内",不是悄悄丢。
    CHECK(result.content.find("未提交改动") != std::string::npos);
    CHECK(result.content.find("不在房内") != std::string::npos);
    // 房从提交起树、子代理没动房:干净无自有提交,收工照旧自动清理——
    // 调用者的 wip.txt 从头到尾没进过任何房。
    CHECK(repo.AgentRooms().empty());
}

TEST_CASE("agent isolation: 嵌套派工按父任务 effective_cwd 冻结,不回读已搬走的宿主 cwd") {
    GitRepo caller_repo;
    GitRepo moved_repo;
    std::ofstream(caller_repo.root / "caller-only.txt") << "caller\n";
    REQUIRE(caller_repo.RunGit({"add", "."}).exit_code == 0);
    REQUIRE(caller_repo.RunGit({"commit", "-q", "-m", "caller head"}).exit_code == 0);
    const std::string caller_head = TrimGitOutput(caller_repo.RunGit({"rev-parse", "HEAD"}).output);

    FakeBackend root_backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(root_backend, sub_registry, PathToUtf8(moved_repo.root));

    auto env = std::make_shared<tools::SubagentDispatchEnv>();
    env->effective_cwd = PathToUtf8(caller_repo.root);
    env->base_registry = &sub_registry;
    env->headless = true;
    env->detached_shared = std::make_shared<tools::DetachedAgentBackend>();
    env->detached_shared->backend = std::make_unique<FakeBackend>();
    static_cast<FakeBackend*>(env->detached_shared->backend.get())->scripts = {TextScript("嵌套完成")};
    tools::AgentDispatchHandle nested(agent_tool.coordinator(), tools::AgentRunIdentity{0, 0, 0, ""}, env);
    const auto result = nested.Dispatch(
        nlohmann::json{{"title", "嵌套基线"}, {"prompt", "检查 caller-only.txt"}, {"isolation", "worktree"}});

    CHECK_FALSE(result.is_error);
    const auto detail = agent_tool.TaskDetail(1);
    REQUIRE(detail.has_value());
    CHECK(detail->isolation_base_commit == caller_head);
    CHECK(result.content.find("[隔离基线] base=main@" + caller_head) != std::string::npos);
    CHECK(caller_repo.AgentRooms().empty());
    CHECK(moved_repo.AgentRooms().empty());
}

TEST_CASE("agent isolation: TaskSnapshot 基线与调用者 HEAD 不符时建房前阻断") {
    GitRepo repo;
    const std::string first = TrimGitOutput(repo.RunGit({"rev-parse", "HEAD"}).output);
    int head_reads = 0;
    const cli::GitRunner drifting = [&repo, &head_reads, &first](const cli::GitCommand& command) {
        if (!command.args.empty() && command.args[0] == "rev-parse" && command.args.size() > 1 &&
            command.args[1] == "HEAD") {
            ++head_reads;
            if (head_reads == 2) {
                return cli::GitCommandResult{0, "0000000000000000000000000000000000000000\n", {}};
            }
        }
        const auto result = repo.RunGitAt(command.working_directory, command.args);
        return cli::GitCommandResult{static_cast<int>(result.exit_code), result.output, result.spawn_error};
    };

    FakeBackend backend;
    backend.scripts = {TextScript("不应请求")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    agent_tool.SetGitRunner(drifting);
    const auto rejected = agent_tool.execute(
        nlohmann::json{{"title", "基线漂移"}, {"prompt", "看看"}, {"isolation", "worktree"}});

    CHECK(rejected.is_error);
    CHECK(rejected.content.find("[isolation_base_mismatch]") != std::string::npos);
    CHECK(rejected.content.find(first) != std::string::npos);
    CHECK(rejected.content.find("0000000000000000000000000000000000000000") != std::string::npos);
    CHECK(repo.AgentRooms().empty());
    CHECK(backend.captured_requests.empty());
}

TEST_CASE("agent 派工前查单: 当前 HEAD 已完成且无未勾批次时拒发并回执完成提交") {
    GitRepo repo;
    std::filesystem::create_directories(repo.root / "todos");
    std::ofstream(repo.root / "todos" / "done.todo") << "# 已完成\n- 状态：已实现并验收\n- [x] P0\n";
    REQUIRE(repo.RunGit({"add", "."}).exit_code == 0);
    REQUIRE(repo.RunGit({"commit", "-q", "-m", "finish todo"}).exit_code == 0);
    const std::string done_commit = TrimGitOutput(repo.RunGit({"rev-parse", "HEAD"}).output);

    FakeBackend backend;
    backend.scripts = {TextScript("不应请求")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const auto rejected = agent_tool.execute(nlohmann::json{
        {"title", "重做旧单"}, {"prompt", "请修 todos/done.todo"}, {"isolation", "worktree"}});

    CHECK(rejected.is_error);
    CHECK(rejected.content.find("[todo_already_completed]") != std::string::npos);
    CHECK(rejected.content.find("此单已修完于 " + done_commit) != std::string::npos);
    CHECK(rejected.content.find("确认要在当前基线重做？") != std::string::npos);
    CHECK(repo.AgentRooms().empty());
    CHECK(backend.captured_requests.empty());
    CHECK(agent_tool.TaskSnapshots().empty());
}

TEST_CASE("agent 派工前查单: 已完成状态仍有未勾批次时不误拦") {
    GitRepo repo;
    std::filesystem::create_directories(repo.root / "todos");
    std::ofstream(repo.root / "todos" / "reopened.todo")
        << "# 又开工\n- 状态：已实现（旧批次）\n- [x] 旧批次\n- [ ] 新批次\n";
    REQUIRE(repo.RunGit({"add", "."}).exit_code == 0);
    REQUIRE(repo.RunGit({"commit", "-q", "-m", "reopen todo"}).exit_code == 0);

    FakeBackend backend;
    backend.scripts = {TextScript("继续实施")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const auto result = agent_tool.execute(
        nlohmann::json{{"title", "实施新批次"}, {"prompt", "修 todos/reopened.todo"}});

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("继续实施") != std::string::npos);
    CHECK(backend.captured_requests.size() == 1);
}

TEST_CASE("agent isolation: 无后台后端的后台+worktree 派工——preflight 先拒,worktree 零创建") {
    GitRepo repo;
    FakeBackend backend;
    backend.scripts = {TextScript("不该跑到")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, PathToUtf8(repo.root));
    const auto rejected = agent_tool.execute(
        nlohmann::json{{"title", "后台隔离"}, {"prompt", "x"}, {"execution_mode", "background"},
                       {"isolation", "worktree"}});
    CHECK(rejected.is_error);
    CHECK(rejected.content.find("[background_unavailable]") != std::string::npos);
    CHECK(repo.AgentRooms().empty());          // worktree 零创建
    CHECK(backend.captured_requests.empty());  // backend 零调用
}
