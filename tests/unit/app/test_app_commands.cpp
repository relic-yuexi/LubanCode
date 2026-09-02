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
#include "api/backend.hpp"
#include "app/commands/command_flow.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/identity.hpp"
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

TEST_CASE("/skills 按来源分组,说明另起一行,不把长路径铺满屏") {
    std::vector<lubancode::tools::SkillMeta> skills = {
        {"project-skill", "项目说明", "D:/very/long/project/path", "项目级"},
        {"official-skill", "官方说明", "D:/very/long/official/path", "官方"},
        {"home-skill", "主目录说明", "D:/very/long/home/path", "主目录级"},
    };
    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    PrintSkillsCommand(skills, "D:/work", std::optional<std::string>("C:/home"));
    std::cout.rdbuf(old_buf);

    const std::string out = captured.str();
    CHECK(out.find("技能 · 3") != std::string::npos);
    CHECK(out.find("项目级 · 1") != std::string::npos);
    CHECK(out.find("主目录级 · 1") != std::string::npos);
    CHECK(out.find("官方 · 1") != std::string::npos);
    CHECK(out.find("D:/very/long") == std::string::npos);
    CHECK(out.find("/skill list") != std::string::npos);
}

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

// (P0-6:/title 的旧档挂起/补写用例已删——标题真账走 control.title.changed,
// 现行路在 HandleSlashTitle 的 trajectory 分支。)

TEST_CASE("/clear 状态账:SessionCommandState 聚合可装,标题活值翻篇") {
    lubancode::tools::ToolRegistry registry;
    NullBackend backend;
    lubancode::agent::Agent loop(backend, registry, lubancode::agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});

    std::string title = "旧标题";
    std::string start_ts = "ts-old";
    const std::string wire = "anthropic";
    auto model = std::make_shared<std::string>("test-model");
    lubancode::cli::WorktreeSession worktree;
    int epoch = 0;

    SessionCommandState state{
        [](bool) {},
        loop,
        epoch,
        title,
        start_ts,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &worktree,
        wire,
        model,
        nullptr};
    // P0-6:聚合字段瘦身后的形状钉住(编译即验);旧档字段(store/
    // persisted/meta/title_pending/broken/sessions_dir)不再存在。
    CHECK(&state.loop == &loop);
    CHECK(state.title == "旧标题");
    CHECK(state.start_ts == "ts-old");
    (void)wire;
    std::error_code ec;
    (void)ec;
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
    options.permission_mode = [] { return lubancode::ApprovalMode::Default; };
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

// (P0-6:CmdWriteSession——旧档造场——已删;顶层 archive/delete 的
// 用例走 WorkspaceSessionsFixture(trajectory 新账)。)

}  // namespace

// ---------------------------------------------------------------------------
// P0-2(Trajectory 升为唯一 Session):顶层 delete/archive/unarchive 走
// workspace 新账——索引解引用、确认屏、lifecycle + 状态图 + tombstone。
// ---------------------------------------------------------------------------

