// 多渠道消息接入单阶段 2:PairingStore 册(configuration.md §6)。
// 规矩:一次性 code、存 hash 不存明文、短期有效、重复申请限速、
// 批准只认宿主看到的 sender id、approved 持久。
#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "channel/pairing.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeAccountDir(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-pairing-test" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::unique_ptr<PairingStore> OpenStore(const std::filesystem::path& dir) {
    auto store = PairingStore::Open(dir, "qqbot", "main");
    REQUIRE(store != nullptr);
    REQUIRE_FALSE(store->write_blocked());
    return store;
}

constexpr std::int64_t kT0 = 1'000'000;

}  // namespace

TEST_CASE("申请-批准-放行全流程;code 只此一次,落盘只有 hash") {
    const auto dir = MakeAccountDir("full_flow");
    std::string code;
    {
        auto store = OpenStore(dir);
        const auto got = store->RequestPairing("owner-1", kT0, [] { return "ABCD2345"; });
        REQUIRE(got.has_value());
        code = *got;
        CHECK(code == "ABCD2345");
        CHECK(store->PendingList(kT0 + 1).size() == 1);
        CHECK(store->PendingList(kT0 + 1)[0].sender_id == "owner-1");
        // 批准:拿明文 code,认的是账上的 sender id。
        const auto approved = store->Approve(code, kT0 + 1000);
        REQUIRE(approved.has_value());
        CHECK(*approved == "owner-1");
        CHECK(store->IsSenderApproved("owner-1"));
        CHECK_FALSE(store->IsSenderApproved("someone-else"));
        CHECK(store->approved_count() == 1);
        // code 一次性:再 approve 报 already_finalized。
        const auto again = store->Approve(code, kT0 + 2000);
        CHECK_FALSE(again.has_value());
        CHECK(store->last_error() == "already_finalized");
    }
    // pairing.json 里没有明文 code,只有 hash。
    {
        std::ifstream stream(dir / "pairing.json");
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("ABCD2345") == std::string::npos);
    }
    // 重开:approved 持久。
    auto reopened = OpenStore(dir);
    CHECK(reopened->IsSenderApproved("owner-1"));
    CHECK(reopened->approved_count() == 1);
    // 已批准的 sender 再申请:already_approved,不再发 code。
    CHECK_FALSE(reopened->RequestPairing("owner-1", kT0 + 10'000).has_value());
    CHECK(reopened->last_error() == "already_approved");
}

TEST_CASE("过期 code 不能批准;过期后可重新申请") {
    const auto dir = MakeAccountDir("expiry");
    auto store = OpenStore(dir);
    const auto code = store->RequestPairing("owner-1", kT0, [] { return "WXYZ6789"; });
    REQUIRE(code.has_value());
    // TTL 过了。
    const auto late = store->Approve(*code, kT0 + kPairingCodeTtlMs + 1);
    CHECK_FALSE(late.has_value());
    CHECK(store->last_error() == "expired");
    CHECK_FALSE(store->IsSenderApproved("owner-1"));
    // 冷却期后重新申请,新 code 能批。
    const auto fresh = store->RequestPairing("owner-1", kT0 + kPairingRequestCooldownMs + kT0,
                                             [] { return "QWER2345"; });
    REQUIRE(fresh.has_value());
    CHECK(store->Approve(*fresh, kT0 + kPairingRequestCooldownMs + kT0 + 10).has_value());
    CHECK(store->IsSenderApproved("owner-1"));
}

TEST_CASE("重复申请限速:冷却期内不再发新 code") {
    const auto dir = MakeAccountDir("cooldown");
    auto store = OpenStore(dir);
    REQUIRE(store->RequestPairing("owner-1", kT0, [] { return "AAAA2222"; }).has_value());
    const auto again = store->RequestPairing("owner-1", kT0 + 1000, [] { return "BBBB3333"; });
    CHECK_FALSE(again.has_value());
    CHECK(store->last_error() == "rate_limited");
    // 另一枚 sender 不受限。
    REQUIRE(store->RequestPairing("owner-2", kT0 + 1000, [] { return "CCCC4444"; }).has_value());
    CHECK(store->PendingList(kT0 + 1001).size() == 2);
}

TEST_CASE("拒绝与不认得的 code") {
    const auto dir = MakeAccountDir("reject_notfound");
    auto store = OpenStore(dir);
    const auto code = store->RequestPairing("owner-1", kT0, [] { return "DDDD5555"; });
    REQUIRE(code.has_value());
    std::string error;
    // Reject 成功同样返回 sender id(语义:这一 code 对应的 sender 已处理)。
    const auto rejected = store->Reject(*code, kT0 + 1, &error);
    REQUIRE(rejected.has_value());
    CHECK(*rejected == "owner-1");
    // 拒绝后:sender 未放行,pending 清空。
    CHECK_FALSE(store->IsSenderApproved("owner-1"));
    CHECK(store->PendingList(kT0 + 2).empty());
    // 不认得的 code。
    const auto ghost = store->Approve("ZZZZ9999", kT0 + 3);
    CHECK_FALSE(ghost.has_value());
    CHECK(store->last_error() == "not_found");
}

TEST_CASE("同 sender 至多一枚活 code:旧 pending 作废") {
    const auto dir = MakeAccountDir("single_live_code");
    auto store = OpenStore(dir);
    const auto first = store->RequestPairing("owner-1", kT0, [] { return "EEEE6666"; });
    REQUIRE(first.has_value());
    const auto second =
        store->RequestPairing("owner-1", kT0 + kPairingRequestCooldownMs + 1,
                              [] { return "FFFF7777"; });
    REQUIRE(second.has_value());
    // 旧 code 已被作废:批准它报 already_finalized(状态已非 pending)。
    CHECK_FALSE(store->Approve(*first, kT0 + kPairingRequestCooldownMs + 2).has_value());
    CHECK(store->last_error() == "already_finalized");
    // 新 code 好使。
    CHECK(store->Approve(*second, kT0 + kPairingRequestCooldownMs + 3).has_value());
}

TEST_CASE("默认 code 生成器:长度与字符集") {
    for (int i = 0; i < 8; ++i) {
        const std::string code = PairingStore::DefaultCodeGenerator();
        CHECK(code.size() == kPairingCodeLength);
        for (const char c : code) {
            CHECK((std::isalnum(static_cast<unsigned char>(c)) != 0));
            CHECK(c != '0');
            CHECK(c != 'O');
            CHECK(c != '1');
            CHECK(c != 'I');
        }
    }
}

// ---- Q1b:提示限频账 / 被拒守门 / 按身份批准 / v2 账面 / 只读投影 ---------

TEST_CASE("Q1b 提示限频:冷却窗内只记一次,持久后重启不重发") {
    const auto dir = MakeAccountDir("notice_cooldown");
    {
        auto store = OpenStore(dir);
        CHECK(store->MarkNoticeSent("owner-1", "dm-a", kT0));
        // 窗内第二笔:拒,不更新账。
        CHECK_FALSE(store->MarkNoticeSent("owner-1", "dm-b", kT0 + 1000));
        REQUIRE(store->NoticeLog().size() == 1);
        CHECK(store->NoticeLog()[0].conversation_id == "dm-a");
        CHECK(store->NoticeLog()[0].notified_at_ms == kT0);
        // 另一枚 sender 不受限。
        CHECK(store->MarkNoticeSent("owner-2", "dm-c", kT0 + 1000));
        // 冷却窗过了:允许再发,会话更新到最新。
        CHECK(store->MarkNoticeSent("owner-1", "dm-b", kT0 + kPairingNoticeCooldownMs));
        REQUIRE(store->NoticeLog().size() == 2);
        CHECK(store->NoticeLog()[0].conversation_id == "dm-b");
    }
    // 重启(重开账):限频账还在,同窗内仍拒——不重发刷屏。
    {
        auto store = OpenStore(dir);
        CHECK_FALSE(store->MarkNoticeSent("owner-1", "dm-b",
                                          kT0 + kPairingNoticeCooldownMs + 1000));
        CHECK(store->MarkNoticeSent("owner-1", "dm-b",
                                    kT0 + 2 * kPairingNoticeCooldownMs));
    }
}

TEST_CASE("Q1b 被拒守门:拒绝过的 sender 不再发 code(不再收提示)") {
    const auto dir = MakeAccountDir("rejected_sender");
    auto store = OpenStore(dir);
    const auto code = store->RequestPairing("spammer", kT0, [] { return "GGGG8888"; });
    REQUIRE(code.has_value());
    std::string error;
    REQUIRE(store->Reject(*code, kT0 + 1, &error).has_value());
    CHECK(store->IsSenderRejected("spammer"));
    // 冷却期过了也不给新 code:被拒是一笔持久事实,不是限速。
    CHECK_FALSE(store->RequestPairing("spammer", kT0 + kPairingRequestCooldownMs + kT0,
                                      [] { return "HHHH9999"; })
                    .has_value());
    CHECK(store->last_error() == "sender_rejected");
    // 别的 sender 照常。
    REQUIRE(store->RequestPairing("owner-2", kT0 + 100, [] { return "JJJJ2222"; }).has_value());
}

TEST_CASE("Q1b 按身份批准/拒绝:结算该 sender 最新一枚 pending") {
    const auto dir = MakeAccountDir("by_sender");
    auto store = OpenStore(dir);
    std::string error;
    // 没有待审:明报 not_found。
    CHECK_FALSE(store->ApproveSender("nobody", kT0, &error).has_value());
    CHECK(error == "not_found");
    // 造一枚 pending。
    REQUIRE(store->RequestPairing("owner-1", kT0, [] { return "KKKK3333"; }).has_value());
    const auto approved = store->ApproveSender("owner-1", kT0 + 10, &error);
    REQUIRE(approved.has_value());
    CHECK(*approved == "owner-1");
    CHECK(store->IsSenderApproved("owner-1"));
    // 已批准再按身份批:already_finalized,不翻旧账。
    CHECK_FALSE(store->ApproveSender("owner-1", kT0 + 20, &error).has_value());
    CHECK(error == "already_finalized");
    // 拒绝路:最新 pending 被 reject。
    REQUIRE(store->RequestPairing("owner-3", kT0 + 1000, [] { return "LLLL4444"; }).has_value());
    const auto rejected = store->RejectSender("owner-3", kT0 + 1010, &error);
    REQUIRE(rejected.has_value());
    CHECK(*rejected == "owner-3");
    CHECK(store->IsSenderRejected("owner-3"));
}

TEST_CASE("Q1b 按身份批准:pending 过期如实报 expired,不当 not_found") {
    const auto dir = MakeAccountDir("by_sender_expiry");
    auto store = OpenStore(dir);
    REQUIRE(store->RequestPairing("owner-1", kT0, [] { return "MMMM5555"; }).has_value());
    std::string error;
    CHECK_FALSE(store->ApproveSender("owner-1", kT0 + kPairingCodeTtlMs + 1, &error).has_value());
    CHECK(error == "expired");
    CHECK_FALSE(store->IsSenderApproved("owner-1"));
}

TEST_CASE("Q1b v2 账面:records+notices 同盘持久;旧裸数组读作无提示账") {
    const auto dir = MakeAccountDir("v2_format");
    {
        auto store = OpenStore(dir);
        REQUIRE(store->RequestPairing("owner-1", kT0, [] { return "NNNN6666"; }).has_value());
        REQUIRE(store->MarkNoticeSent("owner-1", "dm-a", kT0 + 5));
        // 盘上是对象(schema_version 2),records 与 notices 两数组。
        std::ifstream stream(dir / "pairing.json");
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        CHECK(text.find("\"schema_version\"") != std::string::npos);
        CHECK(text.find("\"records\"") != std::string::npos);
        CHECK(text.find("\"notices\"") != std::string::npos);
    }
    {
        auto store = OpenStore(dir);
        CHECK(store->NoticeLog().size() == 1);
        CHECK(store->NoticeLog()[0].sender_id == "owner-1");
        // 旧格式(裸数组)重放:records 还在,notices 读作空(旧进程没发过
        // 提示,空账即事实——第一次提示不再被旧冷却挡住)。
        std::vector<PairingStore::Record> records = store->Records();
        REQUIRE(records.size() == 1);
        std::error_code ec;
        std::filesystem::remove(dir / "pairing.json", ec);
        nlohmann::json legacy = nlohmann::json::array();
        // 注意:内容不能手拼 string——用 json 对象序列化。
        // 这里直接把旧账写成单元素数组(RecordToJson 的同形状)。
        nlohmann::json record = nlohmann::json::object({
            {"channel_id", "qqbot"},
            {"account_id", "main"},
            {"sender_id", "owner-1"},
            {"code_hash", records[0].code_hash},
            {"created_at_ms", records[0].created_at_ms},
            {"expires_at_ms", records[0].expires_at_ms},
            {"status", "pending"},
        });
        legacy.push_back(record);
        std::ofstream(dir / "pairing.json", std::ios::trunc) << legacy.dump();
    }
    {
        auto store = OpenStore(dir);
        CHECK(store->NoticeLog().empty());
        // TTL(5 分钟)过了 pending 不再列(裸数组重放的那枚,kT0 基)。
        const std::int64_t kLater = kT0 + kPairingCodeTtlMs + 1000;
        CHECK(store->PendingList(kLater).empty());
        CHECK(store->MarkNoticeSent("owner-1", "dm-z", kLater));
    }
}

TEST_CASE("Q1b 只读投影:零建目录零写盘;批准/待审计数;坏账如实报") {
    const auto dir = MakeAccountDir("projection");
    // 账不在:present=false,不建目录。
    const auto missing = PairingStore::ReadProjection(dir / "ghost");
    CHECK_FALSE(missing.present);
    CHECK(missing.parse_ok);
    CHECK_FALSE(std::filesystem::exists(dir / "ghost"));

    // ReadProjection 的 pending 过滤走真墙钟——这里用真实时间基(非 kT0)。
    const std::int64_t real_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
    auto store = OpenStore(dir);
    REQUIRE(store->RequestPairing("owner-1", real_now, [] { return "PPPP7777"; }).has_value());
    REQUIRE(store->RequestPairing("owner-2", real_now + 100, [] { return "QQRR8899"; })
                .has_value());
    REQUIRE(store->Approve("PPPP7777", real_now + 200).has_value());
    {
        const auto projection = PairingStore::ReadProjection(dir);
        CHECK(projection.present);
        CHECK(projection.parse_ok);
        CHECK(projection.approved == 1);
        CHECK(projection.pending == 1);
    }
    // 坏账:parse_ok=false,不冒充 0 个。
    std::ofstream(dir / "pairing.json", std::ios::trunc) << "{ not json";
    const auto broken = PairingStore::ReadProjection(dir);
    CHECK(broken.present);
    CHECK_FALSE(broken.parse_ok);
}

TEST_CASE("Q1b 提示正文:含配对指引与 approve 命令,零敏感字段") {
    const std::string text = MakePairingNoticeText("ABCD2345", "qqbot", "main");
    CHECK(text.find("ABCD2345") != std::string::npos);
    CHECK(text.find("lubancode channel pairing approve qqbot main ABCD2345") != std::string::npos);
    CHECK(text.find("重新发送") != std::string::npos);  // 批准后不自动补跑
    // 零敏感:不带"secret/token/密钥"字样(码是配对码,一次性,不是凭据)。
    CHECK(text.find("secret") == std::string::npos);
    CHECK(text.find("token") == std::string::npos);
}
