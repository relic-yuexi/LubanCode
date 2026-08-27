// 领域命令 handler 的状态账钉子:/clear /title /resume 走 SessionCommandState,
// /send /peerperm 走 PeerCommandState。handler 借引用干活,这里用真
// SessionStore / 真 PeerRuntime(临时目录)对状态变化逐项断言。
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "sessions/session_store.hpp"
#include "api/backend.hpp"
#include "app/commands/command_flow.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
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

// /config 的 hooks 摘要:旧四枚数组之外,schema 2 events 也得按事件名数出
// 来,user/project 两层分开——数量与启动横幅、/hooks 的"已装载 N 条"对账
// (schema 2 ×N 的 N = 装载后的定义数,即全部 handler 数)。
TEST_CASE("/config hooks 摘要:schema 2 按事件名计数,分 user/project 两层") {
    lubancode::config::ConfigResult result;
    result.global_config_file_path = "C:/home/user/.lubancode/config.json";
    result.project_config_file_path = "C:/work/proj/.lubancode/config.json";

    auto group = [](const std::string& source_path, std::size_t handler_count) {
        lubancode::config::HookMatcherGroupConfig g;
        g.source_path = source_path;
        for (std::size_t i = 0; i < handler_count; ++i) {
            lubancode::config::HookHandlerConfig handler;
            handler.command = "echo hook" + std::to_string(i);
            g.hooks.push_back(handler);
        }
        return g;
    };
    // user 层:SessionStart×1、UserPromptSubmit×1;project 层:PreToolUse×1。
    result.config.hooks.events[lubancode::hooks::HookEvent::SessionStart].push_back(
        group(*result.global_config_file_path, 1));
    result.config.hooks.events[lubancode::hooks::HookEvent::UserPromptSubmit].push_back(
        group(*result.global_config_file_path, 1));
    result.config.hooks.events[lubancode::hooks::HookEvent::PreToolUse].push_back(
        group(*result.project_config_file_path, 1));

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    PrintConfigDiagnostics(result, std::nullopt, nullptr, nullptr);
    std::cout.rdbuf(old_buf);

    const std::string out = captured.str();
    const std::size_t hooks_line = out.find("hooks              = ");
    REQUIRE(hooks_line != std::string::npos);
    const std::string tail = out.substr(hooks_line, out.find('\n', hooks_line) - hooks_line);
    CHECK(tail.find("schema 2 ×3 (user×2, project×1)") != std::string::npos);
    CHECK(tail.find("SessionStart×1") != std::string::npos);
    CHECK(tail.find("UserPromptSubmit×1") != std::string::npos);
    CHECK(tail.find("PreToolUse×1") != std::string::npos);
}

TEST_CASE("/config hooks 摘要:只配旧四类时列 legacy,不冒出 schema 2 的 ×0") {
    lubancode::config::ConfigResult result;
    result.global_config_file_path = "C:/home/user/.lubancode/config.json";
    lubancode::config::HookEntry legacy_entry;
    legacy_entry.command = "echo old";
    legacy_entry.source_path = *result.global_config_file_path;
    result.config.hooks.pre_tool.push_back(legacy_entry);

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    PrintConfigDiagnostics(result, std::nullopt, nullptr, nullptr);
    std::cout.rdbuf(old_buf);

    const std::string out = captured.str();
    const std::size_t hooks_line = out.find("hooks              = ");
    REQUIRE(hooks_line != std::string::npos);
    const std::string tail = out.substr(hooks_line, out.find('\n', hooks_line) - hooks_line);
    CHECK(tail.find("legacy pre_tool×1") != std::string::npos);
    CHECK(tail.find("schema 2") == std::string::npos);
}

TEST_CASE("/config hooks 摘要:什么都没配时仍说未配置") {
    lubancode::config::ConfigResult result;
    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    PrintConfigDiagnostics(result, std::nullopt, nullptr, nullptr);
    std::cout.rdbuf(old_buf);

    const std::string out = captured.str();
    const std::size_t hooks_line = out.find("hooks              = ");
    REQUIRE(hooks_line != std::string::npos);
    const std::string tail = out.substr(hooks_line, out.find('\n', hooks_line) - hooks_line);
    CHECK(tail.find("schema 2") == std::string::npos);
    CHECK(tail.find("legacy") == std::string::npos);
    CHECK(tail.find("未配置") != std::string::npos);
}

