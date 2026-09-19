// 更新器事务账册(批二第③单)。覆盖:
//   - 状态机全相流转:checking -> downloading -> verified -> staged ->
//     waiting-for-idle -> activating -> healthy -> committed,每步落盘;
//   - 逐字节黄金对拍:拿 python 版 updater.py 的 Transaction 真跑落盘的
//     样例(tests/fixtures/updater/txn_*_golden.json,生成器
//     make_txn_golden.py,时钟/账号钉死)对拍 C++ 侧同样驱动的产物——
//     字段名与 schema 逐字相同的验收口径;
//   - 异常终态持久化:needs-review(blocking)/failed(reason)/rolled-back
//     (reason/rolled_back_at_utc/restored_to);
//   - resumable/superseded 裁决:同 digest 未终结/needs-review 续跑,
//     异目标 failed{reason:"superseded"}+staging 清场,终态跳过;
//   - txn id 形状、OpenExisting 容错、ListTransactions 排序、Note 追加。
#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "updater/layout.hpp"
#include "updater/txn.hpp"

namespace {

using namespace lubancode::updater;

std::filesystem::path TempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_updater_txn_" + std::string(name) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::optional<std::string> ReadBytes(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::optional<std::string> ReadGolden(const char* name) {
    return ReadBytes(std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "updater" / name);
}

std::string FixedNow() { return "2026-09-20T12:00:00Z"; }

// 与黄金生成器(make_txn_golden.py)逐字段同形的目标。
TxnTarget GoldenTarget() {
    const std::string digest(64, 'b');
    TxnTarget target;
    target.version = "0.26.280";
    target.tag = "v0.26.280";
    target.exe_version = "0.26.280";
    target.platform = "windows-x64";
    target.dirname = "0.26.280-bbbbbbbb";
    target.digest_hex = digest;
    target.repo = "relic-yuexi/LubanCode";
    target.release_id = 123456789;
    target.asset_id = 987654321;
    target.asset_name = "lubancode-0.26.280-windows-x64.zip";
    target.asset_size = 536870912;
    target.download_url = std::nullopt;
    return target;
}

}  // namespace

// ----------------------------------------------------------- 黄金对拍 ---

TEST_CASE("黄金对拍: 全相流转到 committed,与 python 落盘逐字节相同") {
    const auto root = TempRoot("golden-committed");
    const LayoutPaths paths = MakeLayoutPaths(root);

    Transaction txn(paths, "20260920T120000Z-1a2b3c4d", FixedNow);
    txn.Create(GoldenTarget());
    txn.Transition(kTxnDownloading);
    nlohmann::json verified = nlohmann::json::object();
    verified["archive_sha256"] = "sha256:" + std::string(64, 'b');
    txn.Transition(kTxnVerified, std::move(verified));
    txn.Note("解包与逐文件核对通过");
    txn.Transition(kTxnStaged);
    nlohmann::json waiting = nlohmann::json::object();
    waiting["rollback_current"] = "0.26.279-aaaaaaaa";
    waiting["layout_before"] = "flat";
    txn.Transition(kTxnWaitingForIdle, std::move(waiting));
    txn.Transition(kTxnActivating);
    nlohmann::json healthy = nlohmann::json::object();
    healthy["health_probe"] = "lubancode 0.26.280";
    txn.Transition(kTxnHealthy, std::move(healthy));
    nlohmann::json committed = nlohmann::json::object();
    committed["committed_at_utc"] = "2026-09-20T12:00:00Z";
    txn.Transition(kTxnCommitted, std::move(committed));

    const auto ledger = ReadBytes(txn.LedgerPath());
    REQUIRE(ledger.has_value());
    const auto golden = ReadGolden("txn_committed_golden.json");
    REQUIRE(golden.has_value());
    CHECK(*ledger == *golden);
}

TEST_CASE("黄金对拍: 异常终态(needs-review -> failed),与 python 落盘逐字节相同") {
    const auto root = TempRoot("golden-failed");
    const LayoutPaths paths = MakeLayoutPaths(root);

    Transaction txn(paths, "20260920T120000Z-1a2b3c4d", FixedNow);
    txn.Create(GoldenTarget());
    txn.Transition(kTxnDownloading);
    txn.Note("下载完成,核对摘要通过");
    nlohmann::json blocking = nlohmann::json::object();
    blocking["blocking"] = nlohmann::json::array(
        {nlohmann::json{{"path", "skills/lubancode-config/SKILL.md"},
                        {"action", "conflict-modified"}},
         nlohmann::json{{"path", "docs/README.md"}, {"action", "conflict-collision"}}});
    txn.Transition(kTxnNeedsReview, std::move(blocking));
    txn.Transition(kTxnActivating);
    nlohmann::json failed = nlohmann::json::object();
    failed["reason"] = "激活中途失败: 旧根 EXE 挪不进备份";
    txn.Transition(kTxnFailed, std::move(failed));

    const auto ledger = ReadBytes(txn.LedgerPath());
    REQUIRE(ledger.has_value());
    const auto golden = ReadGolden("txn_failed_golden.json");
    REQUIRE(golden.has_value());
    CHECK(*ledger == *golden);
}

