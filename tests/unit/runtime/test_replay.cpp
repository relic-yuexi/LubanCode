// 统一回放接口的机制钉。
//
// 规矩钉死(不带各家语义——loop/goal/workflow 的折叠各有自家测试钉):
//   - 次序:不带 seq 的行文件序原样(session 行的形状);带 seq 的域按
//     seq 稳定排序(journal 的形状,半截尾行跳过后仍单调);
//   - 坏行跳过不废整场:编解码给 nullopt、抛异常,折叠口返 false、抛
//     异常,四路都算"这一条不认",整场照跑;
//   - 账面:lines(过眼)/replayed(收进)/skipped(解不动 + 拒收)对得上。

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/replay.hpp"

using namespace lubancode::runtime::replay;

namespace {

// 测试用编解码:{"type":"t_v1","event":...,"payload":{...}} 认,其余不认。
LineCodec TestCodec() {
    return [](const std::string& line) -> std::optional<Envelope> {
        const nlohmann::json j = nlohmann::json::parse(line);
        if (!j.is_object() || j.value("type", std::string()) != "t_v1") {
            return std::nullopt;
        }
        Envelope envelope;
        envelope.family = j.value("type", std::string());
        envelope.event = j.value("event", std::string());
        envelope.payload = j.value("payload", nlohmann::json::object());
        if (const auto seq = j.find("seq"); seq != j.end() && seq->is_number_unsigned()) {
            envelope.seq = seq->get<std::uint64_t>();
        }
        return envelope;
    };
}

std::string Line(const std::string& event, const char* extra = "") {
    return std::string("{\"type\":\"t_v1\",\"event\":\"") + event + "\",\"payload\":{}" + extra + "}";
}

}  // namespace

TEST_CASE("文件序:不带 seq 的行按出现次序喂折叠口") {
    std::vector<std::string> order;
    const auto stats = ReplayLedgerLines(
        {Line("first"), Line("second"), Line("third")}, TestCodec(),
        [&order](const Envelope& envelope) {
            order.push_back(envelope.event);
            return true;
        });
    CHECK(stats.lines == 3);
    CHECK(stats.replayed == 3);
    CHECK(stats.skipped == 0);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "first");
    CHECK(order[1] == "second");
    CHECK(order[2] == "third");
}

TEST_CASE("seq 次序:带 seq 的域按 seq 排序喂,跳过的半截行不碍事") {
    std::vector<std::string> order;
    const auto stats = ReplayLedgerLines(
        {Line("c", ",\"seq\":3"), "{\"type\":\"t_v1\",\"event\":\"broken\"",  // 半截尾行
         Line("a", ",\"seq\":1"), Line("b", ",\"seq\":2")},
        TestCodec(),
        [&order](const Envelope& envelope) {
            order.push_back(envelope.event);
            return true;
        });
    CHECK(stats.lines == 4);
    CHECK(stats.replayed == 3);
    CHECK(stats.skipped == 1);  // 半截行解不动
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "a");
    CHECK(order[1] == "b");
    CHECK(order[2] == "c");
}

TEST_CASE("坏行跳过不废整场:异族行/坏 JSON/折叠口拒收/折叠口抛异常") {
    int accepted = 0;
    const auto stats = ReplayLedgerLines(
        {"not json at all",
         "{\"type\":\"other_v1\",\"event\":\"x\",\"payload\":{}}",  // 异族行
         Line("good"),
         Line("rejected"),  // 折叠口拒收
         Line("throwing")},  // 折叠口抛异常
        TestCodec(),
        [&accepted](const Envelope& envelope) {
            if (envelope.event == "throwing") {
                throw std::runtime_error("域折叠里的坏载荷");
            }
            if (envelope.event == "rejected") {
                return false;
            }
            accepted += 1;
            return true;
        });
    CHECK(stats.lines == 5);
    CHECK(stats.replayed == 1);
    CHECK(stats.skipped == 4);  // 坏 JSON + 异族 + 拒收 + 抛异常
    CHECK(accepted == 1);       // 整场照跑:好行照收
}

TEST_CASE("编解码抛异常按坏行收(调用方不必自带 try/catch)") {
    const auto stats = ReplayLedgerLines(
        {Line("ok"), "not json"}, TestCodec(), [](const Envelope&) { return true; });
    CHECK(stats.replayed == 1);
    CHECK(stats.skipped == 1);
}

TEST_CASE("ReplayEnvelopes:已解析信封走同一条次序/跳过/账面规矩") {
    std::vector<Envelope> envelopes(3);
    envelopes[0].event = "late";
    envelopes[0].seq = 9;
    envelopes[1].event = "early";
    envelopes[1].seq = 2;
    envelopes[2].event = "middle";
    envelopes[2].seq = 5;

    std::vector<std::string> order;
    const auto stats = ReplayEnvelopes(
        std::move(envelopes), [&order](const Envelope& envelope) {
            order.push_back(envelope.event);
            return true;
        });
    CHECK(stats.lines == 3);
    CHECK(stats.replayed == 3);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "early");
    CHECK(order[1] == "middle");
    CHECK(order[2] == "late");
}

TEST_CASE("载荷过境:域字段在信封 payload 里原样进出,信封不裁剪") {
    nlohmann::json seen;
    const auto stats = ReplayLedgerLines(
        {"{\"type\":\"t_v1\",\"event\":\"e\",\"payload\":{\"task_id\":\"loop-1\",\"nested\":{\"a\":[1,2]}}}"},
        TestCodec(), [&seen](const Envelope& envelope) {
            seen = envelope.payload;
            return true;
        });
    REQUIRE(stats.replayed == 1);
    CHECK(seen.at("task_id") == "loop-1");
    CHECK(seen.at("nested").at("a") == nlohmann::json({1, 2}));
}
