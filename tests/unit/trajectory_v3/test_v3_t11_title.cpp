// T11-A / V3-GAP-06(Session v3 旧设计清理单):标题域来源分家。
//
// 断点背景(#52 一族落的标题账):v3 场所有标题采用行一律 source=manual
// 且伪造 titleGenerationId("title-manual-N");自动精炼的模型请求在 v3
// 一笔不落(旁路桥只认 memory_extract);v3 源 resume 不折 control.title,
// 恢复场看不到源场已采用标题,回填路反而再编一枚本地标题。
//
// 本册钉:
//   1. schema:session.title.applied 的 titleGenerationId 可选(生成行必带、
//      manual/local 不带不伪造);source 枚举 manual/local/generated/inherited。
//   2. 手动 /title:applied(source=manual,信封无 titleGenerationId)。
//   3. 本地启发式:applied(source=local,无生成身份)。
//   4. 生成流:title.requested/extracted 带真号;采用行 source=generated
//      同号;prompt/assistant 落正式 message(purpose=session_title,归
//      首问主回合,§4.34),usage owner 在 assistant。
//   5. 迟到生成与用户改名竞态:提取事实照记,采用行不落(代数对不上)。
//   6. resume 显示真实已采用标题:control.title 折源场最后一枚 applied;
//      新场落 source=inherited(带 inheritedFrom 指源事件)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "agent/loop.hpp"  // RequestPreparedContext
#include "api/types.hpp"
#include "app/session_title_account.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

namespace fs = std::filesystem;

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

fs::path FreshRoot(const char* tag) {
    const auto dir =
        fs::temp_directory_path() / ("lubancode-v3-t11-title-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.269-test";
    options.v3_system_content = "你是 LubanCode。";
    return options;
}

fs::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::vector<nlohmann::json> ReadLines(const fs::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

std::vector<const nlohmann::json*> RowsOfKind(const std::vector<nlohmann::json>& rows,
                                              const char* kind) {
    std::vector<const nlohmann::json*> out;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == kind) {
            out.push_back(&row);
        }
    }
    return out;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

// ---- schema 合同层(纯构造,不动盘) ----

nlohmann::json EventJson(const char* kind, nlohmann::json payload,
                          std::optional<std::string> generation_id = std::nullopt) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260916-090000-PC0001";
    json["runId"] = "run-000001";
    json["seq"] = 5;
    json["timestamp"] = "2026-09-16T01:00:00.000Z";
    json["eventId"] = "evt-000004";
    json["kind"] = kind;
    if (generation_id.has_value()) {
        json["titleGenerationId"] = *generation_id;
    }
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    json["lineHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    return json;
}

std::optional<lubancode::trajectory::v3::Schema3Error> CheckEvent(
    const char* kind, nlohmann::json payload, std::optional<std::string> generation_id = std::nullopt) {
    nlohmann::json json = EventJson(kind, std::move(payload), generation_id);
    std::string ec, msg;
    auto parsed = lubancode::trajectory::v3::EventLine::FromJsonStrict(json, &ec, &msg);
    if (!parsed.has_value()) {
        return lubancode::trajectory::v3::Schema3Error{ec, msg};
    }
    return lubancode::trajectory::v3::ValidateEventLine(*parsed);
}

bool HasCode(const std::optional<lubancode::trajectory::v3::Schema3Error>& error, const char* code) {
    return error.has_value() && error->code == code;
}

}  // namespace

// ---------------------------------------------------------------------------
// schema 合同
// ---------------------------------------------------------------------------