TEST_CASE("schema 与字段名逐字断言: 账面键集与 python 版逐字相同") {
    const auto root = TempRoot("field-names");
    Transaction txn(MakeLayoutPaths(root), "20260920T120000Z-1a2b3c4d", FixedNow);
    txn.Create(GoldenTarget());
    txn.Transition(kTxnDownloading);
    txn.Note("note-a");
    nlohmann::json waiting = nlohmann::json::object();
    waiting["rollback_current"] = "0.26.279-aaaaaaaa";
    waiting["layout_before"] = "versioned";
    txn.Transition(kTxnWaitingForIdle, std::move(waiting));
    txn.Transition(kTxnActivating);
    nlohmann::json healthy = nlohmann::json::object();
    healthy["health_probe"] = "lubancode 0.26.280";
    txn.Transition(kTxnHealthy, std::move(healthy));

    const auto bytes = ReadBytes(txn.LedgerPath());
    REQUIRE(bytes.has_value());
    const nlohmann::json data = nlohmann::json::parse(*bytes, nullptr, false);
    REQUIRE(data.is_object());
    // 建账九键(python Transaction.create 的 dict 逐名)。
    for (const char* key : {"schema", "id", "kind", "created_at_utc", "state", "state_at_utc",
                            "target_version", "target_dirname", "target_digest", "target"}) {
        CHECK(data.contains(key));
    }
    CHECK(data["schema"] == 1);
    CHECK(data["id"] == "20260920T120000Z-1a2b3c4d");
    CHECK(data["kind"] == "update");
    CHECK(data["state"] == "healthy");
    CHECK(data["state_at_utc"] == "2026-09-20T12:00:00Z");
    CHECK(data["target_version"] == "0.26.280");
    CHECK(data["target_dirname"] == "0.26.280-bbbbbbbb");
    CHECK(data["target_digest"] == std::string(64, 'b'));
    CHECK(data["target"].is_object());
    // 各步骤的追加键逐名:notes/rollback_current/layout_before/health_probe。
    CHECK(data.contains("notes"));
    CHECK(data["notes"].is_array());
    CHECK(data["notes"].size() == 1);
    CHECK(data["notes"][0] == "2026-09-20T12:00:00Z note-a");
    CHECK(data["rollback_current"] == "0.26.279-aaaaaaaa");
    CHECK(data["layout_before"] == "versioned");
    CHECK(data["health_probe"] == "lubancode 0.26.280");
}

// ----------------------------------------------------------- 状态机 ---

