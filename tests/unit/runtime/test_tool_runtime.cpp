// ToolRuntime 的装配与寿命性状测试:真构造、真查询、真析构(空配置下
// 不起 MCP 子进程、不配 LSP;用户主目录由 ScopedHomeEnv 钉到空临时目录,
// 插件三路扫描静默空,工具数只剩内置那批)。MCP/DLL/LSP 的真 fixture 见
// test_mcp_*、test_plugins、test_lsp_*,这边只钉装配结构:哪张表有哪些
// 工具、agent 工具抓的引用、Explore 硬边界、过滤与补挂。
// 全局插件真装上时 deferral 该触发,是产品行为——那笔对账归集成册
// integration/plugins/test_tool_runtime_deferral.cpp,这边只管隔离。
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "app/tool_runtime.hpp"
#include "platform/paths.hpp"

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

// 用户目录闸:把 HomeLubancodeDir() 的根(Windows 读 %USERPROFILE%、别的
// 平台读 $HOME)钉到一只空临时目录,出了作用域原样还原。MountPlugins 在
// ToolRuntime 构造里会扫 <主目录>/.lubancode/plugins(DLL/Lua/process 三
// 路),真机上用户装过插件,插件工具就数进注册表——空配置的"工具数低于
// 延迟阈值"断言跟着假红(真机实测单 P1-3:一枚 gui-agent 十枚工具就能把
// deferral 顶成 true)。铁律是 ctest 裸跑即绿,单测不读用户家目录:Config
// 与 ToolRuntime::Options 都没有插件目录注入口,环境变量是唯一口子,就从
// 这儿钉。doctest 单进程:设/还原由 RAII 成对保证,异常路径也还原;本册
// 三只用例各持各的闸,先构造的用例还原后才轮到下一只,不连坐。
class ScopedHomeEnv {
public:
    ScopedHomeEnv() {
        std::error_code ec;
        home_ = std::filesystem::temp_directory_path(ec) / "lubancode_tool_runtime_home";
        std::filesystem::remove_all(home_, ec);  // 上一回跑剩的插件清零
        std::filesystem::create_directories(home_, ec);
        const std::string value = lubancode::platform::PathToUtf8(home_);
#ifdef _WIN32
        // 旧值按 HomeLubancodeDir 同一条编码链存成 UTF-8(GetEnvVar 拿的是
        // ACP 字节,先 AcpBytesToUtf8 解回),还原时再转回去,一口进一口出。
        const auto raw = lubancode::platform::GetEnvVar("USERPROFILE");
        old_utf8_ = raw.has_value() ? std::optional<std::string>(lubancode::platform::AcpBytesToUtf8(*raw))
                                    : std::nullopt;
        // _wputenv 走宽口,同步写 CRT 与进程两块环境;_dupenv_s 读的正是
        // CRT 块。不经窄口,编码不漂。
        SetWindowsUserProfiles(value);
#else
        const char* raw = std::getenv("HOME");
        old_utf8_ = raw != nullptr ? std::optional<std::string>(raw) : std::nullopt;
        setenv("HOME", value.c_str(), /*replace=*/1);
#endif
    }
    ~ScopedHomeEnv() {
#ifdef _WIN32
        SetWindowsUserProfiles(old_utf8_.value_or(std::string()));  // 空串即移除
#else
        if (old_utf8_.has_value()) {
            setenv("HOME", old_utf8_->c_str(), /*replace=*/1);
        } else {
            unsetenv("HOME");
        }
#endif
    }
    ScopedHomeEnv(const ScopedHomeEnv&) = delete;
    ScopedHomeEnv& operator=(const ScopedHomeEnv&) = delete;

private:
#ifdef _WIN32
    static void SetWindowsUserProfiles(const std::string& utf8_value) {
        const std::wstring entry = L"USERPROFILE=" + lubancode::platform::Utf8ToWide(utf8_value);
        _wputenv(entry.c_str());
    }
#endif

    std::filesystem::path home_;
    std::optional<std::string> old_utf8_;
};

}  // namespace

using namespace lubancode::app;

TEST_CASE("默认装配:主表有 agent/todo_write/基础工具,子表同级(含 agent 转发壳与 todo)") {
    ScopedHomeEnv home_guard;  // 主目录钉空:插件零挂载,下面的工具数口径才可信
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    ToolRuntime runtime(config, lubancode::cli::BuiltinTheme("plain"), backend, NoSkills(),
                        /*skills_segment=*/"", /*cwd_utf8=*/"/tmp", ToolRuntime::Options{});

    CHECK(runtime.main_registry().Find("agent") != nullptr);
    CHECK(runtime.main_registry().Find("todo_write") != nullptr);
    CHECK(runtime.main_registry().Find("read_file") != nullptr);
    CHECK(runtime.main_registry().Find("run_command") != nullptr);
    CHECK(runtime.main_registry().Find("ask_user") == nullptr);  // 交互独有,默认不挂
    // 同级能力(规格"产品不变量"):子表也挂 agent(AgentDispatchTool 转发壳)
    // 与 todo_write(RunTask 给每只任务换独占实例);递归治理靠 AgentTool 的
    // 深度账,不靠"子表没有 agent"。
    CHECK(runtime.sub_registry().Find("agent") != nullptr);
    CHECK(runtime.sub_registry().Find("agent")->name() == "agent");
    CHECK(runtime.sub_registry().Find("todo_write") != nullptr);
    CHECK(runtime.sub_registry().Find("read_file") != nullptr);
    // 子表 todo 板与主表各是各的:子代理不写 main 的待办。
    CHECK(runtime.sub_todo_state() != nullptr);
    CHECK(runtime.sub_todo_state() != runtime.todo_state());
    CHECK(runtime.explore_registry() == nullptr);  // 单发/默认无 Explore
    CHECK(runtime.agent_tool() != nullptr);
    CHECK(runtime.todo_state() != nullptr);
    CHECK(runtime.loaded_tools() != nullptr);
    // 空配置 + 零插件(ScopedHomeEnv 钉死):两张表只剩内置工具(主 12、子
    // 11,均低于默认阈值 20),口径直接钉数字——总数严格大于阈值才启用
    // (DeferralEnabled 的合同),所以 deferral 必关、tool_search 不挂、
    // 过滤直通。真机上用户装多少插件都进不来,这几条在谁的家目录下跑都
    // 是同一个数。
    CHECK(runtime.main_registry().All().size() <
          static_cast<std::size_t>(config.tool_search_threshold));
    CHECK(runtime.sub_registry().All().size() <
          static_cast<std::size_t>(config.tool_search_threshold));
    CHECK(runtime.main_deferral() == false);
    CHECK(runtime.sub_deferral() == false);
    CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    CHECK(runtime.sub_tool_filter()(*runtime.sub_registry().Find("read_file")));
    runtime.AttachMemoryTool(nullptr);  // 空指针安全
}

TEST_CASE("with_explore:Explore 只读硬边界,并挂到 agent 工具") {
    ScopedHomeEnv home_guard;  // 不读用户家目录:Explore 断言不吃全局插件
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
    CHECK(explore->Find("agent") == nullptr);        // 只读角色不派工(角色限制)
    CHECK(explore->Find("todo_write") == nullptr);   // 只读角色无 todo
    // Explore 表不进 MCP(空配置下无从验),但 agent 工具确实拿到了它。
    CHECK(runtime.agent_tool() != nullptr);
}

TEST_CASE("寿命:构造-查询-析构全程不崩,表地址稳定") {
    ScopedHomeEnv home_guard;  // 不读用户家目录:析构册也不碰真机 DLL
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