TEST_CASE("T11-A schema: 标题族 statusless、来源枚举、生成身份按 source 分家") {
    using lubancode::trajectory::v3::EventKindV3;
    // 注册与 statusless。
    CHECK(lubancode::trajectory::v3::EventKindV3FromName("session.title.applied") ==
          EventKindV3::SessionTitleApplied);
    CHECK_FALSE(lubancode::trajectory::v3::RequiredStatusForKind(EventKindV3::SessionTitleApplied)
                    .has_value());
    // manual:不带生成身份,合法;带伪造号的老档(#52 一族)读取兼容,不拒。
    CHECK_FALSE(CheckEvent("session.title.applied",
                           nlohmann::json{{"title", "修光标"}, {"source", "manual"}})
                    .has_value());
    CHECK_FALSE(CheckEvent("session.title.applied",
                           nlohmann::json{{"title", "修光标"}, {"source", "manual"}},
                           std::optional<std::string>("title-manual-1"))
                    .has_value());
    // local:不带生成身份。
    CHECK_FALSE(CheckEvent("session.title.applied",
                           nlohmann::json{{"title", "入口修复"}, {"source", "local"}})
                    .has_value());
    // generated:必带 titleGenerationId——缺了即造假。
    CHECK(HasCode(CheckEvent("session.title.applied",
                             nlohmann::json{{"title", "终端光标修复"}, {"source", "generated"}}),
                  "schema3.missing_field"));
    CHECK_FALSE(CheckEvent("session.title.applied",
                           nlohmann::json{{"title", "终端光标修复"}, {"source", "generated"}},
                           std::optional<std::string>("titlegen-1"))
                    .has_value());
    // inherited:必带 inheritedFrom 指源场事件。
    CHECK(HasCode(CheckEvent("session.title.applied",
                             nlohmann::json{{"title", "续场"}, {"source", "inherited"}}),
                  "schema3.missing_field"));
    CHECK_FALSE(CheckEvent("session.title.applied",
                           nlohmann::json{{"title", "续场"},
                                          {"source", "inherited"},
                                          {"inheritedFrom",
                                           nlohmann::json{{"sessionId", "20260916-01"},
                                                          {"runId", "run-1"},
                                                          {"seq", 4},
                                                          {"id", "evt-1"},
                                                          {"hash", std::string(64, 'a')}}}})
                    .has_value());
    // 来源枚举之外的值拒收。
    CHECK(HasCode(CheckEvent("session.title.applied",
                             nlohmann::json{{"title", "x"}, {"source", "auto"}}),
                  "schema3.bad_type"));
    // 自动生成流两枚必带生成身份。
    CHECK(HasCode(CheckEvent("title.requested", nlohmann::json{{"task", "session_title_refine"}}),
                  "schema3.missing_field"));
    CHECK_FALSE(CheckEvent("title.requested", nlohmann::json{{"task", "session_title_refine"}},
                           std::optional<std::string>("titlegen-1"))
                    .has_value());
    CHECK(HasCode(CheckEvent("title.extracted", nlohmann::json{{"title", ""}},
                             std::optional<std::string>("titlegen-1")),
                  "schema3.bad_type"));
    CHECK_FALSE(CheckEvent("title.extracted", nlohmann::json{{"title", "终端光标修复"}},
                           std::optional<std::string>("titlegen-1"))
                    .has_value());
}

// ---------------------------------------------------------------------------
// 写路:三来源分家 + 生成流消息 + 迟到竞态 + resume 真值
// ---------------------------------------------------------------------------