namespace {

// 一只临时 workspaces 根 + 一场封了口、带标题的会话。
class WorkspaceSessionsFixture {
public:
    // TempDir 的路径按进程内 rand 序列生成,跨进程会同址——先清场再住,
    // 上次跑剩的账不掺进这次(引用解析的歧义测试尤其忌脏账)。
    explicit WorkspaceSessionsFixture(const std::string& tag)
        : dir_(TempDir(tag)), root_(dir_ / "workspaces") {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    ~WorkspaceSessionsFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    // 造一场:turn 一问一答 + 可选标题,正常封口(closed)。
    std::string MakeSession(const std::string& title, const std::string& cwd_utf8) {
        std::error_code ec;
        std::filesystem::create_directories(dir_ / "repo", ec);
        lubancode::runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = root_;
        options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir_ / "repo");
        options.lubancode_version = "test";
        options.launch_cwd = cwd_utf8;
        auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(options);
        REQUIRE(ledger.has_value());
        if (!title.empty()) {
            ledger->RecordTitleChanged(title, "");
        }
        auto bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        lubancode::api::Message input;
        input.role = lubancode::api::Role::User;
        input.content.push_back(lubancode::api::TextBlock{"问一句 " + title});
        bridge->RecordInput(input);
        lubancode::api::Request prepared;
        prepared.model = "m1";
        const std::string request =
            bridge->OnRequestPrepared(prepared, lubancode::agent::RequestPreparedContext{});
        REQUIRE_FALSE(request.empty());
        bridge->OnRequestSent(request);
        lubancode::api::Message answer;
        answer.role = lubancode::api::Role::Assistant;
        answer.content.push_back(lubancode::api::TextBlock{"回一句"});
        REQUIRE(bridge->OnOutputCompleted(request, answer, "end_turn", "resp-1"));
        bridge->EndTurn(true, false, "done");
        const std::string id = ledger->session_id();
        (void)ledger->CloseSession("exit");
        return id;
    }
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path SessionDirOf(const std::string& session_id) const {
        // 单一 workspace(fallback 身份),目录枚举找场次。
        for (const auto& workspace : std::filesystem::directory_iterator(root_)) {
            const auto candidate = workspace.path() / "sessions" / std::filesystem::path(
                                                               std::u8string(reinterpret_cast<const char8_t*>(
                                                                                 session_id.data()),
                                                                             session_id.size()));
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return {};
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path root_;
};

}  // namespace

TEST_CASE("顶层 delete:确认屏 y/n/force 三态,引用按标题解,重名列短 id 拒绝") {
    WorkspaceSessionsFixture fixture("delete_confirm");
    const lubancode::cli::Theme theme;
    const std::string id = fixture.MakeSession("甲的场", "D:/房");
    REQUIRE_FALSE(id.empty());
    const auto file = fixture.SessionDirOf(id);
    REQUIRE_FALSE(file.empty());

    // n:取消,盘上不动。
    CHECK(HandleSessionManagementCommand(fixture.root(), /*kind=delete=*/2, id, false, theme,
                                         [] { return std::string("n"); }) == 1);
    CHECK(std::filesystem::exists(file));

    // y:删掉(tombstone 留痕,目录消失)。
    CHECK(HandleSessionManagementCommand(fixture.root(), 2, id, false, theme,
                                         [] { return std::string("y"); }) == 0);
    CHECK_FALSE(std::filesystem::exists(file));

    // --force:跳过确认直接删(脚本路);引用按标题解。
    const std::string id_b = fixture.MakeSession("唯一标题", "D:/房");
    CHECK(HandleSessionManagementCommand(fixture.root(), 2, "唯一标题", true, theme, nullptr) == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.SessionDirOf(id_b)));

    // 重名:两场同名,列短 id 拒绝(绝不猜一场)。
    const std::string id_c = fixture.MakeSession("同名", "D:/房");
    const std::string id_d = fixture.MakeSession("同名", "D:/房");
    CHECK(HandleSessionManagementCommand(fixture.root(), 2, "同名", true, theme, nullptr) == 1);
    CHECK(std::filesystem::exists(fixture.SessionDirOf(id_c)));
    CHECK(std::filesystem::exists(fixture.SessionDirOf(id_d)));