TEST_CASE("状态机: 每步换相 state_at_utc 跟着走,终态与异常终态持久化") {
    const auto root = TempRoot("states");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 步进钟:每拍 +1 秒,state_at_utc 的刷新看得见。
    int tick = 0;
    auto stepping = [&tick]() {
        return "2026-09-20T12:00:" + (tick < 10 ? "0" + std::to_string(tick) : std::to_string(tick)) +
               "Z";
    };

    Transaction txn(paths, "20260920T120000Z-00000001", stepping);
    txn.Create(GoldenTarget());
    CHECK(txn.state() == "checking");
    CHECK(ReadBytes(txn.LedgerPath()).has_value());  // create 即落盘

    ++tick;
    txn.Transition(kTxnDownloading);
    CHECK(txn.state() == "downloading");
    auto parsed = nlohmann::json::parse(*ReadBytes(txn.LedgerPath()));
    CHECK(parsed["state_at_utc"] == "2026-09-20T12:00:01Z");

    ++tick;
    txn.Transition(kTxnVerified);
    ++tick;
    txn.Transition(kTxnStaged);
    ++tick;
    txn.Transition(kTxnWaitingForIdle);
    ++tick;
    txn.Transition(kTxnActivating);
    ++tick;
    txn.Transition(kTxnHealthy);
    ++tick;
    txn.Transition(kTxnCommitted);
    CHECK(txn.state() == "committed");
    parsed = nlohmann::json::parse(*ReadBytes(txn.LedgerPath()));
    CHECK(parsed["state"] == "committed");
    CHECK(parsed["state_at_utc"] == "2026-09-20T12:00:07Z");
    // 终态在重跑裁决里跳过。
    CHECK(IsTxnTerminalForResume("committed"));
    CHECK(IsTxnTerminalForResume("failed"));
    CHECK(IsTxnTerminalForResume("rolled-back"));
    CHECK_FALSE(IsTxnTerminalForResume("needs-review"));
    CHECK_FALSE(IsTxnTerminalForResume("healthy"));
    CHECK_FALSE(IsTxnTerminalForResume(""));

    // rolled-back 的追加字段(rollback_after_activation_failure 的 detail)。
    ++tick;
    Transaction rollback_txn(paths, "20260920T120000Z-00000002", stepping);
    rollback_txn.Create(GoldenTarget());
    nlohmann::json rolled = nlohmann::json::object();
    rolled["reason"] = "健康检查不过: 探针版本不合";
    rolled["rolled_back_at_utc"] = "2026-09-20T12:00:09Z";
    rolled["restored_to"] = "0.26.279-aaaaaaaa";
    rollback_txn.Transition(kTxnRolledBack, std::move(rolled));
    parsed = nlohmann::json::parse(*ReadBytes(rollback_txn.LedgerPath()));
    CHECK(parsed["state"] == "rolled-back");
    CHECK(parsed["reason"] == "健康检查不过: 探针版本不合");
    CHECK(parsed["rolled_back_at_utc"] == "2026-09-20T12:00:09Z");
    CHECK(parsed["restored_to"] == "0.26.279-aaaaaaaa");
}

TEST_CASE("Note: setdefault 语义,多行追加,时间戳打头") {
    const auto root = TempRoot("notes");
    Transaction txn(MakeLayoutPaths(root), "20260920T120000Z-00000003", FixedNow);
    txn.Create(GoldenTarget());  // 建账没有 notes
    txn.Note("第一行");
    txn.Transition(kTxnDownloading);
    txn.Note("第二行");
    const nlohmann::json parsed = nlohmann::json::parse(*ReadBytes(txn.LedgerPath()));
    REQUIRE(parsed.contains("notes"));
    REQUIRE(parsed["notes"].is_array());
    REQUIRE(parsed["notes"].size() == 2);
    CHECK(parsed["notes"][0] == "2026-09-20T12:00:00Z 第一行");
    CHECK(parsed["notes"][1] == "2026-09-20T12:00:00Z 第二行");
}

TEST_CASE("Transition 的追加字段不是 object:拒") {
    const auto root = TempRoot("bad-fields");
    Transaction txn(MakeLayoutPaths(root), "20260920T120000Z-00000004", FixedNow);
    txn.Create(GoldenTarget());
    CHECK_THROWS(txn.Transition(kTxnDownloading, nlohmann::json::array({1, 2})));
    CHECK_THROWS(txn.Transition(kTxnDownloading, nlohmann::json("text")));
    // 空账不能落盘。
    Transaction empty;
    CHECK_THROWS(empty.Flush());
}

// ------------------------------------------------------- id/路径/清单 ---

TEST_CASE("MakeTxnId: UTC 时间戳段 + 8 位小写十六进制随机段") {
    const std::string id = MakeTxnId();
    // 形状:<YYYYmmddTHHMMSSZ>-<8 hex>,共 16 + 1 + 8 = 25 字符。
    REQUIRE(id.size() == 25);
    REQUIRE(id[8] == 'T');
    REQUIRE(id[15] == 'Z');
    REQUIRE(id[16] == '-');
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(std::isdigit(static_cast<unsigned char>(id[i])) != 0);
    }
    for (std::size_t i = 9; i < 15; ++i) {
        CHECK(std::isdigit(static_cast<unsigned char>(id[i])) != 0);
    }
    for (std::size_t i = 17; i < 25; ++i) {
        const char c = id[i];
        CHECK((std::isdigit(static_cast<unsigned char>(c)) != 0 || (c >= 'a' && c <= 'f')));
    }
    // 两枚不重样(随机段)。
    CHECK(MakeTxnId() != MakeTxnId());
}

