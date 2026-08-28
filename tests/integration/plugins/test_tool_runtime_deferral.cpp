// tool deferral 的全局插件对账(真机实测单 P1-3):用户全局目录
// <主目录>/.lubancode/plugins 真装了插件、工具总数越过阈值时,tool_search
// 延迟挂载确实启用——这是产品行为,该有人对账,只是不归单测管(单测册
// test_tool_runtime.cpp 把主目录钉在空临时目录,读不到用户家目录,用户装
// 0、1、10 枚结果都不变;本册反着来:主目录想装几枚装几枚,验装配链对
// 插件数敏感)。
//
// 插件用 process 形态(plugin.json):装配只解析 manifest、造 adapter,不
// 起子进程,command 填什么都不会被执行;一枚 manifest 可声明多枚工具,
// 压阈值的余量比一文件一工具的 Lua 插件宽。主目录侧扫描无信任门(信任
// 门只管项目级),manifest 合法即挂。

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
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

// 用户目录闸:与单测册 test_tool_runtime.cpp 同一条路(构造改
// USERPROFILE/HOME、析构还原),那边的注释这里不重抄,详见彼处。集成册
// 用它不是隔离,是摆布:主目录指到调用方造好的临时目录(装了几枚插件
// 是调用方的事,这里不动目录一根汗毛),跑完还原环境,真家目录不碰。
class ScopedHomeEnv {
public:
    explicit ScopedHomeEnv(const std::filesystem::path& home) : home_(home) {
        const std::string value = lubancode::platform::PathToUtf8(home_);
#ifdef _WIN32
        const auto raw = lubancode::platform::GetEnvVar("USERPROFILE");
        old_utf8_ = raw.has_value() ? std::optional<std::string>(lubancode::platform::AcpBytesToUtf8(*raw))
                                    : std::nullopt;
        SetWindowsUserProfile(value);
#else
        const char* raw = std::getenv("HOME");
        old_utf8_ = raw != nullptr ? std::optional<std::string>(raw) : std::nullopt;
        setenv("HOME", value.c_str(), /*replace=*/1);
#endif
    }
    ~ScopedHomeEnv() {
#ifdef _WIN32
        SetWindowsUserProfile(old_utf8_.value_or(std::string()));  // 空串即移除
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
    static void SetWindowsUserProfile(const std::string& utf8_value) {
        const std::wstring entry = L"USERPROFILE=" + lubancode::platform::Utf8ToWide(utf8_value);
        _wputenv(entry.c_str());
    }
#endif

    std::filesystem::path home_;
    std::optional<std::string> old_utf8_;
};

// 往 <home>/.lubancode/plugins/ 装 count 枚 process 插件,每枚声明
// tools_per_plugin 件工具。工具名带插件序号,跨插件不重名(重名会被
// ScanPluginDirectories 整件拒掉)。
void InstallGlobalPlugins(const std::filesystem::path& home, int count, int tools_per_plugin) {
    const std::filesystem::path plugins = home / ".lubancode" / "plugins";
    for (int i = 0; i < count; ++i) {
        const std::filesystem::path dir = plugins / ("defl_" + std::to_string(i));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string tools_json;
        for (int t = 0; t < tools_per_plugin; ++t) {
            tools_json += (t > 0 ? "," : "");
            tools_json += "{\"name\": \"t" + std::to_string(t) +
                          "\", \"description\": \"deferral 对账用的占位工具\", "
                          "\"input_schema\": {\"type\": \"object\"}}";
        }
        // command 填 "echo":装配只解析 manifest 不起进程,这一格不会被
        // 执行;相对可执行名也不触发 ${plugin_dir} 的路径圈禁。
        const std::string manifest = "{\"manifest_version\": 1, \"id\": \"defl_" + std::to_string(i) +
                                     "\", \"version\": \"1.0.0\", \"language\": \"shell\", "
                                     "\"runtime\": {\"kind\": \"process\", \"command\": \"echo\"}, "
                                     "\"tools\": [" +
                                     tools_json + "]}";
        std::ofstream out(dir / "plugin.json", std::ios::binary);
        out << manifest;
    }
}

// 一轮三档共用:装 count 枚、构造、还账。
void CheckDeferralForPluginCount(int count) {
    CAPTURE(count);
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() / ("lubancode_deferral_home_" + std::to_string(count));
    InstallGlobalPlugins(home, count, /*tools_per_plugin=*/3);
    ScopedHomeEnv home_guard(home);
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    lubancode::app::ToolRuntime runtime(config, lubancode::cli::BuiltinTheme("plain"), backend, NoSkills(),
                                         /*skills_segment=*/"", /*cwd_utf8=*/"/tmp",
                                         lubancode::app::ToolRuntime::Options{});

    // 插件真挂上了(manifest 一枚不落),不是空转的断言。
    REQUIRE(runtime.process_manifests().size() == static_cast<std::size_t>(count));

    const auto threshold = static_cast<std::size_t>(config.tool_search_threshold);  // 默认 20
    const std::size_t main_size = runtime.main_registry().All().size();
    const std::size_t sub_size = runtime.sub_registry().All().size();
    INFO("main tools: ", main_size, " sub tools: ", sub_size, " threshold: ", threshold);
    // 口径钉死:总数(不含 tool_search 自身,装配在注册它之前数的)严格
    // 大于阈值才启用——DeferralEnabled 的合同,主表子表同一条。
    CHECK((main_size > threshold) == runtime.main_deferral());
    CHECK((sub_size > threshold) == runtime.sub_deferral());

    if (count >= 10) {
        // 十枚 × 三工具 = 30 枚插件工具,加上内置一批,两张表都压过线:
        // 产品行为对账——全局插件多了,延迟挂载真的开。
        CHECK(runtime.main_deferral());
        CHECK(runtime.sub_deferral());
        CHECK(runtime.main_registry().Find("tool_search") != nullptr);
        CHECK(runtime.sub_registry().Find("tool_search") != nullptr);
        // 插件工具在表里、标了 deferred,过滤器把它拦在 tools 数组外;
        // 内置工具不 deferred,照旧放行——这就是延迟挂载的分界线。
        lubancode::tools::Tool* plugin_tool = runtime.main_registry().Find("plugin__defl_1__t0");
        REQUIRE(plugin_tool != nullptr);
        CHECK(plugin_tool->deferred());
        CHECK_FALSE(runtime.main_tool_filter()(*plugin_tool));
        CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    } else {
        // 0 枚与 1 枚(三工具)都还在线下:延迟不启用,tool_search 不挂,
        // 与单测册的口径一致——装得少不改变直挂行为。
        CHECK_FALSE(runtime.main_deferral());
        CHECK_FALSE(runtime.sub_deferral());
        CHECK(runtime.main_registry().Find("tool_search") == nullptr);
        CHECK(runtime.sub_registry().Find("tool_search") == nullptr);
        CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    }
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

}  // namespace

using namespace lubancode::app;

TEST_CASE("全局插件数与 tool deferral:0、1 枚不触发,10 枚越线触发") {
    CheckDeferralForPluginCount(0);
    CheckDeferralForPluginCount(1);
    CheckDeferralForPluginCount(10);
}
