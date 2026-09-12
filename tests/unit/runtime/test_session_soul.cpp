// Soul 会话冻结单 P0(快照件合同):规范化与"内容未变"判定、JSON 序列化
// 的缺字段/坏材料报错(nlohmann contains 纪律)、会话目录 blob 的原子写读
// round-trip 与损坏检测、多会话快照互不串值。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "runtime/session_soul.hpp"

using namespace lubancode;
using lubancode::runtime::SessionSoulSnapshot;

namespace {

// 每案一间临时目录,析构清场。
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto base = std::filesystem::temp_directory_path();
        std::error_code ec;
        for (int i = 0; i < 64; ++i) {
            const auto candidate = base / ("lubancode-soul-test-" + std::to_string(++counter_));
            if (std::filesystem::create_directory(candidate, ec)) {
                path = candidate;
                return;
            }
        }
        path = base / ("lubancode-soul-test-" + std::to_string(counter_));
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    static int counter_;
};
int TempDir::counter_ = 0;

SessionSoulSnapshot SampleSnapshot() {
    SessionSoulSnapshot snapshot;
    snapshot.name = "wenge";
    snapshot.content = "<!-- 注释 -->\n文风:短句,多动词。";
    snapshot.source = "config:wenge";
    snapshot.content_hash = runtime::SessionSoulContentHash(snapshot.content);
    snapshot.revision = 3;
    snapshot.locked = true;
    return snapshot;
}

}  // namespace

TEST_CASE("规范化:剥注释、统一行尾、去首尾空白") {
    // 注释剥除 + 首尾空白去净(与注入前 StripPromptComments 同一把刀)。
    CHECK(runtime::NormalizeSoulContent("  <!--说明-->\n正文\n") == "正文");
    CHECK(runtime::NormalizeSoulContent("<!--a-->x<!--b-->y") == "xy");
    // 行尾统一:\r\n 与 \n 不算两份魂。
    CHECK(runtime::NormalizeSoulContent("a\r\nb") == "a\nb");
    CHECK(runtime::NormalizeSoulContent("a\r\nb") == runtime::NormalizeSoulContent("a\nb"));
}

TEST_CASE("草稿更新:内容规范化后相同不记虚假 revision") {
    SessionSoulSnapshot snapshot;
    snapshot.name = "default";
    snapshot.content = "魂一";
    snapshot.content_hash = runtime::SessionSoulContentHash(snapshot.content);
    snapshot.revision = 0;

    CHECK_FALSE(runtime::UpdateSessionSoulDraft(snapshot, "default", "魂一\n", "config:default"));
    CHECK(snapshot.revision == 0);  // 规范化后相同:不改账
    CHECK(snapshot.content == "魂一");

    CHECK(runtime::UpdateSessionSoulDraft(snapshot, "default", "魂二", "config:default"));
    CHECK(snapshot.revision == 1);
    CHECK(snapshot.content == "魂二");

    // 只换名(内容同)也算变更——名称是快照的一部分。
    CHECK(runtime::UpdateSessionSoulDraft(snapshot, "wenge", "魂二", "config:wenge"));
    CHECK(snapshot.revision == 2);
    CHECK(snapshot.name == "wenge");
}

TEST_CASE("JSON round-trip:字段原样往返") {
    const SessionSoulSnapshot snapshot = SampleSnapshot();
    const auto json = runtime::SessionSoulSnapshotToJson(snapshot);
    const auto back = runtime::SessionSoulSnapshotFromJson(json);
    REQUIRE(back.has_value());
    CHECK(*back == snapshot);
}

TEST_CASE("JSON 解析的失败面:schema/缺字段/类型/正文与 hash 对不上都报错") {
    const auto json = runtime::SessionSoulSnapshotToJson(SampleSnapshot());

    SUBCASE("不是对象") {
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(nlohmann::json::array()).has_value());
    }
    SUBCASE("schema 不认识") {
        auto bad = json;
        bad["schema"] = "soul-snapshot-v2";
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(bad).has_value());
    }
    SUBCASE("缺字段") {
        auto bad = json;
        bad.erase("contentHash");
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(bad).has_value());
        auto bad2 = json;
        bad2.erase("locked");
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(bad2).has_value());
    }
    SUBCASE("类型不对") {
        auto bad = json;
        bad["revision"] = "三";
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(bad).has_value());
    }
    SUBCASE("正文与 hash 对不上(材料损坏)") {
        auto bad = json;
        bad["content"] = "被篡改的正文";
        CHECK_FALSE(runtime::SessionSoulSnapshotFromJson(bad).has_value());
    }
}

TEST_CASE("会话目录 blob:写入后读回原样;不存在给 nullopt;坏 JSON 报错") {
    TempDir dir;
    const auto read_missing = runtime::ReadSessionSoulSnapshot(dir.path);
    REQUIRE(read_missing.has_value());
    CHECK_FALSE(read_missing->has_value());  // 从未锁定过:不是错误

    const SessionSoulSnapshot snapshot = SampleSnapshot();
    REQUIRE(runtime::WriteSessionSoulSnapshot(dir.path, snapshot).has_value());
    const auto read = runtime::ReadSessionSoulSnapshot(dir.path);
    REQUIRE(read.has_value());
    REQUIRE(read->has_value());
    CHECK(**read == snapshot);

    // 再次成功保存覆盖旧快照(以后一次成功保存为准)。
    SessionSoulSnapshot newer = snapshot;
    newer.content = "换过的魂";
    newer.content_hash = runtime::SessionSoulContentHash(newer.content);
    REQUIRE(runtime::WriteSessionSoulSnapshot(dir.path, newer).has_value());
    const auto read_again = runtime::ReadSessionSoulSnapshot(dir.path);
    REQUIRE(read_again.has_value());
    REQUIRE(read_again->has_value());
    CHECK(**read_again == newer);

    // 坏 JSON:报错不半造快照(resume 侧据此拒绝,不静默换魂)。
    {
        std::ofstream out(dir.path / runtime::SessionSoulSnapshotFileName(), std::ios::binary | std::ios::trunc);
        out << "{ 这不是 JSON";
    }
    const auto read_broken = runtime::ReadSessionSoulSnapshot(dir.path);
    CHECK_FALSE(read_broken.has_value());
}

TEST_CASE("多会话不串值:两只快照各自改动互不影响") {
    SessionSoulSnapshot session_a;
    session_a.name = "default";
    session_a.content = "会话 A 的魂";
    session_a.content_hash = runtime::SessionSoulContentHash(session_a.content);

    SessionSoulSnapshot session_b = session_a;

    CHECK(runtime::UpdateSessionSoulDraft(session_a, "default", "会话 A 改过的魂", "config:default"));
    CHECK(session_a.content == "会话 A 改过的魂");
    CHECK(session_b.content == "会话 A 的魂");  // B 不跟着动
    CHECK(session_a.revision == 1);
    CHECK(session_b.revision == 0);

    session_a.locked = true;
    CHECK_FALSE(session_b.locked);
}
