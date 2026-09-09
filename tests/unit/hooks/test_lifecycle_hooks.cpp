// 四层生命周期单 P2:分层事件与可靠 Post 的纯函数面。
//
//   1) 事件面:六枚新事件(Session/Turn/Step 三层)的解析、规范名、matcher
//      门槛、输出能力矩阵;PreAction/PostAction 别名指向既有枚举值;
//   2) 配置面:新事件键可配,别名键与旧键同效;
//   3) outbox:幂等键 (event_id, handler_definition_hash) 判重、ack 销账、
//      重开账本 pending 留存/已 ack 压实。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "hooks/events.hpp"
#include "hooks/outbox.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// 1) 事件面。
// ---------------------------------------------------------------------------

TEST_CASE("分层事件:六枚新名解析与规范名") {
    struct Row {
        const char* name;
        hooks::HookEvent event;
    };
    const Row rows[] = {
        {"PreSession", hooks::HookEvent::PreSession},   {"PostSession", hooks::HookEvent::PostSession},
        {"PreTurn", hooks::HookEvent::PreTurn},         {"PostTurn", hooks::HookEvent::PostTurn},
        {"PreStep", hooks::HookEvent::PreStep},         {"PostStep", hooks::HookEvent::PostStep},
    };
    for (const auto& row : rows) {
        hooks::HookEvent parsed{};
        REQUIRE(hooks::ParseHookEvent(row.name, parsed));
        CHECK(parsed == row.event);
        CHECK(hooks::ToString(row.event) == row.name);
        // 分层事件没有 matcher 字段:具体 matcher 配置在解析阶段就该拦。
        CHECK_FALSE(hooks::EventHasMatcherField(row.event));
    }
}

TEST_CASE("Action 别名:PreAction/PostAction 与 PostActionHook 两写法都指向旧枚举") {
    hooks::HookEvent parsed{};
    REQUIRE(hooks::ParseHookEvent("PreAction", parsed));
    CHECK(parsed == hooks::HookEvent::PreToolUse);
    REQUIRE(hooks::ParseHookEvent("PreActionHook", parsed));
    CHECK(parsed == hooks::HookEvent::PreToolUse);
    REQUIRE(hooks::ParseHookEvent("PostAction", parsed));
    CHECK(parsed == hooks::HookEvent::PostToolUse);
    REQUIRE(hooks::ParseHookEvent("PostActionHook", parsed));
    CHECK(parsed == hooks::HookEvent::PostToolUse);
    // wire 事件名保持旧名稳定(既有脚本零迁移)。
    CHECK(hooks::ToString(hooks::HookEvent::PreToolUse) == "PreToolUse");
    CHECK(hooks::ToString(hooks::HookEvent::PostToolUse) == "PostToolUse");
    // 别名仍匹配工具名(与旧事件同一枚值,能力不变)。
    CHECK(hooks::EventMatchesOnToolName(hooks::HookEvent::PreToolUse));
}

TEST_CASE("能力矩阵:Pre 系可拦可注入,Post 系只观察(§四)") {
    // PreSession/PreTurn/PreStep:additional_context + can_block。
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreSession).can_block);
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreSession).additional_context);
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreTurn).can_block);
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreTurn).additional_context);
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreStep).can_block);
    CHECK(hooks::OutputCapabilities(hooks::HookEvent::PreStep).additional_context);
    // 都没有权限表态/改参权。
    CHECK_FALSE(hooks::OutputCapabilities(hooks::HookEvent::PreTurn).permission_decision);
    CHECK_FALSE(hooks::OutputCapabilities(hooks::HookEvent::PreTurn).updated_input);
    CHECK_FALSE(hooks::OutputCapabilities(hooks::HookEvent::PreStep).permission_decision);
    // PostSession/PostTurn/PostStep:纯观察。
    const hooks::HookEvent posts[] = {hooks::HookEvent::PostSession, hooks::HookEvent::PostTurn,
                                      hooks::HookEvent::PostStep};
    for (const hooks::HookEvent event : posts) {
        const auto caps = hooks::OutputCapabilities(event);
        CHECK_FALSE(caps.permission_decision);
        CHECK_FALSE(caps.updated_input);
        CHECK_FALSE(caps.additional_context);
        CHECK_FALSE(caps.can_block);
    }
}

TEST_CASE("outbox 记账范围:Post 型入账,决策型不入") {
    CHECK(hooks::IsPostObservationEvent(hooks::HookEvent::PostToolUse));
    CHECK(hooks::IsPostObservationEvent(hooks::HookEvent::PostSession));
    CHECK(hooks::IsPostObservationEvent(hooks::HookEvent::PostTurn));
    CHECK(hooks::IsPostObservationEvent(hooks::HookEvent::PostStep));
    CHECK(hooks::IsPostObservationEvent(hooks::HookEvent::SessionEnd));
    CHECK_FALSE(hooks::IsPostObservationEvent(hooks::HookEvent::PreToolUse));
    CHECK_FALSE(hooks::IsPostObservationEvent(hooks::HookEvent::PreStep));
    CHECK_FALSE(hooks::IsPostObservationEvent(hooks::HookEvent::Stop));
    CHECK_FALSE(hooks::IsPostObservationEvent(hooks::HookEvent::PreSession));
}

// ---------------------------------------------------------------------------
// 2) 配置面。
// ---------------------------------------------------------------------------