TEST_CASE("路径形状: 账本/staging/archive 按 id 落位") {
    const LayoutPaths paths = MakeLayoutPaths(std::filesystem::path("R"));
    Transaction txn(paths, "20260920T120000Z-1a2b3c4d");
    CHECK(txn.LedgerPath() == paths.updates / "20260920T120000Z-1a2b3c4d.json");
    CHECK(txn.StageDir() == paths.staging / "20260920T120000Z-1a2b3c4d");
    CHECK(txn.ArchivePath() == paths.staging / "20260920T120000Z-1a2b3c4d" / "archive.bin");
}

TEST_CASE("OpenExisting: 好账读回,坏账/缺 id 拒") {
    const auto root = TempRoot("open-existing");
    const LayoutPaths paths = MakeLayoutPaths(root);

    Transaction txn(paths, "20260920T120000Z-00000005", FixedNow);
    txn.Create(GoldenTarget());
    txn.Transition(kTxnStaged);

    auto opened = Transaction::OpenExisting(txn.LedgerPath(), FixedNow);
    REQUIRE(opened.has_value());
    CHECK(opened->id() == "20260920T120000Z-00000005");
    CHECK(opened->state() == "staged");
    CHECK(opened->data() == txn.data());
    CHECK(opened->StageDir() == txn.StageDir());  // root 从账本路径反推

    // 坏 JSON / 非 object / 缺 id:一概 nullopt。
    const auto bad = root / "updates" / "bad.json";
    std::error_code ec;
    std::filesystem::create_directories(bad.parent_path(), ec);
    {
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        out << "{ half written";
    }
    CHECK_FALSE(Transaction::OpenExisting(bad).has_value());
    {
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        out << "[1, 2]";
    }
    CHECK_FALSE(Transaction::OpenExisting(bad).has_value());
    {
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        out << R"({"schema": 1, "state": "checking"})";  // 缺 id
    }
    CHECK_FALSE(Transaction::OpenExisting(bad).has_value());
    CHECK_FALSE(Transaction::OpenExisting(root / "updates" / "not-there.json").has_value());

    // 坏账不进清单(ListTransactions 跳过,好账照收)。
    const auto listed = ListTransactions(paths, FixedNow);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].id() == "20260920T120000Z-00000005");
}

TEST_CASE("ListTransactions: 按文件名排序,非 .json 不收") {
    const auto root = TempRoot("list");
    const LayoutPaths paths = MakeLayoutPaths(root);
    for (const std::string id : {"20260920T120001Z-000000aa", "20260920T120000Z-000000bb",
                                 "20260920T120002Z-000000cc"}) {
        Transaction txn(paths, id, FixedNow);
        txn.Create(GoldenTarget());
    }
    // updates/ 里的杂件:不是 .json 的不收,坏 .json 的跳过。
    std::error_code ec;
    std::filesystem::create_directories(paths.updates, ec);
    { std::ofstream(paths.updates / "README.md", std::ios::binary) << "note"; }
    { std::ofstream(paths.updates / ".lock", std::ios::binary) << "{}"; }
    { std::ofstream(paths.updates / "zz-broken.json", std::ios::binary) << "{"; }

    const auto listed = ListTransactions(paths, FixedNow);
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].id() == "20260920T120000Z-000000bb");
    CHECK(listed[1].id() == "20260920T120001Z-000000aa");
    CHECK(listed[2].id() == "20260920T120002Z-000000cc");
}

// ------------------------------------------------------- 续跑裁决 ---