TEST_CASE("/config:env 把运行端点换走时明说 provider unbound") {
    lubancode::config::ConfigResult result;
    lubancode::config::ProviderConfig provider;
    provider.name = "preset-a";
    provider.wire = lubancode::config::Wire::Responses;
    provider.base_url = "https://preset-a.example/v1";
    provider.model = "gpt-5.6";
    result.config.providers.push_back(provider);
    result.config.active_provider = "preset-a";
    result.config.wire = lubancode::config::Wire::Anthropic;
    result.config.base_url = "http://localhost:8001";
    result.config.model = "MiniCPM5-1B";
    result.sources.wire = lubancode::config::Source::LubancodeEnv;
    result.sources.base_url = lubancode::config::Source::LubancodeEnv;
    result.sources.model = lubancode::config::Source::LubancodeEnv;

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    PrintConfigDiagnostics(result, std::nullopt, nullptr, nullptr);
    std::cout.rdbuf(old_buf);
    CHECK(captured.str().find("provider_binding   = env override / unbound") != std::string::npos);
}

TEST_CASE("/title 状态账:建档前挂起,建档后补写,再设当场落事件行") {
    const auto dir = TempDir("title");
    lubancode::sessions::SessionStore store(dir.string());
    lubancode::tools::ToolRegistry registry;
    NullBackend backend;
    lubancode::agent::Agent loop(backend, registry, lubancode::agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});

    std::string title;
    bool title_pending = false;
    std::size_t persisted = 0;
    bool broken = false;
    std::string start_ts = "ts-1";
    lubancode::sessions::SessionMeta meta;
    const std::string sessions_dir = dir.string();
    const std::string wire = "anthropic";
    auto model = std::make_shared<std::string>("test-model");
    lubancode::cli::WorktreeSession worktree;
    std::vector<lubancode::peers::PeerEnvelope> ready;
    std::vector<lubancode::peers::PeerEnvelope> held;

    int compact_epoch = 0;
    SessionCommandState state{[](bool) {}, loop, store,  persisted, compact_epoch, meta, title,
                               title_pending, broken,  start_ts, nullptr, nullptr, nullptr,
                               nullptr,       &worktree, sessions_dir, wire, model};
    const lubancode::cli::Theme theme;

    // 建档前:/title 名字 只挂起,不写文件。
    CHECK(HandleTitleCommand(state, "场子一", theme) == CommandFlow::Continue);
    CHECK(title == "场子一");
    CHECK(title_pending);
    CHECK_FALSE(store.active());

    // 建档后(模拟首条消息落盘路径):再设标题当场补写事件行。
    lubancode::sessions::SessionMeta begin_meta;
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
    lubancode::sessions::SessionStore store(dir.string());
    lubancode::tools::ToolRegistry registry;
    NullBackend backend;
    lubancode::agent::Agent loop(backend, registry, lubancode::agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    lubancode::sessions::SessionMeta begin_meta;
    REQUIRE(store.Begin(begin_meta, "sess-clear-test"));
    store.AppendTitleEvent("旧标题");

    std::string title = "旧标题";
    bool title_pending = false;
    std::size_t persisted = 7;
    bool broken = true;  // 旧场的坏账,/clear 后该翻篇
    std::string start_ts = "ts-old";
    lubancode::sessions::SessionMeta meta;
    const std::string sessions_dir = dir.string();
    const std::string wire = "anthropic";
    auto model = std::make_shared<std::string>("test-model");
    lubancode::cli::WorktreeSession worktree;
    bool restarted = false;
    int epoch = 0;
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
        epoch,
        meta,
        title,
        title_pending,
        broken,
        start_ts,
        [&] { restarted = true; },
        nullptr,
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
    std::vector<lubancode::peers::PeerEnvelope> ready;
    std::vector<lubancode::peers::PeerEnvelope> held;
    std::optional<lubancode::peers::PeerRuntime> idle_runtime;
    const lubancode::cli::Theme theme;

    // 没起服务:/send /peerperm 只说明一句,不碰任何状态。
    PeerCommandState off{idle_runtime, false, ready, held};
    CHECK(HandleSendCommand(off, "alpha 在吗", theme) == CommandFlow::Continue);
    CHECK(HandlePeerpermCommand(off, "hold") == CommandFlow::Continue);
    CHECK(HandlePeersCommand(off, theme, false) == CommandFlow::Continue);

    // 起真服务(临时名册目录):空名册里 /send 找不到人;权限档可设可查。
    const auto dir = TempDir("peer");
    lubancode::peers::PeerRuntimeOptions options;
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
    CHECK(idle_runtime->tier() == lubancode::peers::PeerPermissionTier::Hold);
    CHECK(HandlePeerpermCommand(on, "refuse") == CommandFlow::Continue);
    CHECK(idle_runtime->tier() == lubancode::peers::PeerPermissionTier::Refuse);
    CHECK(HandlePeerpermCommand(on, "nonsense") == CommandFlow::Continue);  // 认不出的值不改档
    CHECK(idle_runtime->tier() == lubancode::peers::PeerPermissionTier::Refuse);

    idle_runtime->Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 会话管理命令(第四、五步):顶层 archive/unarchive/delete 的确认与消歧。
// 搬删经 SessionLifecycle(单测见 test_session_lifecycle.cpp),这里钉
// 接线层:确认屏(缺省取消/EOF 取消/y 才删)、--force、引用消歧、
// /sessions archived 的只读列表。
// ---------------------------------------------------------------------------

namespace {

std::string CmdPathUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 写一场带标题的会话(写完关柄)。返回文件路径。
// 文件名走 u8string:MSVC 下窄串拼 path 按 ANSI 代码页解码,中文 slug 写出
// 乱码名——Windows CI 上 delete 确认后删不掉的根因(写盘真名与 lifecycle
// 的 u8 拼名对不上,exists 落空回 NotFound)。夹具必须写真名。
std::filesystem::path CmdWriteSession(const std::filesystem::path& dir, const std::string& id,
                                      const std::string& title, const std::string& cwd) {
    lubancode::sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = cwd;
    meta.started_at = "2026-08-20 10:10:10";
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{"首句"});
    const std::string content = lubancode::sessions::SerializeSessionMeta(meta) + "\n" +
                                lubancode::sessions::SerializeSessionMessage(message, "2026-08-20 10:10:11") +
                                "\n" +
                                (title.empty() ? std::string()
                                               : lubancode::sessions::SerializeTitleEvent(title,
                                                                                      "2026-08-20 10:10:12") +
                                                     "\n");
    const std::u8string u8name(reinterpret_cast<const char8_t*>((id + ".jsonl").data()),
                               (id + ".jsonl").size());
    const auto path = dir / std::filesystem::path(u8name);
    {  // MSVC:写完显式关柄
        std::ofstream f(path, std::ios::binary);
        f << content;
    }
    return path;
}

}  // namespace

