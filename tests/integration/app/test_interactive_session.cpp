// InteractiveSession 真构造、真析构的集成钉子:走 RunInteractiveSession 的
// 正门,构造装配(工具全栈/回调注册/存档材料)→ 主循环第一圈 stdin EOF →
// 退场 → 析构关门,全程不许崩、不许挂。不发任何网络请求(退场发生在
// 第一次 ReadLine,backend 指向连不上的回环口也只是备而不用)。
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "app/interactive_session.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/settings_local.hpp"

#if defined(_WIN32)
const char* kNullDevice = "nul";
#else
const char* kNullDevice = "/dev/null";
#endif

TEST_CASE("InteractiveSession:真构造、EOF 退场、真析构") {
    // cwd 挪进临时目录:会话构造里的陈房清扫按 cwd 找 git 根,临时目录没
    // 有 .git,整段天然跳过——测试绝不碰真实仓库的 agent worktree。
    const std::filesystem::path old_cwd = std::filesystem::current_path();
    std::error_code ec;
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path(ec) /
        ("lubancode_session_test_" + std::to_string(::rand()));
    std::filesystem::create_directories(temp_root, ec);
    std::filesystem::current_path(temp_root, ec);

    // stdin 指到空设备:ReadLine 第一圈就 EOF,主循环当场退场。
    std::FILE* redirected = nullptr;
#if defined(_WIN32)
    if (freopen_s(&redirected, kNullDevice, "r", stdin) != 0) {
        redirected = nullptr;
    }
#else
    redirected = std::freopen(kNullDevice, "r", stdin);
#endif
    REQUIRE(redirected != nullptr);

    lubancode::config::ConfigResult config_result;
    config_result.config.wire = lubancode::config::Wire::Anthropic;
    config_result.config.base_url = "http://127.0.0.1:9";  // 备而不用,不发请求
    config_result.config.auth_token = "session-test";
    config_result.config.model = "session-test-model";
    lubancode::config::ModelCatalog model_catalog;
    lubancode::config::SettingsLocal settings_local;
    lubancode::cli::Theme theme;  // 默认即 plain,无 ANSI 输出

    const lubancode::app::InteractiveSessionOptions options{
        config_result, theme, model_catalog, settings_local,
        /*auto_confirm=*/false, /*persona=*/std::string(),
        /*spinner_enabled=*/false, /*continue_last=*/false};
    CHECK(lubancode::app::RunInteractiveSession(options) == 0);

    // 回到原目录,收走临时目录;析构已在 RunInteractiveSession 返回前跑完
    // (收件点摘除、peer 停、UI 回调清挂),这里不崩就是活着回来了。
    std::filesystem::current_path(old_cwd, ec);
    std::filesystem::remove_all(temp_root, ec);
}