    // 认不出 / 缺参:如实退 1。
    CHECK(HandleSessionManagementCommand(fixture.root(), 2, "没这一场", true, theme, nullptr) == 1);
    CHECK(HandleSessionManagementCommand(fixture.root(), 2, "", true, theme, nullptr) == 1);
}

TEST_CASE("顶层 archive/unarchive 往返;归档后默认列表不见、archived 列表见") {
    WorkspaceSessionsFixture fixture("archive_cli");
    const lubancode::cli::Theme theme;
    const std::string id = fixture.MakeSession("甲的场", "D:/房");
    const auto dir = fixture.SessionDirOf(id);
    REQUIRE_FALSE(dir.empty());

    // archive(标题命中):目录不搬,session.json 转 archived + lifecycle 记账。
    CHECK(HandleSessionManagementCommand(fixture.root(), /*kind=archive=*/0, "甲的场", false, theme,
                                         nullptr) == 0);
    {
        const auto manifest = lubancode::trajectory::ReadSessionJson(dir);
        REQUIRE(manifest.has_value());
        CHECK(manifest->status == "archived");
    }

    // 默认列表(active)不见;archived 只读入口见。
    lubancode::trajectory::SessionIndexQuery active;
    const std::string key = lubancode::workspace::MakeFallbackIdentity(
                                 fixture.root().parent_path() / "repo")
                                 .workspace_key;
    active.current_workspace_key = key;
    CHECK(lubancode::trajectory::QueryWorkspaceSessions(fixture.root(), active).entries.empty());
    lubancode::trajectory::SessionIndexQuery archived;
    archived.current_workspace_key = key;
    archived.archived_only = true;
    CHECK(lubancode::trajectory::QueryWorkspaceSessions(fixture.root(), archived).total == 1);

    // unarchive(完整 id)搬回 closed,默认列表又见。
    CHECK(HandleSessionManagementCommand(fixture.root(), /*kind=unarchive=*/1, id, false, theme,
                                         nullptr) == 0);
    {
        const auto manifest = lubancode::trajectory::ReadSessionJson(dir);
        REQUIRE(manifest.has_value());
        CHECK(manifest->status == "closed");
    }
    CHECK(lubancode::trajectory::QueryWorkspaceSessions(fixture.root(), active).total == 1);
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
    auto current_think_history = std::make_shared<lubancode::api::ReasoningHistoryMode>();
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
                                session_wire, current_model, current_think, current_think_history,
                                tracker, current_instructions, catalog, prompt_options, rebuild_loop,
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
                                      current_model, current_think, current_think_history, tracker,
                                      current_instructions, catalog, prompt_options, rebuild_loop,
                                      false, theme, std::nullopt, source));
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

// ---------------------------------------------------------------------------
// TryRunCompact 的会话级钉子(P1-1/P1-2):滞回带、反涨拒收、压缩后下一
// 请求/工具执行/session flush 回归。夹具全是合成数据——多轮、多工具、
// 带旧压缩摘要的形状照真机会话档捏,不碰用户真实会话。
// ---------------------------------------------------------------------------

namespace {

// 按脚本吐流的假后端:记下收到的请求(滞回断言"没再发请求"要数它)。
// 双账压缩路(Compact 四分区单·阶段 2-4)配 map_script/reduce_script 两份
// 脚本:system 带"两份总账"的是 final reduce,带"局部小结"的是 map。两份
// 都非空才走分流;只配 script 时维持老行为(每个请求都吐同一份)。

// 文本脚本(先定义,ScriptBackend::send_stream 分流要用)。
std::vector<lubancode::api::StreamEvent> TextScript(const std::string& text) {
    return {
        lubancode::api::MessageStart{"msg", "model"},
        lubancode::api::TextDelta{text},
        lubancode::api::ContentBlockDone{0},
        lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}},
    };
}

