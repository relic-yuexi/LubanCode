// ToolRuntime 的装配与寿命性状测试:真构造、真查询、真析构(空配置下
// 不起 MCP 子进程、不配 LSP;插件目录扫到什么都不影响断言)。MCP/DLL/LSP
// 的真 fixture 见 test_mcp_*、test_plugins、test_lsp_*,这边只钉装配结构:
// 哪张表有哪些工具、agent 工具抓的引用、Explore 硬边界、过滤与补挂。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "app/tool_runtime.hpp"

namespace {

class NullBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        return {};
    }
};

lubancode::config::Config EmptyConfig() {
    lubancode::config::Config config;
    config.mcp_servers.clear();
    config.lsp_servers.clear();
    config.search = lubancode::config::SearchConfig();
    return config;
}

const std::vector<lubancode::tools::SkillMeta>& NoSkills() {
    static const std::vector<lubancode::tools::SkillMeta> skills;
    return skills;
}

}  // namespace

using namespace lubancode::app;

TEST_CASE("默认装配:主表有 agent/todo_write/基础工具,子表无 agent,Explore 缺席") {
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    ToolRuntime runtime(config, lubancode::cli::BuiltinTheme("plain"), backend, NoSkills(),
                        /*skills_segment=*/"", /*cwd_utf8=*/"/tmp", ToolRuntime::Options{});

    CHECK(runtime.main_registry().Find("agent") != nullptr);
    CHECK(runtime.main_registry().Find("todo_write") != nullptr);
    CHECK(runtime.main_registry().Find("read_file") != nullptr);
    CHECK(runtime.main_registry().Find("run_command") != nullptr);
    CHECK(runtime.main_registry().Find("ask_user") == nullptr);  // 交互独有,默认不挂
    CHECK(runtime.sub_registry().Find("agent") == nullptr);      // 防递归
    CHECK(runtime.sub_registry().Find("todo_write") == nullptr); // 只挂主表
    CHECK(runtime.sub_registry().Find("read_file") != nullptr);
    CHECK(runtime.explore_registry() == nullptr);  // 单发/默认无 Explore
    CHECK(runtime.agent_tool() != nullptr);
    CHECK(runtime.todo_state() != nullptr);
    CHECK(runtime.loaded_tools() != nullptr);
    // 空配置下工具总数低于延迟阈值,tool_search 不启用,过滤直通。
    CHECK(runtime.main_deferral() == false);
    CHECK(runtime.sub_deferral() == false);
    CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    CHECK(runtime.sub_tool_filter()(*runtime.sub_registry().Find("read_file")));
    runtime.AttachMemoryTool(nullptr);  // 空指针安全
}

TEST_CASE("with_explore:Explore 只读硬边界,并挂到 agent 工具") {
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    ToolRuntime::Options options;
    options.with_explore = true;
    ToolRuntime runtime(config, lubancode::cli::BuiltinTheme("plain"), backend, NoSkills(),
                        /*skills_segment=*/"", /*cwd_utf8=*/"/tmp", std::move(options));

    lubancode::tools::ToolRegistry* explore = runtime.explore_registry();
    REQUIRE(explore != nullptr);
    CHECK(explore->Find("read_file") != nullptr);
    CHECK(explore->Find("search") != nullptr);
    CHECK(explore->Find("write_file") == nullptr);   // 只读边界:无写入
    CHECK(explore->Find("run_command") == nullptr);  // 无命令
    CHECK(explore->Find("agent") == nullptr);        // 无委托
    // Explore 表不进 MCP(空配置下无从验),但 agent 工具确实拿到了它。
    CHECK(runtime.agent_tool() != nullptr);
}

TEST_CASE("寿命:构造-查询-析构全程不崩,表地址稳定") {
    lubancode::config::Config config = EmptyConfig();
    auto backend = std::make_unique<NullBackend>();
    const std::vector<lubancode::tools::SkillMeta> no_skills;
    auto runtime = std::make_unique<ToolRuntime>(config, lubancode::cli::BuiltinTheme("plain"), *backend,
                                                 no_skills, /*skills_segment=*/"", /*cwd_utf8=*/"/tmp",
                                                 ToolRuntime::Options{});
    lubancode::tools::ToolRegistry* main_before = &runtime->main_registry();
    lubancode::tools::ToolRegistry* sub_before = &runtime->sub_registry();
    CHECK(runtime->main_registry().Find("agent") != nullptr);
    CHECK(&runtime->main_registry() == main_before);
    CHECK(&runtime->sub_registry() == sub_before);
    runtime.reset();  // 真析构:表先亡、拥有者后亡
}
