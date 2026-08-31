// TelemetrySpool 测试(端云协同可观测单 §18,实施分期 T1"segmented
// spool、crash recovery、容量/TTL";验收线"截 active segment,Agent 仍能
// 运行;投影可补账或明确 quarantine"):
//   - append/seal:字节帽与条目帽自动封口,meta 带批账与版本;
//   - 崩溃恢复:active.tmp 半行截断 → 完整批补 seal,半批进 quarantine;
//   - orphan 段:payload 在 meta 丢 → 从 payload 行重造 meta(不丢数据);
//   - ACK 原语:全 ACK 段删除,部分 ACK 段保留;
//   - 容量/TTL:超帽按最老先删,仍超 → degraded。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "telemetry/spool.hpp"

using namespace lubancode::telemetry;

namespace {

std::filesystem::path FreshRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-tel-spool-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SpoolBatchRecord MakeRecord(const std::string& id, const std::string& last_event_id,
                            Priority priority = Priority::P1) {
    SpoolBatchRecord record;
    record.batch_id = id;
    record.signal = "traces";
    record.priority = priority;
    record.workspace_key = "ws-000000000000";
    record.session_id = "20260830-031522-TEST01";
    record.stream_id = "main.jsonl";
    record.first_event_id = "main-1:evt-00000001";
    record.last_event_id = last_event_id;
    record.last_event_hash = "hash-" + last_event_id;
    record.final_window = false;
    record.data_class = DataClass::Metadata;
    record.projector_version = std::string(kProjectorVersion);
    record.redaction_version = std::string(kRedactionPolicyVersion);
    record.telemetry_schema_version = kTelemetrySchemaVersion;
    record.projection_generation = 1;
    record.payload = nlohmann::json{{"resourceSpans", nlohmann::json::array()}};
    return record;
}

struct SpoolDirs {
    std::filesystem::path root;
    SpoolDirs(const char* tag) : root(FreshRoot(tag)) {}
    std::filesystem::path spool() const { return root / "spool"; }
    std::filesystem::path quarantine() const { return root / "quarantine"; }
};

}  // namespace

TEST_CASE("append/seal:条目帽自动封口,meta 带批账") {
    SpoolDirs dirs("seal");
    SpoolOptions options;
    options.segment_items_cap = 2;
    TelemetrySpool spool(dirs.spool(), dirs.quarantine(), options);
    const auto recovery = spool.OpenAndRecover(1000);
    CHECK(recovery.error_code.empty());

    CHECK(spool.AppendBatch(MakeRecord("b1", "main-1:evt-00000001"), 1100));
    CHECK(spool.AppendBatch(MakeRecord("b2", "main-1:evt-00000002"), 1200));
    // 条目帽(2)达帽:第二笔已自动 seal。
    REQUIRE(spool.sealed_segments().size() == 1);
    const SealedSegment& segment = spool.sealed_segments().front();
    REQUIRE(segment.batches.size() == 2);
    CHECK(segment.batches[0].batch_id == "b1");
    CHECK(segment.batches[0].sha256.size() == 64);
    CHECK(segment.projector_version == std::string(kProjectorVersion));
    CHECK(segment.data_class == DataClass::Metadata);
    // 段文件与 meta 都在,active.tmp 清空重开。
    std::error_code ec;
    CHECK(std::filesystem::exists(dirs.spool() / "seg-00000001.otlpjson", ec));
    CHECK(std::filesystem::exists(dirs.spool() / "seg-00000001.meta.json", ec));
    // Coverage 报出 durable 端点;HasBatch 查得到。
    const auto coverage = spool.Coverage();
    const auto found = coverage.find("ws-000000000000|20260830-031522-TEST01|main.jsonl");
    REQUIRE(found != coverage.end());
    CHECK(found->second.last_event_id == "main-1:evt-00000002");
    CHECK(spool.HasBatch("b1"));
    CHECK_FALSE(spool.HasBatch("nope"));
    CHECK(spool.seal_generation() == 1);
}

TEST_CASE("崩溃恢复:active.tmp 半行截断,完整批补 seal,半批隔离") {
    SpoolDirs dirs("truncated");
    {
        TelemetrySpool spool(dirs.spool(), dirs.quarantine(), SpoolOptions{});
        spool.OpenAndRecover(1000);
        spool.AppendBatch(MakeRecord("b1", "main-1:evt-00000001"), 1100);
        spool.AppendBatch(MakeRecord("b2", "main-1:evt-00000002"), 1200);
        // 模拟"截 active segment":不调 SealNow,直接在文件尾砍半行。
        const std::filesystem::path active = dirs.spool() / "active.tmp";
        std::string content;
        { std::ifstream file(active, std::ios::binary); std::getline(file, content, '\0'); }
        // 找第二行的中间下刀:保留第一行 + 第二行前半。
        const std::size_t second_line = content.find("b2") - 10;
        content = content.substr(0, second_line + 4);
        { std::ofstream file(active, std::ios::binary | std::ios::trunc); file << content; }
    }
    TelemetrySpool reopened(dirs.spool(), dirs.quarantine(), SpoolOptions{});
    const auto report = reopened.OpenAndRecover(2000);
    CHECK(report.sealed_from_active == 1);    // 完整批(b1)补 seal
    CHECK(report.quarantined_batches == 1);  // 半行(b2)进 quarantine
    REQUIRE(reopened.sealed_segments().size() == 1);
    CHECK(reopened.sealed_segments().front().batches.size() == 1);
    CHECK(reopened.sealed_segments().front().batches[0].batch_id == "b1");
    // durable 端点只到 b1:半批的窗口不算入账。
    const auto coverage = reopened.Coverage();
    REQUIRE(coverage.count("ws-000000000000|20260830-031522-TEST01|main.jsonl") == 1);
    CHECK(coverage.at("ws-000000000000|20260830-031522-TEST01|main.jsonl").last_event_id ==
          "main-1:evt-00000001");
    // 隔离区有取证件。
    std::error_code ec;
    CHECK(std::filesystem::exists(dirs.quarantine(), ec));
}