class ScriptBackend : public lubancode::api::Backend {
public:
    std::vector<lubancode::api::StreamEvent> script;
    std::string map_script;     // map 请求吐它(严格 JSON TurnGroupSummary)
    std::string reduce_script;  // reduce 请求吐它(严格双账 JSON)
    std::vector<lubancode::api::Request> captured;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        captured.push_back(request);
        if (!reduce_script.empty()) {
            const std::string& chosen =
                request.system.find("两份总账") != std::string::npos ? reduce_script : map_script;
            for (const auto& event : TextScript(chosen)) {
                on_event(event);
            }
            return {};
        }
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

// 双账路的合法脚本:map = 严格 JSON 局部小结;reduce = 严格双账(goal 可
// 塞长文,反涨夹具靠它把存档撑大)。
std::string TurnGroupMapJsonScript() {
    return "{\"user_requirement_changes\": [\"先规划再读文件落码\"], \"confirmed_facts\": [], "
           "\"tool_results\": [], \"files\": [\"src/app.cpp\"], \"changes_made\": [], "
           "\"failed_attempts\": [], \"open_items\": [], \"next_step_candidates\": []}";
}

std::string DualLedgerJsonScript(const std::string& goal_pad = std::string()) {
    return "{\"user_contract\": {\"goal\": {\"text\": \"建图书系统" + goal_pad +
           "\", \"source_turns\": [\"t1\"]}, \"active_constraints\": [], "
           "\"acceptance_criteria\": [], \"additions\": [], \"superseded_requirements\": [], "
           "\"open_questions\": []}, \"work_state\": {\"confirmed_facts\": [], \"tool_results\": [], "
           "\"files\": [], \"changes_made\": [], \"failed_attempts\": [], \"open_items\": [], "
           "\"next_action\": \"落码\"}}";
}

std::vector<lubancode::api::StreamEvent> ToolUseScript(const std::string& tool_use_id) {
    // 一条带 tool_use 的 assistant 消息:验证压缩后的工具执行链。
    return {
        lubancode::api::MessageStart{"msg", "model"},
        lubancode::api::ToolUseStart{0, tool_use_id, "no_such_tool"},
        lubancode::api::ToolUseInputDelta{0, "{}"},
        lubancode::api::ContentBlockDone{0},
        lubancode::api::MessageDone{"tool_use", lubancode::api::Usage{}},
    };
}

// 合成会话:两轮 + 末轮多组工具来回,总量 ~16k token(ASCII 4 字符/token)。
// 形状照真机会话档捏——工具结果占大头,正是 L1 结构压缩的主战场。
std::vector<lubancode::api::Message> SyntheticMultiToolHistory() {
    std::vector<lubancode::api::Message> history;
    auto user_text = [](const std::string& text) {
        lubancode::api::Message m;
        m.role = lubancode::api::Role::User;
        m.content.push_back(lubancode::api::TextBlock{text});
        return m;
    };
    auto assistant_text = [](const std::string& text) {
        lubancode::api::Message m;
        m.role = lubancode::api::Role::Assistant;
        m.content.push_back(lubancode::api::TextBlock{text});
        return m;
    };
    history.push_back(user_text("第一轮:先做规划"));
    history.push_back(assistant_text(std::string(2000, 'a')));
    history.push_back(user_text("第二轮:读文件并落码"));
    const std::string big_result(16000, 'b');
    for (int i = 0; i < 4; ++i) {
        const std::string id = "u" + std::to_string(i);
        lubancode::api::Message use;
        use.role = lubancode::api::Role::Assistant;
        lubancode::api::ToolUseBlock block;
        block.id = id;
        block.name = "read_file";
        block.input = nlohmann::json{{"path", "src/big_" + std::to_string(i) + ".cpp"}};
        use.content.push_back(block);
        history.push_back(use);
        lubancode::api::Message result;
        result.role = lubancode::api::Role::User;
        result.content.push_back(lubancode::api::ToolResultBlock{id, big_result, false});
        history.push_back(result);
    }
    return history;
}

}  // namespace

