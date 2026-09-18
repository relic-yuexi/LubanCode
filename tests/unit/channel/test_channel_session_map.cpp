// QQ 接入单 Q2 §六:渠道会话映射账与工作绑定账——workspace 隔离、
// resume-as-new 更新映射、重启重放;绑定幂等与反查。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "channel/session_map.hpp"
#include "channel/work_ledger.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path FreshFile(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-session-map-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "account.jsonl";
}

}  // namespace

TEST_CASE("session map:按 (session_key, workspace) 隔离;同键同场幂等") {
    const auto file = FreshFile("isolate");
    ChannelSessionMap map;
    REQUIRE(ChannelSessionMap::Open(&map, file).ok);
    REQUIRE_FALSE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").has_value());
    REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-001", 1000));
    REQUIRE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").value() == "s-001");
    // 不同 workspace 不串场(重启换 cwd 不误续)。
    REQUIRE_FALSE(map.Find("channel:qqbot:main:direct:dm-a", "ws-beta").has_value());
    // 不同会话键各开各场。
    REQUIRE(map.Map("channel:qqbot:main:direct:dm-b", "ws-alpha", "s-002", 1100));
    REQUIRE(map.Find("channel:qqbot:main:direct:dm-b", "ws-alpha").value() == "s-002");
    REQUIRE(map.size() == 2);
    // 同键同场幂等(不追加);resume-as-new 换场才追加。
    REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-001", 1200));
    std::ifstream stream(file);
    std::size_t lines = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) ++lines;
    }
    REQUIRE(lines == 2);
    REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-009", 1300));
    REQUIRE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").value() == "s-009");
}

TEST_CASE("session map:重启重放取最后一笔;坏行容错") {
    const auto file = FreshFile("replay");
    {
        ChannelSessionMap map;
        REQUIRE(ChannelSessionMap::Open(&map, file).ok);
        REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-001", 1000));
        REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-002", 2000));
    }
    {
        ChannelSessionMap map;
        REQUIRE(ChannelSessionMap::Open(&map, file).ok);
        REQUIRE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").value() == "s-002");
    }
    // 坏行(半行/非 JSON)跳过,最后一笔好账仍生效。
    {
        std::ofstream append(file, std::ios::binary | std::ios::app);
        append << "{\"t\":\"mapped\",\"sessionKey\":\"channel:qqbot:main:direct:dm-a\"\n";
        append << "not-json\n";
    }
    {
        ChannelSessionMap map;
        REQUIRE(ChannelSessionMap::Open(&map, file).ok);
        REQUIRE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").value() == "s-002");
        REQUIRE(map.Map("channel:qqbot:main:direct:dm-a", "ws-alpha", "s-003", 3000));
    }
    {
        ChannelSessionMap map;
        REQUIRE(ChannelSessionMap::Open(&map, file).ok);
        REQUIRE(map.Find("channel:qqbot:main:direct:dm-a", "ws-alpha").value() == "s-003");
    }
}

TEST_CASE("work ledger:绑定幂等(首笔为准),重启反查回得来") {
    const auto file = FreshFile("work");
    {
        ChannelWorkLedger ledger;
        REQUIRE(ChannelWorkLedger::Open(&ledger, file).ok);
        REQUIRE(ledger.FindBound(7).has_value() == false);
        REQUIRE(ledger.Bind(7, "channel:qqbot:main:direct:dm-a", "s-001", "turn-1", 1000));
        REQUIRE(ledger.Bind(7, "channel:qqbot:main:direct:dm-a", "s-001", "turn-1", 1100));
        REQUIRE(ledger.Bind(8, "channel:qqbot:main:direct:dm-b", "s-002", "turn-2", 1200));
    }
    {
        ChannelWorkLedger ledger;
        REQUIRE(ChannelWorkLedger::Open(&ledger, file).ok);
        const auto bound = ledger.FindBound(7);
        REQUIRE(bound.has_value());
        REQUIRE(bound->session_key == "channel:qqbot:main:direct:dm-a");
        REQUIRE(bound->session_id == "s-001");
        REQUIRE(bound->turn_id == "turn-1");
        REQUIRE(ledger.FindBound(8)->turn_id == "turn-2");
        REQUIRE_FALSE(ledger.FindBound(9).has_value());
    }
}

TEST_CASE("session map:写不进的账报 broken,不冒充映射已更新") {
    // 账文件位置放一个目录:写者开不了。
    const auto dir = FreshFile("broken");
    const auto file = dir / "sub";
    std::error_code ec;
    std::filesystem::create_directories(file, ec);
    ChannelSessionMap map;
    REQUIRE(ChannelSessionMap::Open(&map, file).ok);
    REQUIRE_FALSE(map.Map("channel:qqbot:main:direct:dm-a", "ws", "s-1", 1));
    REQUIRE(map.broken());
}

TEST_CASE("logical channel sessions persist selection without crossing owner or workspace") {
    const auto file = FreshFile("slots");
    {
        ChannelSessionMap map;
        REQUIRE(ChannelSessionMap::Open(&map, file).ok);
        CHECK(map.ActiveSlot("owner-a", "ws-a") == "default");
        REQUIRE(map.SelectSlot("owner-a", "ws-a", "s-1", true, 1));
        REQUIRE(map.Map(ChannelSessionMap::SlotKey("owner-a", "s-1"), "ws-a", "v3-one", 2));
        CHECK_FALSE(map.SelectSlot("owner-b", "ws-a", "s-1", false, 3));
        CHECK_FALSE(map.SelectSlot("owner-a", "ws-b", "s-1", false, 3));
        CHECK_FALSE(map.SelectSlot("owner-a", "ws-a", "../escape", true, 3));
        REQUIRE(map.SelectSlot("owner-a", "ws-a", "default", false, 4));
    }
    ChannelSessionMap reopened;
    REQUIRE(ChannelSessionMap::Open(&reopened, file).ok);
    CHECK(reopened.ActiveSlot("owner-a", "ws-a") == "default");
    REQUIRE(reopened.SelectSlot("owner-a", "ws-a", "s-1", false, 5));
    CHECK(reopened.Find(ChannelSessionMap::SlotKey("owner-a", "s-1"), "ws-a") == "v3-one");
    CHECK(reopened.Slots("owner-a", "ws-a").size() == 2);
    CHECK(reopened.Slots("owner-b", "ws-a").size() == 1);
}
