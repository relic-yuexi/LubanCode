// 多渠道消息接入单阶段 2:PairingStore 册(configuration.md §6)。
// 规矩:一次性 code、存 hash 不存明文、短期有效、重复申请限速、
// 批准只认宿主看到的 sender id、approved 持久。
#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>

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