TEST_CASE("TryRunCompact: 压缩收窄落档;同一视图无进展的第二次触发被滞回拦下") {
    const auto dir = TempDir("compact_hysteresis");
    lubancode::tools::ToolRegistry registry;
    ScriptBackend compact_backend;
    // 双账压缩(阶段 2-4):map 吐严格 JSON 小结,reduce 吐严格双账。
    compact_backend.map_script = TurnGroupMapJsonScript();
    compact_backend.reduce_script = DualLedgerJsonScript();
    lubancode::agent::Agent loop(compact_backend, registry,
                                 lubancode::agent::AgentProfile{.request{.model = "test-model"},
                                                                .system_prompt = "sys"});
    loop.ReplaceHistory(SyntheticMultiToolHistory());

    CompactSessionInputs in;
    in.agent = &loop;
    const lubancode::cli::Theme theme;
    in.theme = &theme;
    int compact_epoch = 0;
    in.session_compact_epoch = &compact_epoch;
    std::string last_compact_line;
    in.last_compact_line = &last_compact_line;
    CompactHysteresis hysteresis;
    in.hysteresis = &hysteresis;
    in.build_compact_options = [] { return lubancode::agent::CompactOptions{}; };
    lubancode::agent::ModelRoute route;
    route.model = "test-model";
    in.route_compact = [&compact_backend, &route]() {
        lubancode::app::ModelRouterService::Routed routed;
        routed.route = route;
        routed.backend = &compact_backend;
        return routed;
    };
    in.route_repair = in.route_compact;
    in.normal_backend = &compact_backend;
    const std::string current_model = "test-model";
    in.current_model = &current_model;
    in.record_usage = [](const lubancode::agent::ModelRole, const lubancode::agent::ModelRoute&,
                         const lubancode::agent::BackgroundCallAccounting&) {};
    in.record_fallback = [](lubancode::agent::TaskKind, lubancode::agent::ModelRole,
                            lubancode::agent::ModelRole, const std::string&) {};

    const std::size_t before = loop.History().size();
    // 第一次:成功换账,历史收窄,滞回账挂上。
    CHECK(TryRunCompact(/*midturn=*/true, in));
    CHECK(loop.History().size() < before);
    CHECK(hysteresis.armed);
    CHECK_FALSE(last_compact_line.empty());
    const std::size_t requests_after_first = compact_backend.captured.size();

    // 第二次,同一副历史,零新增:滞回拦下,连摘要请求都不发。
    CHECK_FALSE(TryRunCompact(/*midturn=*/true, in));
    CHECK(compact_backend.captured.size() == requests_after_first);

    // 攒足新内容(越过滞回带)再触发:放行。
    lubancode::api::Message fresh;
    fresh.role = lubancode::api::Role::User;
    fresh.content.push_back(lubancode::api::TextBlock{std::string(24000, 'c')});
    loop.ReplaceHistory([&loop]() {
        std::vector<lubancode::api::Message> extended = loop.History();
        lubancode::api::Message fresh;
        fresh.role = lubancode::api::Role::User;
        fresh.content.push_back(lubancode::api::TextBlock{std::string(24000, 'c')});
        extended.push_back(fresh);
        return extended;
    }());
    CHECK(TryRunCompact(/*midturn=*/true, in));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("TryRunCompact: 压缩结果不降反升时拒收换账,历史一字未动") {
    const auto dir = TempDir("compact_grew");
    lubancode::tools::ToolRegistry registry;
    ScriptBackend compact_backend;
    // 双账存档本身 ~1100 token(goal 塞 4000 字):两轮共 ~1500 token 的小史,
    // 压完 [双账+末轮热区] 反而更长 → 反涨拒收。
    compact_backend.map_script = TurnGroupMapJsonScript();
    compact_backend.reduce_script = DualLedgerJsonScript(std::string(4000, 's'));
    lubancode::agent::Agent loop(compact_backend, registry,
                                 lubancode::agent::AgentProfile{.request{.model = "test-model"},
                                                                .system_prompt = "sys"});
    // 两轮小史(单轮会被"没有冷区"更早拒掉,这里要测的是反涨闸本身)。
    std::vector<lubancode::api::Message> small;
    for (int turn = 0; turn < 2; ++turn) {
        lubancode::api::Message u;
        u.role = lubancode::api::Role::User;
        u.content.push_back(lubancode::api::TextBlock{std::string(1500, 'u')});
        small.push_back(u);
        lubancode::api::Message a;
        a.role = lubancode::api::Role::Assistant;
        a.content.push_back(lubancode::api::TextBlock{std::string(1500, 'v')});
        small.push_back(a);
    }
    loop.ReplaceHistory(small);
    const std::size_t size_before = loop.History().size();

    CompactSessionInputs in;
    in.agent = &loop;
    const lubancode::cli::Theme theme;
    in.theme = &theme;
    int compact_epoch = 0;
    in.session_compact_epoch = &compact_epoch;
    std::string last_compact_line;
    in.last_compact_line = &last_compact_line;
    in.build_compact_options = [] { return lubancode::agent::CompactOptions{}; };
    lubancode::agent::ModelRoute route;
    route.model = "test-model";
    in.route_compact = [&compact_backend, &route]() {
        lubancode::app::ModelRouterService::Routed routed;
        routed.route = route;
        routed.backend = &compact_backend;
        return routed;
    };
    in.route_repair = in.route_compact;
    in.normal_backend = &compact_backend;
    const std::string current_model = "test-model";
    in.current_model = &current_model;
    in.record_usage = [](const lubancode::agent::ModelRole, const lubancode::agent::ModelRoute&,
                         const lubancode::agent::BackgroundCallAccounting&) {};
    in.record_fallback = [](lubancode::agent::TaskKind, lubancode::agent::ModelRole,
                            lubancode::agent::ModelRole, const std::string&) {};
    CompactHysteresis hysteresis;
    in.hysteresis = &hysteresis;

    // 热区默认 12k 装得下整条小史:archive 并进去后比原史还长 → 拒收。
    CHECK_FALSE(TryRunCompact(/*midturn=*/true, in));
    CHECK(loop.History().size() == size_before);  // 历史未动
    CHECK(hysteresis.armed);                      // 拒收也算收口:同轮不再自动重试

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("TryRunCompact 压缩成功后:下一次模型请求、工具执行、session flush 都活着") {
    const auto dir = TempDir("compact_aftermath");
    lubancode::tools::ToolRegistry registry;
    ScriptBackend compact_backend;
    compact_backend.map_script = TurnGroupMapJsonScript();
    compact_backend.reduce_script = DualLedgerJsonScript();
    lubancode::agent::Agent loop(compact_backend, registry,
                                 lubancode::agent::AgentProfile{.request{.model = "test-model"},
                                                                .runtime{.max_steps_per_turn = 2},
                                                                .system_prompt = "sys"});
    loop.ReplaceHistory(SyntheticMultiToolHistory());

    CompactSessionInputs in;
    in.agent = &loop;
    const lubancode::cli::Theme theme;
    in.theme = &theme;
    int compact_epoch = 0;
    in.session_compact_epoch = &compact_epoch;
    std::string last_compact_line;
    in.last_compact_line = &last_compact_line;
    in.build_compact_options = [] { return lubancode::agent::CompactOptions{}; };
    lubancode::agent::ModelRoute route;
    route.model = "test-model";
    in.route_compact = [&compact_backend, &route]() {
        lubancode::app::ModelRouterService::Routed routed;
        routed.route = route;
        routed.backend = &compact_backend;
        return routed;
    };
    in.route_repair = in.route_compact;
    in.normal_backend = &compact_backend;
    const std::string current_model = "test-model";
    in.current_model = &current_model;
    in.record_usage = [](const lubancode::agent::ModelRole, const lubancode::agent::ModelRoute&,
                         const lubancode::agent::BackgroundCallAccounting&) {};
    in.record_fallback = [](lubancode::agent::TaskKind, lubancode::agent::ModelRole,
                            lubancode::agent::ModelRole, const std::string&) {};
    CompactHysteresis hysteresis;
    in.hysteresis = &hysteresis;

    REQUIRE(TryRunCompact(/*midturn=*/true, in));

    // 压缩后的下一次模型请求 + 工具执行:清掉双账分流脚本、换上带 tool_use
    // 的脚本再跑一轮,未知工具也走完整执行链(结果配对入史,请求链路活着)。
    compact_backend.map_script.clear();
    compact_backend.reduce_script.clear();
    compact_backend.script = ToolUseScript("post_compact_use");
    const auto outcome = loop.Run("压缩完继续干活", lubancode::agent::TurnWiring{});
    REQUIRE(outcome.has_value());
    CHECK(compact_backend.captured.size() >= 2);  // 压缩请求 + 至少一轮续请求
    bool paired = false;
    for (const auto& message : loop.History()) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == "post_compact_use") {
                paired = true;
            }
        }
    }
    CHECK(paired);

    // (P0-6:旧存档的 session flush 断言已删;压缩的持久账是 trajectory 的
    // compact.applied。)
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 手工 /compact 的反涨闸(Compact 四分区单·阶段 0):手工路此前只换账不
// 核涨——小史压完并入存档反而更长,照样把旧史顶掉。现在 HandleCompactCommand
// 与 TryRunCompact 共用 PressureEstimateTokens/RejectGrownCompactHistory,
// 新史(压力口径)不比旧史小便拒收,历史一字不动、事件不落盘。
// ---------------------------------------------------------------------------