TEST_CASE("顶层 delete:确认屏 y 才删,空答/EOF/别答都取消不动盘") {
    const auto dir = TempDir("delete_confirm");
    const std::string sessions = CmdPathUtf8(dir);
    const lubancode::cli::Theme theme;
    const auto file = CmdWriteSession(dir, "20260820-101010-甲", "甲的场", "D:/房");
    const std::string id = "20260820-101010-甲";

    // 空答:取消,文件还在。
    CHECK(HandleSessionManagementCommand(sessions, /*kind=delete=*/2, id, false, theme,
                                         [] { return std::string(""); }) == 1);
    CHECK(std::filesystem::exists(file));

    // 别的答案(n):取消。
    CHECK(HandleSessionManagementCommand(sessions, 2, id, false, theme,
                                         [] { return std::string("n"); }) == 1);
    CHECK(std::filesystem::exists(file));

    // y:删掉。
    CHECK(HandleSessionManagementCommand(sessions, 2, id, false, theme,
                                         [] { return std::string("y"); }) == 0);
    CHECK_FALSE(std::filesystem::exists(file));

    // --force:跳过确认直接删(脚本路)。
    const auto file_b = CmdWriteSession(dir, "20260821-111111-乙", "", "D:/房");
    CHECK(HandleSessionManagementCommand(sessions, 2, "20260821-111111-乙", true, theme,
                                         [] { return std::string(""); }) == 0);
    CHECK_FALSE(std::filesystem::exists(file_b));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("顶层 delete:引用按标题解,重名列短 id 拒绝,缺参报用法") {
    const auto dir = TempDir("delete_ref");
    const std::string sessions = CmdPathUtf8(dir);
    const lubancode::cli::Theme theme;
    CmdWriteSession(dir, "20260820-101010-甲", "唯一标题", "D:/房");

    // 标题唯一命中。
    CHECK(HandleSessionManagementCommand(sessions, 2, "唯一标题", true, theme, nullptr) == 0);

    // 重名:两场同名,列短 id 拒绝(绝不猜一场)。
    CmdWriteSession(dir, "20260821-111111-乙", "同名", "D:/房");
    CmdWriteSession(dir, "20260822-121212-丙", "同名", "D:/房");
    CHECK(HandleSessionManagementCommand(sessions, 2, "同名", true, theme, nullptr) == 1);
    // 两场都还在(没连坐)。中文文件名按 u8 拼(窄串在 MSVC 下按 ANSI
    // 代码页解码,exists 会找错名)。
    const auto u8name_b = [](const char* utf8) {
        return std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(utf8), std::strlen(utf8)));
    };
    CHECK(std::filesystem::exists(dir / u8name_b("20260821-111111-乙.jsonl")));
    CHECK(std::filesystem::exists(dir / u8name_b("20260822-121212-丙.jsonl")));

    // 认不出。
    CHECK(HandleSessionManagementCommand(sessions, 2, "没这一场", true, theme, nullptr) == 1);

    // 缺参:报用法退 1。
    CHECK(HandleSessionManagementCommand(sessions, 2, "", true, theme, nullptr) == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("顶层 archive/unarchive 往返;归档后默认列表不见、archived 列表见") {
    const auto dir = TempDir("archive_cli");
    const std::string sessions = CmdPathUtf8(dir);
    const lubancode::cli::Theme theme;
    const auto file = CmdWriteSession(dir, "20260820-101010-甲", "甲的场", "D:/房");

    // archive(标题命中)。
    CHECK(HandleSessionManagementCommand(sessions, /*kind=archive=*/0, "甲的场", false, theme,
                                         nullptr) == 0);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(dir / "archive" /
                                  std::filesystem::path(std::u8string(
                                      reinterpret_cast<const char8_t*>("20260820-101010-甲.jsonl"),
                                      sizeof("20260820-101010-甲.jsonl") - 1))));

    // ListSessions(默认列表/--continue 的口径)不掺归档:根里没有 .jsonl。
    CHECK(lubancode::sessions::ListSessions(sessions).empty());

    // /sessions archived(只读入口)列得到。
    PrintSessionsCommand(sessions, "archived");

    // unarchive(完整 id)搬回根,老路又能列。
    CHECK(HandleSessionManagementCommand(sessions, /*kind=unarchive=*/1, "20260820-101010-甲",
                                         false, theme, nullptr) == 0);
    CHECK(std::filesystem::exists(file));
    CHECK(lubancode::sessions::ListSessions(sessions).size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// /model 跨 provider 收口 + provider 切换同路(ExecuteProviderSwitch)
// ---------------------------------------------------------------------------

namespace {

// 隔离 HOME 的 RAII:ExecuteProviderSwitch 会把 active_provider 写进
// <HOME>/.lubancode/config.json,别碰开发机真配置。Windows 换 USERPROFILE,
// POSIX 换 HOME——platform::HomeDir 只认这两个。
class HomeEnvGuard {
public:
    explicit HomeEnvGuard(const std::filesystem::path& home) {
#ifdef _WIN32
        const char* old = std::getenv("USERPROFILE");
        if (old != nullptr) {
            old_.emplace(old);
        }
        _putenv_s("USERPROFILE", CmdPathUtf8(home).c_str());
#else
        const char* old = std::getenv("HOME");
        if (old != nullptr) {
            old_.emplace(old);
        }
        setenv("HOME", home.c_str(), /*replace=*/1);
#endif
    }
    ~HomeEnvGuard() {
#ifdef _WIN32
        if (old_.has_value()) {
            _putenv_s("USERPROFILE", old_->c_str());
        } else {
            _putenv_s("USERPROFILE", "");
        }
#else
        if (old_.has_value()) {
            setenv("HOME", old_->c_str(), /*replace=*/1);
        } else {
            unsetenv("HOME");
        }
#endif
    }
    HomeEnvGuard(const HomeEnvGuard&) = delete;
    HomeEnvGuard& operator=(const HomeEnvGuard&) = delete;

private:
    std::optional<std::string> old_;
};

}  // namespace

TEST_CASE("ModelProviderHopFor:当前家有的模型不报跨家,归属只认配置里真有的那家") {
    lubancode::config::ModelCatalog catalog;
    lubancode::config::ModelCatalogEntry openai;
    openai.provider_id = "openai";
    openai.slug = "gpt-x";
    lubancode::config::ModelCatalogEntry cc;
    cc.provider_id = "ccmoon";
    cc.slug = "gpt-x";  // 同名多家:官方 openai 与中转家 ccmoon 都列着
    lubancode::config::ModelCatalogEntry own;
    own.provider_id = "local";
    own.slug = "qwen-y";
    lubancode::config::ModelCatalogEntry relay;
    relay.provider_id = "local";
    relay.slug = "gpt-x";  // 当前家(自建中转)目录里也列着 openai 家的模型名
    lubancode::config::ModelCatalogEntry mine;
    mine.slug = "anonymous-model";  // 用户自写条目:没写归属,全局覆盖
    lubancode::config::ModelCatalogEntry cc_lm_official;
    cc_lm_official.provider_id = "openai";  // 没配的家排前
    cc_lm_official.slug = "cc-lm";
    lubancode::config::ModelCatalogEntry cc_lm;
    cc_lm.provider_id = "ccmoon";  // 配了的家排后
    cc_lm.slug = "cc-lm";
    lubancode::config::ModelCatalogEntry other;
    other.provider_id = "gemini";
    other.slug = "solo-model";
    // 活列表落痕写的用户条目:带归属的 models.json 条目,"这家确实用过这
    // 模型"的凭据。当前家(local)目录条目没有、官方 openai 条目列着的
    // 模型,凭它也命中本家。
    lubancode::config::ModelCatalogEntry traced;
    traced.provider_id = "local";
    traced.slug = "relay-only";
    catalog.models = {openai, cc, own, relay, mine, cc_lm_official, cc_lm, other, traced};

    lubancode::config::ProviderConfig p_cc{
        .name = "ccmoon",
        .base_url = "https://cc.test/v1",
        .wire = lubancode::config::Wire::ChatCompletions,
        .key_env = "CC_KEY",
        .api_key = "",
        .model = "gpt-x",
    };
    std::vector<lubancode::config::ProviderConfig> providers = {p_cc};

    // 当前家 local 的模型(目录条目):本家切换,零提示零动作。
    CHECK_FALSE(ModelProviderHopFor(catalog, providers, "local", "qwen-y").has_value());
    // 中转家场景(验收返件):同名条目里 openai 官方排前、当前家 local
    // 排后,当前家列着就是本家,不拿先出现的 openai 条目报跨家。
    CHECK_FALSE(ModelProviderHopFor(catalog, providers, "local", "gpt-x").has_value());
    // 用户自写条目(归属不明)压过一切:本家,不猜。
    CHECK_FALSE(ModelProviderHopFor(catalog, providers, "local", "anonymous-model").has_value());
    // 活列表落痕条目(带归属的用户条目):这家确实用过,本家,零提示。
    CHECK_FALSE(ModelProviderHopFor(catalog, providers, "local", "relay-only").has_value());
    // 目录没有的模型(手敲的裸名):不猜归属。
    CHECK_FALSE(ModelProviderHopFor(catalog, providers, "local", "who-am-i").has_value());
    // 真跨家:当前家没有、别家条目列了、配置里有那家 → 切那家。没配的
    // openai 条目排前也拦不住——归属只认配置里真有的那家。
    const auto hop = ModelProviderHopFor(catalog, providers, "local", "cc-lm");
    REQUIRE(hop.has_value());
    CHECK(hop->provider_id == "ccmoon");
    CHECK(hop->configured);
    // 同名条目全是没配的家(只列在官方 gemini 条目下):提示属谁未配置,
    // configured=false 由调用方提示并保持本家。
    const auto hop2 = ModelProviderHopFor(catalog, providers, "local", "solo-model");
    REQUIRE(hop2.has_value());
    CHECK(hop2->provider_id == "gemini");
    CHECK_FALSE(hop2->configured);
}

TEST_CASE("ExecuteProviderSwitch:整套连接字段连同 backend/会话状态一起换家") {
    const auto home = TempDir("pswitch_home");
    HomeEnvGuard guard(home);

    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::Responses;
    config.base_url = "http://127.0.0.1:49821/v1";
    config.model = "old-model";
    config.auth_mode = lubancode::config::ProviderAuthMode::None;
    lubancode::config::ProviderConfig ccmoon{
        .name = "ccmoon",
        .base_url = "https://cc.test/v1",
        .wire = lubancode::config::Wire::ChatCompletions,
        .key_env = "CC_KEY",
        .api_key = "sk-cc",
        .model = "gpt-x",
        .model_reasoning_effort = "high",
        .context_window_tokens = 64000,
    };
    ccmoon.auth = lubancode::config::ProviderAuthMode::Inline;
    lubancode::config::ProviderConfig local{
        .name = "local",
        .base_url = "http://127.0.0.1:49821/v1",
        .wire = lubancode::config::Wire::Responses,
        .model = "old-model",
    };
    local.auth = lubancode::config::ProviderAuthMode::None;
    config.providers = {local, ccmoon};

    std::string active_provider = "local";
    RebuildableBackend backend(config);
    std::string session_wire = lubancode::config::ProviderWireName(config.wire);
    auto current_model = std::make_shared<std::string>("old-model");
    auto current_think = std::make_shared<std::string>("low");
    auto current_instructions = std::make_shared<std::string>("OLD");
    lubancode::cli::ContextTracker tracker(128000);
    lubancode::config::ModelCatalog catalog;  // 空目录:ApplyModelCatalog 全空应用
    lubancode::agent::PromptOptions prompt_options;
    int rebuild_count = 0;
    const auto rebuild_loop = [&](bool preserve_history) {
        CHECK(preserve_history);
        ++rebuild_count;
    };
    const lubancode::cli::Theme theme;
    lubancode::config::Source source = lubancode::config::Source::Default;

    CHECK(ExecuteProviderSwitch("ccmoon", /*switch_model=*/"", config, active_provider, backend,
                                session_wire, current_model, current_think, tracker,
                                current_instructions, catalog, prompt_options, rebuild_loop,
                                /*is_console=*/false, theme, /*active_provider_write_path=*/std::nullopt,
                                source));

    // 顶层镜像字段全套换成 ccmoon(provider add 收尾切与 /model 跨家切
    // 共用的就是这一条路)。
    CHECK(config.wire == lubancode::config::Wire::ChatCompletions);
    CHECK(config.base_url == "https://cc.test/v1");
    CHECK(config.auth_token == "sk-cc");
    CHECK(config.model == "gpt-x");
    CHECK(config.context_window_tokens == 64000);
    CHECK(config.active_provider == "ccmoon");
    CHECK(*current_model == "gpt-x");
    CHECK(*current_think == "high");  // provider 声明的档位跟着上
    CHECK(active_provider == "ccmoon");
    CHECK(session_wire == "openai-chat-completions");
    CHECK(prompt_options.wire == "openai-chat-completions");
    CHECK(tracker.window_tokens() == 64000);
    CHECK(rebuild_count == 1);
    CHECK(source == lubancode::config::Source::GlobalConfigFile);
    // 记忆落进隔离 HOME:全局 config.json 里 active_provider 已写回。
    std::error_code ec;
    const bool remembered =
        std::filesystem::exists(home / ".lubancode" / std::filesystem::path("config.json"), ec);
    CHECK(remembered);

    // 名字找不着:报一行、返回 false,任何状态都不动。
    const std::string wire_before = session_wire;
    CHECK_FALSE(ExecuteProviderSwitch("no-such", "", config, active_provider, backend, session_wire,
                                      current_model, current_think, tracker, current_instructions,
                                      catalog, prompt_options, rebuild_loop, false, theme,
                                      std::nullopt, source));
    CHECK(session_wire == wire_before);
    CHECK(rebuild_count == 1);

    std::filesystem::remove_all(home, ec);
}



// ---------------------------------------------------------------------------
// ccmoon 真机巡检单 P1:活列表证据先行(先落痕再判定),跨家判定只吃
// 权威且唯一的映射。钉住的正是真机现场:当前家(ccmoon)真机列出
// gpt-5.6-luna,静态目录却把这名字归给没配置的 openai 家。
// ---------------------------------------------------------------------------

TEST_CASE("活列表证据:先落痕再判定,静态指向别家的首选也零跨家提示") {
    lubancode::config::ModelCatalog catalog;
    lubancode::config::ModelCatalogEntry official;
    official.provider_id = "openai";  // 静态目录:官方条目(没配置)
    official.slug = "gpt-5.6-luna";
    catalog.models = {official};
    const std::vector<lubancode::config::ProviderConfig> no_providers;

    // 旧病根(判定序没倒过来时):静态归属直接把这名字报成"属 openai
    // 家(未配置)"——模型明明正由当前家真机列出、随后也由当前家调通。
    const auto before = ModelProviderHopFor(catalog, no_providers, "ccmoon", "gpt-5.6-luna");
    REQUIRE(before.has_value());
    CHECK(before->provider_id == "openai");
    CHECK_FALSE(before->configured);
    CHECK_FALSE(before->ambiguous);

    // 证据先行(RememberModelChoiceInCatalog 落的用户条目,带当前家
    // 归属):同一张目录再判,第一步就认本家,零提示零动作——不许靠
    // 选第二回才自愈。
    lubancode::config::ModelCatalogEntry remembered;
    remembered.provider_id = "ccmoon";
    remembered.slug = "gpt-5.6-luna";
    catalog.models.push_back(remembered);
    CHECK_FALSE(ModelProviderHopFor(catalog, no_providers, "ccmoon", "gpt-5.6-luna").has_value());
}

TEST_CASE("跨家判定:多家已配目录都列同名 → ambiguous,不自动跳") {
    lubancode::config::ModelCatalog catalog;
    lubancode::config::ModelCatalogEntry via_gateway;
    via_gateway.provider_id = "gateway";
    via_gateway.slug = "shared-model";
    lubancode::config::ModelCatalogEntry via_relay;
    via_relay.provider_id = "ccmoon";
    via_relay.slug = "shared-model";
    catalog.models = {via_gateway, via_relay};
    std::vector<lubancode::config::ProviderConfig> providers = {
        {.name = "gateway", .base_url = "https://gw.test/v1", .wire = lubancode::config::Wire::ChatCompletions,
         .key_env = "GW_KEY", .api_key = "", .model = "shared-model"},
        {.name = "ccmoon", .base_url = "https://cc.test/v1", .wire = lubancode::config::Wire::ChatCompletions,
         .key_env = "CC_KEY", .api_key = "", .model = "shared-model"},
    };

    // 自动跳家只吃权威且唯一的映射:两家都配了就不跳,报 ambiguous,
    // provider_id 把各家名拼上,调用方提示并留在本家。
    const auto hop = ModelProviderHopFor(catalog, providers, "local", "shared-model");
    REQUIRE(hop.has_value());
    CHECK(hop->ambiguous);
    CHECK_FALSE(hop->configured);
    CHECK(hop->provider_id.find("gateway") != std::string::npos);
    CHECK(hop->provider_id.find("ccmoon") != std::string::npos);

    // 唯一配置家(另一家没配)照旧自动跳:权威且唯一。
    lubancode::config::ModelCatalogEntry solo_relay;
    solo_relay.provider_id = "ccmoon";
    solo_relay.slug = "solo-hop";
    lubancode::config::ModelCatalogEntry solo_official;
    solo_official.provider_id = "openai";  // 没配,排前也不拦
    solo_official.slug = "solo-hop";
    lubancode::config::ModelCatalog solo_catalog;
    solo_catalog.models = {solo_official, solo_relay};
    const auto solo = ModelProviderHopFor(solo_catalog, providers, "local", "solo-hop");
    REQUIRE(solo.has_value());
    CHECK(solo->configured);
    CHECK(solo->provider_id == "ccmoon");
}
