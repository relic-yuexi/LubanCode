// 确定性 trace/span id 测试(端云协同可观测单 §9.2/§9.3,T0 验收线
// "同一 Journal 重放两次 ids 完全稳定"):
//   - 确定性:同输入同输出;换 projection_key/session/run/event 全变;
//   - 形状:trace 32 位、span 16 位、小写十六进制、非全零;
//   - 不裸 hash:workspace/session 直接拼的 SHA256 不等于派生值;
//   - W3C traceparent 格式。
#include <doctest/doctest.h>

#include <string>

#include "hooks/hash.hpp"
#include "telemetry/identity.hpp"

using namespace lubancode::telemetry;

TEST_CASE("确定性: 同输入同输出,输入变 id 变") {
    const std::string trace_a = DeriveTraceId("key-a", "sess-1", "run-1");
    const std::string trace_b = DeriveTraceId("key-a", "sess-1", "run-1");
    CHECK(trace_a == trace_b);

    CHECK(DeriveTraceId("key-b", "sess-1", "run-1") != trace_a);  // 换钥匙
    CHECK(DeriveTraceId("key-a", "sess-2", "run-1") != trace_a);  // 换 session
    CHECK(DeriveTraceId("key-a", "sess-1", "run-2") != trace_a);  // 换 run

    const std::string span_a = DeriveSpanId("key-a", "run-1:evt-00000005", "gen_ai.request");
    CHECK(span_a == DeriveSpanId("key-a", "run-1:evt-00000005", "gen_ai.request"));
    CHECK(span_a != DeriveSpanId("key-a", "run-1:evt-00000006", "gen_ai.request"));
    CHECK(span_a != DeriveSpanId("key-a", "run-1:evt-00000005", "tool"));
    CHECK(span_a != DeriveSpanId("key-b", "run-1:evt-00000005", "gen_ai.request"));
}

TEST_CASE("形状: 32/16 位小写十六进制非全零") {
    const std::string trace = DeriveTraceId("k", "s", "r");
    const std::string span = DeriveSpanId("k", "run:evt-00000001", "run");
    CHECK(IsValidTraceId(trace));
    CHECK(IsValidSpanId(span));

    CHECK_FALSE(IsValidTraceId(std::string(32, '0')));  // 全零
    CHECK_FALSE(IsValidTraceId("ABCDEF0123456789ABCDEF0123456789"));  // 大写
    CHECK_FALSE(IsValidTraceId("short"));
    CHECK_FALSE(IsValidTraceId("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));  // 非十六进制
    CHECK_FALSE(IsValidSpanId(std::string(16, '0')));
    CHECK_FALSE(IsValidSpanId("0123456789abcde"));  // 15 位
    CHECK_FALSE(IsValidSpanId(""));
}

TEST_CASE("不裸 hash: 派生值不等于对 session/run 的直接 SHA256") {
    const std::string direct =
        lubancode::hooks::Sha256Hex("demo-session|main-0001");
    const std::string derived = DeriveTraceId("key", "demo-session", "main-0001");
    CHECK(derived != direct.substr(0, 32));
    // 不截 event_hash:span id 不暴露 Journal 链(§9.3)。
    const std::string event_hash =
        "0e0378a9242acefb69019a274ee04caed54217501a1b4e65f9d2d0dc3e2942a1";
    const std::string span = DeriveSpanId("key", "main-0001:evt-00000001", "run");
    CHECK(span != event_hash.substr(0, 16));
}

TEST_CASE("W3C traceparent: 00-<32>-<16>-<flags>") {
    const std::string trace = DeriveTraceId("k", "s", "r");
    const std::string span = DeriveSpanId("k", "r:evt-00000001", "run");
    const std::string parent = FormatTraceParent(trace, span, true);
    CHECK(parent.size() == 2 + 1 + 32 + 1 + 16 + 1 + 2);
    CHECK(parent.substr(0, 3) == "00-");
    CHECK(parent.substr(3, 32) == trace);
    CHECK(parent.substr(36, 16) == span);  // 3 + 32 + 1 = 36
    CHECK(parent.substr(53) == "01");      // 36 + 16 + 1 = 53
    CHECK(FormatTraceParent(trace, span, false).substr(53) == "00");
}