TEST_CASE("ParseHooksConfig:分层事件键可配,别名键与旧键同效") {
    const auto json = nlohmann::json::parse(R"({
        "schema_version": 2,
        "PreStep": [{"hooks": [{"command": "audit.sh"}]}],
        "PostTurn": [{"hooks": [{"command": "report.sh"}]}],
        "PreAction": [{"matcher": "run_command", "hooks": [{"command": "guard.sh"}]}],
        "PostAction": [{"hooks": [{"command": "log.sh"}]}]
    })");
    const auto result = config::ParseHooksConfig(json, "g.json");
    REQUIRE(result.has_value());
    REQUIRE(result->events.count(hooks::HookEvent::PreStep) == 1);
    REQUIRE(result->events.count(hooks::HookEvent::PostTurn) == 1);
    // 别名:配的键是 PreAction/PostAction,落的枚举与旧键同一枚。
    REQUIRE(result->events.count(hooks::HookEvent::PreToolUse) == 1);
    REQUIRE(result->events.count(hooks::HookEvent::PostToolUse) == 1);
}

// ---------------------------------------------------------------------------
// 3) outbox。
// ---------------------------------------------------------------------------

namespace {

// 每册一份独立临时账本路径(doctest 按册跑,册内用唯一文件名防串)。
std::filesystem::path TempOutboxPath(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path();
    return dir / ("lubancode-hooks-outbox-test-" + std::string(tag) + ".jsonl");
}

}  // namespace

TEST_CASE("outbox:幂等键判重——同 (event_id, handler_hash) 重放只记一次") {
    const auto path = TempOutboxPath("idempotent");
    std::filesystem::remove(path);
    auto outbox = hooks::HookOutbox::Open(path);
    REQUIRE(outbox != nullptr);

    const std::string event_id = "hookrun_1";
    const std::string hash_a = "aaaa";
    const std::string hash_b = "bbbb";
    const std::uint64_t first = outbox->RecordPending(event_id, hash_a, "PostToolUse");
    CHECK(first != 0);
    CHECK(outbox->pending_count() == 1);
    // 同键重放:回 0,账不增。
    CHECK(outbox->RecordPending(event_id, hash_a, "PostToolUse") == 0);
    CHECK(outbox->pending_count() == 1);
    CHECK(outbox->Contains(event_id, hash_a));
    // 同事件另一 handler:另一键,各记各的。
    const std::uint64_t second = outbox->RecordPending(event_id, hash_b, "PostToolUse");
    CHECK(second != 0);
    CHECK(second != first);
    CHECK(outbox->pending_count() == 2);
    // ack 后仍判得到(账在,只是销了待办),再重放仍不增。
    outbox->Ack(first);
    CHECK(outbox->pending_count() == 1);
    CHECK(outbox->Contains(event_id, hash_a));
    CHECK(outbox->RecordPending(event_id, hash_a, "PostToolUse") == 0);
    CHECK(outbox->total_count() == 2);
    // 认不得的号:安静忽略。
    outbox->Ack(9999);
    CHECK(outbox->pending_count() == 1);
}

TEST_CASE("outbox:重开账本——pending 留存,已 ack 压实出账") {
    const auto path = TempOutboxPath("reopen");
    std::filesystem::remove(path);
    {
        auto outbox = hooks::HookOutbox::Open(path);
        REQUIRE(outbox != nullptr);
        const std::uint64_t kept = outbox->RecordPending("evt-keep", "hash-1", "PostTurn");
        const std::uint64_t done = outbox->RecordPending("evt-done", "hash-2", "PostStep");
        CHECK(kept != 0);
        CHECK(done != 0);
        outbox->Ack(done);
        CHECK(outbox->pending_count() == 1);
    }
    // 模拟下一次进程开张:重开同一账本。
    auto reopened = hooks::HookOutbox::Open(path);
    REQUIRE(reopened != nullptr);
    CHECK(reopened->pending_count() == 1);
    CHECK(reopened->Contains("evt-keep", "hash-1"));
    CHECK(reopened->Contains("evt-done", "hash-2"));  // 账面判重仍认,重放不增
    CHECK(reopened->RecordPending("evt-keep", "hash-1", "PostTurn") == 0);
    CHECK(reopened->RecordPending("evt-done", "hash-2", "PostStep") == 0);
    CHECK(reopened->pending_count() == 1);
    // 新事件接着发号,不与旧号撞。
    const std::uint64_t fresh = reopened->RecordPending("evt-new", "hash-3", "PostSession");
    CHECK(fresh > 1);
}

TEST_CASE("outbox:坏行容错——截断/垃圾行计数丢弃,好行照进账") {
    const auto path = TempOutboxPath("badlines");
    std::filesystem::remove(path);
    {
        std::ofstream seed(path, std::ios::app);
        seed << "{\"kind\":\"pending\",\"id\":1,\"event_id\":\"evt-good\",\"handler_hash\":\"h\",\"event\":\"PostTurn\"}\n";
        seed << "not json at all\n";
        seed << "{\"kind\":\"weird\",\"id\":2}\n";
    }
    auto outbox = hooks::HookOutbox::Open(path);
    REQUIRE(outbox != nullptr);
    CHECK(outbox->Contains("evt-good", "h"));
    CHECK(outbox->pending_count() == 1);
    CHECK(outbox->dropped_lines() == 2);
}