TEST_CASE("T11-A 写路: manual/local 不伪造生成身份,generated 带真号贯穿") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("sources");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 手动 /title。
    ledger->RecordTitleChanged("手动题名", "");
    // 本地启发式(首问建档/老档补名同路)。
    ledger->RecordLocalTitleApplied("本地题名", "手动题名");
    // 生成流:requested -> extracted -> adopted,同号贯穿。
    ledger->RecordTitleRequested("titlegen-3", "kimi-k2.6", "moonshot");
    ledger->RecordTitleExtracted("titlegen-3", "精炼题名");
    ledger->RecordGeneratedTitleApplied("titlegen-3", "精炼题名", "本地题名");

    const auto rows = ReadLines(stream);
    const auto applied = RowsOfKind(rows, "session.title.applied");
    REQUIRE(applied.size() == 3);
    CHECK(applied[0]->at("payload").value("source", std::string()) == "manual");
    CHECK(applied[0]->contains("titleGenerationId") == false);
    CHECK(applied[1]->at("payload").value("source", std::string()) == "local");
    CHECK(applied[1]->contains("titleGenerationId") == false);
    REQUIRE(applied[2]->contains("titleGenerationId"));
    CHECK(applied[2]->at("titleGenerationId") == "titlegen-3");
    CHECK(applied[2]->at("payload").value("source", std::string()) == "generated");
    CHECK(applied[2]->at("payload").value("oldTitle", std::string()) == "本地题名");

    const auto requested = RowsOfKind(rows, "title.requested");
    REQUIRE(requested.size() == 1);
    CHECK(requested[0]->at("titleGenerationId") == "titlegen-3");
    const auto extracted = RowsOfKind(rows, "title.extracted");
    REQUIRE(extracted.size() == 1);
    CHECK(extracted[0]->at("titleGenerationId") == "titlegen-3");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("T11-A 生成流: prompt/assistant 落正式 message(purpose=session_title,归首问主回合)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("bypass");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 首问主回合:turn-1 收口后空闲边界,精炼请求起飞。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("帮我修终端光标跳动"));
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::MainTurn;
        api::Request request;
        request.model = "kimi-k2.6";
        request.system = "SYSTEM-X";
        request.messages.push_back(UserMessage("帮我修终端光标跳动"));
        const std::string request_id = bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnRequestSent(request_id));
        api::Usage usage;
        usage.input_tokens = 900;
        usage.output_tokens = 24;
        bridge->OnUsageRecorded(request_id, usage, /*reported_by_provider=*/true, "resp-1");
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"先看看配置。"});
        REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "end_turn", "resp-1"));
        bridge->EndTurn(true, false, "");
    }
    // 精炼旁路(purpose=title_refine):T11-A 起工厂放行。
    auto bypass = ledger->NewBypassBridge({"moonshot", "openai-chat-completions", "host"},
                                          accounting::RequestPurpose::TitleRefine);
    REQUIRE(bypass != nullptr);
    agent::RequestPreparedContext refine_ctx;
    refine_ctx.purpose = accounting::RequestPurpose::TitleRefine;
    refine_ctx.timeout_budget_secs = 5;
    api::Request refine;
    refine.model = "kimi-k2.6";
    refine.system = "给下面这条用户请求起一个会话标题。";
    refine.messages.push_back(UserMessage("原请求: 帮我修终端光标跳动"));
    const std::string refine_id = bypass->OnRequestPrepared(refine, refine_ctx);
    REQUIRE_FALSE(refine_id.empty());
    REQUIRE(bypass->OnRequestSent(refine_id));
    api::Usage refine_usage;
    refine_usage.input_tokens = 120;
    refine_usage.output_tokens = 8;
    bypass->OnUsageRecorded(refine_id, refine_usage, true, "resp-title");
    api::Message refine_assistant;
    refine_assistant.role = api::Role::Assistant;
    refine_assistant.content.push_back(api::TextBlock{"修终端光标跳动"});
    REQUIRE(bypass->OnOutputCompleted(refine_id, refine_assistant, "end_turn", "resp-title"));

    const auto rows = ReadLines(stream);
    // system/user/assistant 三枚 purpose=session_title 的消息;user/assistant
    // 的 turnId 归首问主回合 turn-1(§4.34),不另铸内部回合。
    int title_systems = 0;
    int title_users = 0;
    int title_assistants = 0;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) != "message" ||
            row.value("purpose", std::string()) != "session_title") {
            continue;
        }
        const std::string role = row.at("message").value("role", std::string());
        if (role == "system") {
            ++title_systems;
            CHECK(row.at("systemMeta").value("cause", std::string()) == "session_title_prompt");
        } else if (role == "user") {
            ++title_users;
            CHECK(row.value("turnId", std::string()) == "turn-1");
            CHECK_FALSE(row.contains("parentTurnId"));
        } else if (role == "assistant") {
            ++title_assistants;
            CHECK(row.value("turnId", std::string()) == "turn-1");
            // usage 唯一 owner:assistant 自带实报(§五键名 camelCase)。
            REQUIRE(row.contains("usage"));
            CHECK(row.at("usage").value("inputTokens", 0) == 120);
        }
    }
    CHECK(title_systems == 1);
    CHECK(title_users == 1);
    CHECK(title_assistants == 1);
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("T11-A 竞态: 迟到生成记提取事实,不落采用行;采用对代才落") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("race");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    std::string title;
    lubancode::app::SessionTitleAccount account(title, &*ledger);
    // 起飞:代数 0;起飞事实照记。
    account.NoteTitleGenerationStarted("kimi-k2.6", "moonshot");
    // 用户抢先 /title:翻代(迟到的精炼落地即弃)。
    account.BumpGeneration();
    lubancode::app::SessionTitleRefiner::Outcome late;
    late.ok = true;
    late.title = "迟到题名";
    late.model = "kimi-k2.6";
    late.generation = 0;  // 起飞时的代数
    CHECK(account.AdoptRefined(late) ==
          lubancode::app::SessionTitleAccount::AdoptResult::Ignored);
    CHECK(title.empty());  // 迟到结果不占内存标题

    const auto rows = ReadLines(stream);
    const auto extracted = RowsOfKind(rows, "title.extracted");
    REQUIRE(extracted.size() == 1);  // 提取事实照记(模型确实回了)
    CHECK(extracted[0]->at("titleGenerationId") == "titlegen-0");
    CHECK(extracted[0]->at("payload").value("title", std::string()) == "迟到题名");
    CHECK(RowsOfKind(rows, "session.title.applied").empty());  // 未采用不落

    // 对代的生成结果:extracted + applied(generated)同号。
    account.NoteTitleGenerationStarted("kimi-k2.6", "moonshot");  // 代数 1
    lubancode::app::SessionTitleRefiner::Outcome on_time;
    on_time.ok = true;
    on_time.title = "准点题名";
    on_time.generation = account.generation();
    CHECK(account.AdoptRefined(on_time) ==
          lubancode::app::SessionTitleAccount::AdoptResult::Adopted);
    CHECK(title == "准点题名");
    const auto final_rows = ReadLines(stream);
    const auto applied = RowsOfKind(final_rows, "session.title.applied");
    REQUIRE(applied.size() == 1);
    CHECK(applied[0]->at("payload").value("source", std::string()) == "generated");
    CHECK(applied[0]->at("titleGenerationId") == "titlegen-1");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("T11-A resume: 折本场真实已采用标题,续接不洗掉标题事实") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("resume");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string source_id = ledger->session_id();
    const fs::path source_stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("帮我修终端光标跳动"));
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::MainTurn;
        api::Request request;
        request.model = "kimi-k2.6";
        request.system = "SYSTEM-X";
        request.messages.push_back(UserMessage("帮我修终端光标跳动"));
        const std::string request_id = bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnRequestSent(request_id));
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"看了配置。"});
        REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "end_turn", "resp-1"));
        bridge->EndTurn(true, false, "");
    }
    // 源场标题演进:本地 -> 生成(最后一枚是已采用真值)。
    ledger->RecordLocalTitleApplied("本地题名", "");
    ledger->RecordGeneratedTitleApplied("titlegen-0", "精炼题名", "本地题名");

    const auto summary = ledger->ResumeInteractive(source_id);
    REQUIRE(summary.outcome.error_code.empty());
    // 恢复显示真实已采用标题:control.title 折最后一枚 applied。
    REQUIRE(summary.outcome.control.title.has_value());
    CHECK(*summary.outcome.control.title == "精炼题名");
    // 2026-09-19 落点拍板:续接源场(本场就是源场,同 id 续写)。标题
    // 本就在本场账上,不写 inherited 重申——applied 仍只有本地/生成两枚,
    // 最后一枚是已采用真值;续接只 append,旧行不动。
    CHECK(ledger->session_id() == source_id);
    CHECK(V3StreamOf(*ledger) == source_stream);
    const auto rows_after = ReadLines(V3StreamOf(*ledger));
    const auto applied = RowsOfKind(rows_after, "session.title.applied");
    REQUIRE(applied.size() == 2);
    CHECK(applied[0]->at("payload").value("source", std::string()) == "local");
    CHECK(applied[1]->at("payload").value("source", std::string()) == "generated");
    CHECK(applied[1]->at("payload").value("title", std::string()) == "精炼题名");
    // 读面:FindLastTitleApplied 报本场真值(generated,不被续接洗掉)。
    const auto fact = lubancode::trajectory::v3::FindLastTitleApplied(
        *lubancode::trajectory::v3::ReadV3Ledger(V3StreamOf(*ledger)));
    REQUIRE(fact.has_value());
    CHECK(fact->title == "精炼题名");
    CHECK(fact->source == "generated");
    CHECK(lubancode::trajectory::v3::VerifyV3File(V3StreamOf(*ledger)).ok);
}

