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

    platform::ProcessResult RunGit(std::vector<std::string> args) const {
        std::vector<std::string> argv = {"git", "-C", PathToUtf8(root)};
        argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
        return platform::RunProcess(argv, 60000);
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