TEST_CASE("orphan 段:meta 丢了,从 payload 行重造(不丢数据)") {
    SpoolDirs dirs("orphan");
    {
        TelemetrySpool spool(dirs.spool(), dirs.quarantine(), SpoolOptions{});
        spool.OpenAndRecover(1000);
        spool.AppendBatch(MakeRecord("b1", "main-1:evt-00000001"), 1100);
        spool.SealNow(1200);
        REQUIRE(spool.sealed_segments().size() == 1);
    }
    std::error_code ec;
    std::filesystem::remove(dirs.spool() / "seg-00000001.meta.json", ec);
    TelemetrySpool reopened(dirs.spool(), dirs.quarantine(), SpoolOptions{});
    const auto report = reopened.OpenAndRecover(2000);
    CHECK(report.orphan_segments == 1);
    REQUIRE(reopened.sealed_segments().size() == 1);
    CHECK(reopened.sealed_segments().front().batches[0].batch_id == "b1");
    // 重造的 meta 落了盘:下次开张走正账。
    CHECK(std::filesystem::exists(dirs.spool() / "seg-00000001.meta.json", ec));
    CHECK(reopened.HasBatch("b1"));
}

TEST_CASE("ACK 原语:全 ACK 段删,部分 ACK 段留") {
    SpoolDirs dirs("ack");
    TelemetrySpool spool(dirs.spool(), dirs.quarantine(), SpoolOptions{});
    spool.OpenAndRecover(1000);
    // 两批进同一段(不中途封口)。
    spool.AppendBatch(MakeRecord("b1", "main-1:evt-00000001"), 1100);
    spool.AppendBatch(MakeRecord("b2", "main-1:evt-00000002"), 1200);
    spool.SealNow(1300);
    REQUIRE(spool.sealed_segments().size() == 1);

    // 部分 ACK:段里还有未 ACK 的 b2 → 段保留。
    CHECK(spool.AckBatches({"b1"}).empty());
    CHECK(spool.sealed_segments().size() == 1);
    CHECK(spool.HasBatch("b1"));
    CHECK(spool.HasBatch("b2"));
    // 全 ACK → 删除。
    const auto deleted = spool.AckBatches({"b1", "b2"});
    REQUIRE(deleted.size() == 1);
    CHECK(spool.sealed_segments().empty());
    CHECK_FALSE(spool.HasBatch("b2"));
}

TEST_CASE("容量清理:超帽按最老先删并记账,清得动不降级(§18.4)") {
    SpoolDirs dirs("cleanup");
    SpoolOptions options;
    options.total_bytes_cap = 1;  // 封口即超帽:清理路接管
    TelemetrySpool spool(dirs.spool(), dirs.quarantine(), options);
    spool.OpenAndRecover(1000);
    CHECK(spool.AppendBatch(MakeRecord("b1", "main-1:evt-00000001"), 1100));
    spool.SealNow(1200);
    auto stats = spool.Stats(1300);
    // 删优先于降级(§18.4 次序):段被清掉、账记上、清理半账可对 cursor 交代。
    CHECK(stats.cleaned_segments_total == 1);
    CHECK(stats.cleaned_bytes_total > 0);
    CHECK_FALSE(stats.degraded);  // 清得动就不降级
    CHECK_FALSE(spool.HasBatch("b1"));
    // 清理半账:被删段覆盖到的 stream 端点记下(cursor 对账用,§18.5)。
    REQUIRE(spool.CleanedCoverage().count("ws-000000000000|20260830-031522-TEST01|main.jsonl") == 1);
    CHECK(spool.CleanedCoverage()
              .at("ws-000000000000|20260830-031522-TEST01|main.jsonl")
              .last_event_id == "main-1:evt-00000001");
    // 再来一批仍收(删出来的空间),流水线不因帽小而停摆。
    CHECK(spool.AppendBatch(MakeRecord("b2", "main-1:evt-00000002"), 1400));
    spool.SealNow(1500);
    CHECK(spool.Stats(1600).cleaned_segments_total == 2);
}