// ---------------------------------------------------------------------------
// 触发时机提前单(发车即起飞):主 turn 还开着(BeginTurn 后、EndTurn 前)
// 旁路桥照常接账——v3 无轮账互斥,title_refine 归首问主回合号,与主 turn
// 的请求并行落账不撞。这是"首问发出后立即起飞"的账面依据:旧注释"回合
// 里发必撞车"是 v2 状态机(一 stream 一 open turn)的机理,v3 侧
// active_main_turn_id 自 BeginTurn 起就有值,提前起飞无需等收口。
// ---------------------------------------------------------------------------
TEST_CASE("发车即起飞: 主 turn open 时旁路桥照常接账,与主 turn 请求并行不撞") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("kickoff");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 首问主回合开跑:BeginTurn 铸号、输入落账,首模型请求在飞(只落
    // prepared+sent,模拟请求未回——精炼恰在此刻起飞)。
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("帮我修终端光标跳动"));
    std::string main_request_id;
    {
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::MainTurn;
        api::Request request;
        request.model = "kimi-k2.6";
        request.system = "SYSTEM-X";
        request.messages.push_back(UserMessage("帮我修终端光标跳动"));
        main_request_id = bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(main_request_id.empty());
        REQUIRE(bridge->OnRequestSent(main_request_id));
    }

    // 发车即起飞:主 turn 未收口,旁路桥此刻接 title_refine 的账(全链)。
    auto bypass = ledger->NewBypassBridge({"moonshot", "openai-chat-completions", "host"},
                                          accounting::RequestPurpose::TitleRefine);
    REQUIRE(bypass != nullptr);
    std::string refine_id;
    {
        agent::RequestPreparedContext refine_ctx;
        refine_ctx.purpose = accounting::RequestPurpose::TitleRefine;
        refine_ctx.timeout_budget_secs = 5;
        api::Request refine;
        refine.model = "kimi-k2.6";
        refine.system = "给下面这条用户请求起一个会话标题。";
        refine.messages.push_back(UserMessage("原请求: 帮我修终端光标跳动"));
        refine_id = bypass->OnRequestPrepared(refine, refine_ctx);
        REQUIRE_FALSE(refine_id.empty());  // active_main_turn_id 已铸,接得上账
        REQUIRE(bypass->OnRequestSent(refine_id));
        api::Usage refine_usage;
        refine_usage.input_tokens = 120;
        refine_usage.output_tokens = 8;
        bypass->OnUsageRecorded(refine_id, refine_usage, true, "resp-title");
        api::Message refine_assistant;
        refine_assistant.role = api::Role::Assistant;
        refine_assistant.content.push_back(api::TextBlock{"修终端光标跳动"});
        REQUIRE(bypass->OnOutputCompleted(refine_id, refine_assistant, "end_turn", "resp-title"));
    }

    // 主 turn 的请求这才回来,回合收口——两边的账交错落同一本流。
    {
        api::Usage usage;
        usage.input_tokens = 900;
        usage.output_tokens = 24;
        bridge->OnUsageRecorded(main_request_id, usage, true, "resp-1");
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"先看看配置。"});
        REQUIRE(bridge->OnOutputCompleted(main_request_id, assistant, "end_turn", "resp-1"));
        bridge->EndTurn(true, false, "");
    }

    const auto rows = ReadLines(stream);
    // title_refine 的消息照归首问主回合 turn-1,与收口后起飞的形状一字
    // 不差(§4.34 的归回合规则不因起飞时机变)。
    int title_systems = 0;
    int title_users = 0;
    int title_assistants = 0;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) != "message" ||
            row.value("purpose", std::string()) != "session_title") {
            continue;
        }
        const std::string role = row.at("message").value("role", std::string());
        if (role == "system") {
            ++title_systems;
        } else if (role == "user") {
            ++title_users;
            CHECK(row.value("turnId", std::string()) == "turn-1");
        } else if (role == "assistant") {
            ++title_assistants;
            CHECK(row.value("turnId", std::string()) == "turn-1");
        }
    }
    CHECK(title_systems == 1);
    CHECK(title_users == 1);
    CHECK(title_assistants == 1);
    // 主 turn 的账一枚不少(prepared/sent/usage/output 各就各位),旁路
    // 起飞没碰坏主 turn 的落账。
    CHECK_FALSE(RowsOfKind(rows, "model.request.prepared").empty());
    CHECK_FALSE(RowsOfKind(rows, "model.request.sent").empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}
