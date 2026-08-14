// 领域命令 handler 的状态账钉子:/clear /title /resume 走 SessionCommandState,
// /send /peerperm 走 PeerCommandState。handler 借引用干活,这里用真
// SessionStore / 真 PeerRuntime(临时目录)对状态变化逐项断言。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "app/commands/command_flow.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "cli/theme.hpp"
#include "tools/registry.hpp"

namespace {

class NullBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        return {};
    }
};

std::filesystem::path TempDir(const std::string& tag) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("lubancode_cmd_test_" + tag + "_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

using namespace lubancode::app;

TEST_CASE("/title 状态账:建档前挂起,建档后补写,再设当场落事件行") {
    const auto dir = TempDir("title");
    lubancode::agent::SessionStore store(dir.string());
    lubancode::tools::ToolRegistry registry;
    NullBackend backend;
    lubancode::agent::AgentLoop loop(backend, registry, "test-model", "system");

    std::string title;
    bool title_pending = false;
    std::size_t persisted = 0;
    bool broken = false;
    std::string start_ts = "ts-1";
    lubancode::agent::SessionMeta meta;
    const std::string sessions_dir = dir.string();
    const std::string wire = "anthropic";
    auto model = std::make_shared<std::string>("test-model");
    lubancode::cli::WorktreeSession worktree;
    std::vector<lubancode::agent::PeerEnvelope> ready;
    std::vector<lubancode::agent::PeerEnvelope> held;

    SessionCommandState state{[](bool) {}, loop, store,  persisted, meta, title, title_pending,
                               broken,       start_ts, nullptr, nullptr, nullptr, &worktree,
                               sessions_dir, wire,     model};
    const lubancode::cli::Theme theme;

    // 建档前:/title 名字 只挂起,不写文件。
    CHECK(HandleTitleCommand(state, "场子一", theme) == CommandFlow::Continue);
    CHECK(title == "场子一");
    CHECK(title_pending);
    CHECK_FALSE(store.active());

    // 建档后(模拟首条消息落盘路径):再设标题当场补写事件行。
    lubancode::agent::SessionMeta begin_meta;
    begin_meta.wire = wire;
    REQUIRE(store.Begin(begin_meta, "sess-title-test"));
    CHECK(HandleTitleCommand(state, "场子二", theme) == CommandFlow::Continue);
    CHECK(title == "场子二");
    // 注意:pending 只由落盘路径(PersistNewMessages)与 /resume 清掉,
    // 这里带着旧 true 过来是搬家前的原语义,handler 照抄不私自翻新。
    CHECK(title_pending);
    CHECK(store.active());

    // 裸敲:不改任何状态。
    CHECK(HandleTitleCommand(state, "", theme) == CommandFlow::Continue);
    CHECK(title == "场子二");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("/clear 状态账:重建不带历史、存档翻篇、标题清空") {
    const auto dir = TempDir("clear");
    lubancode::agent::SessionStore store(dir.string());
    lubancode::tools::ToolRegistry registry;
    NullBackend backend;
    lubancode::agent::AgentLoop loop(backend, registry, "test-model", "system");
    lubancode::agent::SessionMeta begin_meta;
    REQUIRE(store.Begin(begin_meta, "sess-clear-test"));
    store.AppendTitleEvent("旧标题");

    std::string title = "旧标题";
    bool title_pending = false;
    std::size_t persisted = 7;
    bool broken = true;  // 旧场的坏账,/clear 后该翻篇
    std::string start_ts = "ts-old";
    lubancode::agent::SessionMeta meta;
    const std::string sessions_dir = dir.string();
    const std::string wire = "anthropic";
    auto model = std::make_shared<std::string>("test-model");
    lubancode::cli::WorktreeSession worktree;
    bool restarted = false;
    int rebuild_calls = 0;
    bool rebuild_preserve_history = true;

    SessionCommandState state{
        [&](bool preserve) {
            ++rebuild_calls;
            rebuild_preserve_history = preserve;
        },
        loop,
        store,
        persisted,
        meta,
        title,
        title_pending,
        broken,
        start_ts,
        [&] { restarted = true; },
        nullptr,
        nullptr,
        &worktree,
        sessions_dir,
        wire,
        model};
    const lubancode::cli::Theme theme;
    lubancode::config::Config config;  // 管道场景 spinner 恒 false,不清屏

    CHECK(HandleClearCommand(state, config, theme, /*spinner_enabled=*/false) == CommandFlow::Continue);
    CHECK(rebuild_calls == 1);
    CHECK_FALSE(rebuild_preserve_history);  // /clear 丢历史重建
    CHECK(restarted);                       // project memory 源的善后跑了
    CHECK_FALSE(store.active());            // 存档翻篇
    CHECK(persisted == 0);
    CHECK_FALSE(broken);
    CHECK(title.empty());
    CHECK_FALSE(title_pending);
    CHECK(start_ts != "ts-old");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("/send 与 /peerperm 状态账:off 档、空名册、权限档切换") {
    std::vector<lubancode::agent::PeerEnvelope> ready;
    std::vector<lubancode::agent::PeerEnvelope> held;
    std::optional<lubancode::agent::PeerRuntime> idle_runtime;
    const lubancode::cli::Theme theme;

    // 没起服务:/send /peerperm 只说明一句,不碰任何状态。
    PeerCommandState off{idle_runtime, false, ready, held};
    CHECK(HandleSendCommand(off, "alpha 在吗", theme) == CommandFlow::Continue);
    CHECK(HandlePeerpermCommand(off, "hold") == CommandFlow::Continue);
    CHECK(HandlePeersCommand(off, theme, false) == CommandFlow::Continue);

    // 起真服务(临时名册目录):空名册里 /send 找不到人;权限档可设可查。
    const auto dir = TempDir("peer");
    lubancode::agent::PeerRuntimeOptions options;
    options.registry_dir = dir;
    options.name = "solo";
    options.cwd = dir.string();
    options.permission_mode = [] { return 0; };
    idle_runtime.emplace(std::move(options));
    std::string error;
    REQUIRE(idle_runtime->Start(&error));

    PeerCommandState on{idle_runtime, true, ready, held};
    CHECK(HandleSendCommand(on, "who-is-this 你好", theme) == CommandFlow::Continue);  // 名册没人
    CHECK(HandleSendCommand(on, "no-space", theme) == CommandFlow::Continue);          // 缺正文
    CHECK(HandlePeerpermCommand(on, "hold") == CommandFlow::Continue);
    CHECK(idle_runtime->tier() == lubancode::agent::PeerPermissionTier::Hold);
    CHECK(HandlePeerpermCommand(on, "refuse") == CommandFlow::Continue);
    CHECK(idle_runtime->tier() == lubancode::agent::PeerPermissionTier::Refuse);
    CHECK(HandlePeerpermCommand(on, "nonsense") == CommandFlow::Continue);  // 认不出的值不改档
    CHECK(idle_runtime->tier() == lubancode::agent::PeerPermissionTier::Refuse);

    idle_runtime->Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