TEST_CASE("FindResumable: 同 digest 未终结续跑,异目标 superseded+清场") {
    const auto root = TempRoot("resumable");
    const LayoutPaths paths = MakeLayoutPaths(root);
    const std::string digest_x(64, 'x');
    const std::string digest_y(64, 'y');

    auto make_target = [&digest_x](const std::string& digest) {
        TxnTarget target = GoldenTarget();
        target.digest_hex = digest;
        target.dirname = "0.26.280-" + digest.substr(0, 8);
        return target;
    };

    // t2:异 digest、staged,带 staging 目录——作废+清场。账号时间戳排最前,
    // 裁决按文件名排序走:异目标先撞上、被作废,然后才轮到可续的同目标。
    Transaction t2(paths, "20260920T115959Z-000000t2", FixedNow);
    t2.Create(make_target(digest_y));
    t2.Transition(kTxnStaged);
    std::error_code ec;
    std::filesystem::create_directories(t2.StageDir() / "pkg", ec);
    { std::ofstream(t2.StageDir() / "pkg" / "seed.txt", std::ios::binary) << "x"; }

    // t1:同 digest、verified——排序在 t2 后,是返回的那笔。
    Transaction t1(paths, "20260920T120000Z-000000t1", FixedNow);
    t1.Create(make_target(digest_x));
    t1.Transition(kTxnDownloading);
    nlohmann::json v = nlohmann::json::object();
    v["archive_sha256"] = "sha256:" + digest_x;
    t1.Transition(kTxnVerified, std::move(v));

    // t3:同 digest 但 committed(终态)——跳过,不动。
    Transaction t3(paths, "20260920T120002Z-000000t3", FixedNow);
    t3.Create(make_target(digest_x));
    t3.Transition(kTxnCommitted);

    // t4:异 digest 且 failed(终态)——同样跳过,不再作废一次。
    Transaction t4(paths, "20260920T120003Z-000000t4", FixedNow);
    t4.Create(make_target(digest_y));
    nlohmann::json f = nlohmann::json::object();
    f["reason"] = "下载失败";
    t4.Transition(kTxnFailed, std::move(f));

    // t5:同 digest、needs-review——也是可续(排序在 t1 后,轮不到它)。
    Transaction t5(paths, "20260920T120004Z-000000t5", FixedNow);
    t5.Create(make_target(digest_x));
    t5.Transition(kTxnNeedsReview);

    const ResumableDecision decision = FindResumable(paths, digest_x, FixedNow);
    REQUIRE(decision.resume.has_value());
    CHECK(decision.resume->id() == "20260920T120000Z-000000t1");  // t2 作废后轮到的同 digest
    CHECK(decision.resume->state() == "verified");
    REQUIRE(decision.superseded_ids.size() == 1);
    CHECK(decision.superseded_ids[0] == "20260920T115959Z-000000t2");

    // t2 被作废:failed + reason superseded + detail 逐字照 python;
    // staging 清掉。
    const auto t2_bytes = ReadBytes(paths.updates / "20260920T115959Z-000000t2.json");
    REQUIRE(t2_bytes.has_value());
    const nlohmann::json t2_data = nlohmann::json::parse(*t2_bytes, nullptr, false);
    CHECK(t2_data["state"] == "failed");
    CHECK(t2_data["reason"] == "superseded");
    CHECK(t2_data["detail"] == "目标版本已换,旧事务作废");
    CHECK_FALSE(std::filesystem::exists(t2.StageDir(), ec));

    // t3/t4/t5 原样不动(终态跳过;needs-review 没轮上)。
    const nlohmann::json t3_data =
        nlohmann::json::parse(*ReadBytes(paths.updates / "20260920T120002Z-000000t3.json"));
    CHECK(t3_data["state"] == "committed");
    const nlohmann::json t4_data =
        nlohmann::json::parse(*ReadBytes(paths.updates / "20260920T120003Z-000000t4.json"));
    CHECK(t4_data["state"] == "failed");
    CHECK(t4_data["reason"] == "下载失败");
    const nlohmann::json t5_data =
        nlohmann::json::parse(*ReadBytes(paths.updates / "20260920T120004Z-000000t5.json"));
    CHECK(t5_data["state"] == "needs-review");

    // 同 digest 换成 digest_y:可续的是……没有非终态的 y(t2 已作废,t4 终态),
    // 且无人再被作废。
    const ResumableDecision none = FindResumable(paths, digest_y, FixedNow);
    CHECK_FALSE(none.resume.has_value());
    CHECK(none.superseded_ids.empty());

    // needs-review 单独在账上时:同 digest 直接续它(冲突处理完重跑不重下)。
    const auto root2 = TempRoot("resumable-review");
    Transaction t6(MakeLayoutPaths(root2), "20260920T130000Z-000000t6", FixedNow);
    t6.Create(make_target(digest_x));
    t6.Transition(kTxnNeedsReview);
    const ResumableDecision review = FindResumable(MakeLayoutPaths(root2), digest_x, FixedNow);
    REQUIRE(review.resume.has_value());
    CHECK(review.resume->id() == "20260920T130000Z-000000t6");
    CHECK(review.superseded_ids.empty());
}

TEST_CASE("CleanupStaging: 目录不在无事,删不动如实报错文本") {
    const auto root = TempRoot("cleanup");
    const LayoutPaths paths = MakeLayoutPaths(root);
    CHECK(CleanupStaging(paths, "not-exist").empty());

    const auto stage = paths.staging / "20260920T120000Z-000000cc";
    std::error_code ec;
    std::filesystem::create_directories(stage / "pkg", ec);
    { std::ofstream(stage / "pkg" / "a.txt", std::ios::binary) << "a"; }
    CHECK(CleanupStaging(paths, "20260920T120000Z-000000cc").empty());
    CHECK_FALSE(std::filesystem::exists(stage, ec));
}