TEST_CASE("HandleCompactCommand: 手工压缩反涨也拒收,历史一字未动") {
    lubancode::tools::ToolRegistry registry;
    ScriptBackend compact_backend;
    // 双账存档 ~1100 token(goal 塞 4000 字):两轮小史 ~1500 token,压完
    // [双账+末轮热区] 反而更长 → 反涨拒收。
    compact_backend.map_script = TurnGroupMapJsonScript();
    compact_backend.reduce_script = DualLedgerJsonScript(std::string(4000, 's'));
    lubancode::agent::Agent loop(compact_backend, registry,
                                 lubancode::agent::AgentProfile{.request{.model = "test-model"},
                                                                .system_prompt = "sys"});
    std::vector<lubancode::api::Message> small;
    for (int turn = 0; turn < 2; ++turn) {
        lubancode::api::Message u;
        u.role = lubancode::api::Role::User;
        u.content.push_back(lubancode::api::TextBlock{std::string(1500, 'u')});
        small.push_back(u);
        lubancode::api::Message a;
        a.role = lubancode::api::Role::Assistant;
        a.content.push_back(lubancode::api::TextBlock{std::string(1500, 'v')});
        small.push_back(a);
    }
    loop.ReplaceHistory(small);
    const std::size_t size_before = loop.History().size();

    const lubancode::cli::Theme theme;
    lubancode::agent::ModelRoute route;
    route.model = "test-model";
    int compact_epoch = 0;
    const auto result = HandleCompactCommand(/*args=*/"", loop, compact_backend, route, theme,
                                             /*spinner_enabled=*/false, lubancode::agent::CompactOptions{},
                                             compact_epoch);

    // 反涨:拒收——没有事件,epoch 不进,历史原样。
    CHECK_FALSE(result.applied);
    CHECK(compact_epoch == 0);
    REQUIRE(loop.History().size() == size_before);
    CHECK(std::get<lubancode::api::TextBlock>(loop.History()[0].content[0]).text.size() == 1500);
    CHECK(result.after_tokens == 0);  // 拒收路不换账,没有"压缩后"那个数
}

TEST_CASE("HandleCompactCommand: 手工压缩收窄时照常换账,反涨闸不拦正常路") {
    lubancode::tools::ToolRegistry registry;
    ScriptBackend compact_backend;
    compact_backend.map_script = TurnGroupMapJsonScript();
    compact_backend.reduce_script = DualLedgerJsonScript();
    lubancode::agent::Agent loop(compact_backend, registry,
                                 lubancode::agent::AgentProfile{.request{.model = "test-model"},
                                                                .system_prompt = "sys"});
    loop.ReplaceHistory(SyntheticMultiToolHistory());  // ~16k token,压完必收窄
    const std::size_t size_before = loop.History().size();

    const lubancode::cli::Theme theme;
    lubancode::agent::ModelRoute route;
    route.model = "test-model";
    int compact_epoch = 0;
    const auto result = HandleCompactCommand(/*args=*/"", loop, compact_backend, route, theme,
                                             /*spinner_enabled=*/false, lubancode::agent::CompactOptions{},
                                             compact_epoch);

    REQUIRE(result.applied);
    CHECK(result.after_tokens < result.before_tokens);
    CHECK(compact_epoch == 1);
    CHECK(loop.History().size() < size_before);
}
