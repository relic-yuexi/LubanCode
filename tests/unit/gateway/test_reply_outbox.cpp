// 常驻总装 V1:DurableReplyOutbox 单元册。钉的合同(reply_outbox.hpp 与
// contracts.md §3/§4.4/§11):
//   - deliveryId 定式:selectionId + target + ordinal 散列,同一选择恒同
//     id——resume/重扫不另发一份;
//   - 入箱幂等:同 deliveryId 不重复入,正文入箱即冻结不重写;
//   - 本地投递幂等:发布文件已核 hash 相符 → 补回执不出第二份;hash 不符
//     → flagged,不覆盖不删;原件缺失 → flagged(隔离,不跳过继续);
//   - 重启重建:重开投影与关前一致;
//   - 写盘失败:账 broken 停投递。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gateway/reply_outbox.hpp"
#include "platform/sha256.hpp"

using namespace lubancode::gateway;

namespace {

struct Fixture {
    std::filesystem::path root;
    DurableReplyOutbox::Paths paths;

    explicit Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-reply-outbox-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        paths.log_file = root / "outbox.jsonl";
        paths.replies_dir = root / "replies";
        paths.published_dir = root / "out";
    }
};

DurableReplyOutbox::OpenResult Open(DurableReplyOutbox* out, const Fixture& fixture) {
    return DurableReplyOutbox::Open(out, fixture.paths);
}

DurableReplyOutbox::OpenResult Open(DurableReplyOutbox* out,
                                    const DurableReplyOutbox::Paths& paths) {
    return DurableReplyOutbox::Open(out, paths);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("deliveryId 定式:同 (selection, target, ordinal) 恒同一 id") {
    const std::string a = MakeDeliveryId("sel-turn-1", "local:file", 1);
    const std::string b = MakeDeliveryId("sel-turn-1", "local:file", 1);
    const std::string c = MakeDeliveryId("sel-turn-1", "local:file", 2);
    const std::string d = MakeDeliveryId("sel-turn-2", "local:file", 1);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.rfind("dl-", 0) == 0);
    CHECK(a.size() == 3 + 16);
}

TEST_CASE("入箱与本地投递:原件先落、账行后落;发布文件名固定;回执落账") {
    Fixture fixture("deliver");
    DurableReplyOutbox outbox;
    REQUIRE(Open(&outbox, fixture).ok);

    const auto enqueued = outbox.Enqueue("sel-t1", "答复正文", "sess-1", "turn-1", 1000);
    REQUIRE(enqueued.accepted);
    const std::string delivery_id = MakeDeliveryId("sel-t1", "local:file", 1);
    CHECK(enqueued.delivery_id == delivery_id);

    // 原件已落。
    CHECK(ReadText(fixture.paths.replies_dir / "sel-t1.txt") == "答复正文");
    // 未投递前 pending=1。
    CHECK(outbox.PendingCount() == 1);

    const auto delivered = outbox.DeliverPending(2000);
    CHECK(delivered.delivered == 1);
    CHECK(delivered.pending == 0);
    // 发布文件内容 == 冻结正文。
    CHECK(ReadText(fixture.paths.published_dir / (delivery_id + ".txt")) == "答复正文");

    const auto item = outbox.Find(delivery_id);
    REQUIRE(item.has_value());
    CHECK(item->state == "delivered");
}

TEST_CASE("入箱幂等:同 selection 重入回 duplicate,正文不重写不重发") {
    Fixture fixture("idem");
    DurableReplyOutbox outbox;
    REQUIRE(Open(&outbox, fixture).ok);

    REQUIRE(outbox.Enqueue("sel-t1", "第一版", "s", "t", 1).accepted);
    // 同 selectionId 不同正文:不该重写原件(入箱即冻结);duplicate 回执。
    const auto again = outbox.Enqueue("sel-t1", "第二版", "s", "t", 2);
    CHECK(again.duplicate);
    CHECK_FALSE(again.accepted);
    CHECK(ReadText(fixture.paths.replies_dir / "sel-t1.txt") == "第一版");

    const auto items = outbox.ListItems();
    REQUIRE(items.size() == 1);  // 只入了一箱
}

TEST_CASE("窗口:文件已发布、回执未落——重开补回执,不出第二份") {
    Fixture fixture("receipt-window");
    std::string delivery_id;
    {
        DurableReplyOutbox outbox;
        REQUIRE(Open(&outbox, fixture).ok);
        const auto enqueued = outbox.Enqueue("sel-t1", "答复", "s", "t", 1);
        delivery_id = enqueued.delivery_id;
        // 手工模拟"发布文件写了、item.delivered 行没落"的硬杀窗口:
        // 直接拷贝原件到发布位(绕过 DeliverPending 的回执落账)。
        std::error_code ec;
        std::filesystem::create_directories(fixture.paths.published_dir, ec);
        std::ofstream out(fixture.paths.published_dir / (delivery_id + ".txt"), std::ios::binary);
        out << "答复";
    }
    // 重开:pending 投递应发现文件已在且 hash 相符 → 补回执不重写。
    DurableReplyOutbox reopened;
    REQUIRE(Open(&reopened, fixture).ok);
    const auto result = reopened.DeliverPending(3000);
    CHECK(result.delivered == 1);
    CHECK(result.pending == 0);
    // 文件还是一份、内容没动。
    CHECK(ReadText(fixture.paths.published_dir / (delivery_id + ".txt")) == "答复");
    const auto item = reopened.Find(delivery_id);
    REQUIRE(item.has_value());
    CHECK(item->state == "delivered");
}

TEST_CASE("窗口:入箱后未发布——重开从原件续投,不丢不多") {
    Fixture fixture("enqueue-window");
    std::string delivery_id;
    {
        DurableReplyOutbox outbox;
        REQUIRE(Open(&outbox, fixture).ok);
        const auto enqueued = outbox.Enqueue("sel-t1", "答复", "s", "t", 1);
        delivery_id = enqueued.delivery_id;
        // 崩在入箱后、发布前:不开 DeliverPending。
    }
    DurableReplyOutbox reopened;
    REQUIRE(Open(&reopened, fixture).ok);
    CHECK(reopened.PendingCount() == 1);
    const auto result = reopened.DeliverPending(3000);
    CHECK(result.delivered == 1);
    CHECK(ReadText(fixture.paths.published_dir / (delivery_id + ".txt")) == "答复");
}

TEST_CASE("异常面:发布文件 hash 不符 → flagged 不覆盖;原件缺失 → flagged") {
    Fixture fixture("flag");
    DurableReplyOutbox outbox;
    REQUIRE(Open(&outbox, fixture).ok);
    const auto enqueued = outbox.Enqueue("sel-t1", "真答复", "s", "t", 1);
    const std::string delivery_id = enqueued.delivery_id;

    SUBCASE("发布位被别人占了内容不对") {
        std::error_code ec;
        std::filesystem::create_directories(fixture.paths.published_dir, ec);
        std::ofstream out(fixture.paths.published_dir / (delivery_id + ".txt"), std::ios::binary);
        out << "别人的内容";
        const auto result = outbox.DeliverPending(2000);
        CHECK(result.flagged == 1);
        CHECK(ReadText(fixture.paths.published_dir / (delivery_id + ".txt")) == "别人的内容");
    }
    SUBCASE("原件丢了") {
        std::error_code ec;
        std::filesystem::remove(fixture.paths.replies_dir / "sel-t1.txt", ec);
        DurableReplyOutbox fresh;  // 内存正文清空,逼它走原件路
        REQUIRE(Open(&fresh, fixture).ok);
        const auto result = fresh.DeliverPending(2000);
        CHECK(result.flagged == 1);
        CHECK(result.pending == 0);
    }
}

TEST_CASE("写盘失败:账行落不了 → broken") {
    const auto root = std::filesystem::temp_directory_path() /
                      "lubancode-reply-outbox-writefail";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "outbox.jsonl", ec);  // 目录占位
    DurableReplyOutbox outbox;
    DurableReplyOutbox::Paths paths;
    paths.log_file = root / "outbox.jsonl";
    paths.replies_dir = root / "replies";
    paths.published_dir = root / "out";
    REQUIRE(Open(&outbox, paths).ok);
    const auto receipt = outbox.Enqueue("sel-t1", "答复", "s", "t", 1);
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code == "outbox.append_failed");
    CHECK(outbox.broken());
}
