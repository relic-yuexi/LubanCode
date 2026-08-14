// 跨会话传话:名册(名片 JSON、写/列、心跳过期与死 PID 清陈条)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agent/peer_registry.hpp"

using namespace lubancode::agent;

namespace {

std::filesystem::path TempDir() {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-peer-test-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

PeerCard MakeCard(const std::string& id, long long last_seen, unsigned long pid = 12345) {
    PeerCard card;
    card.peer_id = id;
    card.session_id = "sess-" + id;
    card.name = "backend";
    card.cwd = "D:\\work\\demo";
    card.pid = pid;
    card.started_at = 900;
    card.status = "idle";
    card.endpoint = "\\\\.\\pipe\\lubancode-peer-" + id;
    card.permission_mode = "confirm";
    card.protocol_version = 1;
    card.last_seen = last_seen;
    return card;
}

const std::function<bool(unsigned long)> kAlwaysAlive = [](unsigned long) { return true; };

}  // namespace

TEST_CASE("名片 JSON:往返无损") {
    const PeerCard card = MakeCard("abcd1234", 1000);
    const nlohmann::json json = PeerCardToJson(card);
    CHECK(json["peer_id"] == "abcd1234");
    CHECK(json["pid"] == 12345);
    CHECK(json["permission_mode"] == "confirm");
    const auto parsed = PeerCardFromJson(json);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == card.name);
    CHECK(parsed->cwd == card.cwd);
    CHECK(parsed->endpoint == card.endpoint);
    CHECK(parsed->last_seen == card.last_seen);
    CHECK(parsed->pid == card.pid);
}

TEST_CASE("名片解析:非对象/缺 peer_id/缺 endpoint 整张不要") {
    CHECK_FALSE(PeerCardFromJson(nlohmann::json::object()).has_value());
    CHECK_FALSE(PeerCardFromJson(nlohmann::json{{"endpoint", "x"}}).has_value());
    CHECK_FALSE(PeerCardFromJson(nlohmann::json{{"peer_id", "a"}}).has_value());
    CHECK_FALSE(PeerCardFromJson(nlohmann::json{{"peer_id", "", "endpoint", "x"}}).has_value());
}

TEST_CASE("写名片 + 列名册:登记可见,重复写是原子替换(旧值不留)") {
    const auto dir = TempDir();
    PeerRegistry registry(dir);
    REQUIRE(registry.WriteOwn(MakeCard("aaaa0001", 2000)));
    REQUIRE(registry.WriteOwn(MakeCard("aaaa0002", 2000)));
    // 覆写同一张:字段变了,文件里只有最新那份。
    PeerCard updated = MakeCard("aaaa0001", 2010);
    updated.status = "busy";
    REQUIRE(registry.WriteOwn(updated));

    const auto peers = registry.ListPeers(2020, kAlwaysAlive);
    REQUIRE(peers.size() == 2);
    int found_busy = 0;
    for (const auto& card : peers) {
        if (card.peer_id == "aaaa0001") {
            CHECK(card.status == "busy");
            CHECK(card.last_seen == 2010);
            ++found_busy;
        }
    }
    CHECK(found_busy == 1);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("摘名片:Remove 之后列不出来") {
    const auto dir = TempDir();
    PeerRegistry registry(dir);
    REQUIRE(registry.WriteOwn(MakeCard("bbbb0001", 1000)));
    CHECK(registry.Remove("bbbb0001"));
    CHECK(registry.ListPeers(1000, kAlwaysAlive).empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("陈条清理:心跳过期或 PID 已死,列的时候顺手删文件") {
    const auto dir = TempDir();
    PeerRegistry registry(dir);
    REQUIRE(registry.WriteOwn(MakeCard("cccc0001", 1000)));              // 新鲜
    REQUIRE(registry.WriteOwn(MakeCard("cccc0002", 1000 - 9999)));       // 心跳早就过期
    REQUIRE(registry.WriteOwn(MakeCard("cccc0003", 1000, /*pid=*/424242)));  // PID 假的死号

    const auto alive = [](unsigned long pid) { return pid != 424242; };
    const auto peers = registry.ListPeers(1000, alive);
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].peer_id == "cccc0001");

    // 陈条文件真被删了:目录里只剩一张名片。
    int cards = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            ++cards;
        }
    }
    CHECK(cards == 1);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("坏名片文件:解析失败时删掉,不拖垮名册") {
    const auto dir = TempDir();
    PeerRegistry registry(dir);
    REQUIRE(registry.WriteOwn(MakeCard("dddd0001", 1000)));
    {
        std::ofstream bad(dir / "broken.json", std::ios::trunc);
        bad << "{ not valid json";
    }
    const auto peers = registry.ListPeers(1000, kAlwaysAlive);
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].peer_id == "dddd0001");
    CHECK_FALSE(std::filesystem::exists(dir / "broken.json"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("PeerCardIsStale:没写过心跳也算陈条") {
    PeerCard card = MakeCard("eeee0001", 0);
    CHECK(PeerCardIsStale(card, 1000, 45, kAlwaysAlive));
    card.last_seen = 1000;
    CHECK_FALSE(PeerCardIsStale(card, 1000, 45, kAlwaysAlive));
    CHECK_FALSE(PeerCardIsStale(card, 1000 + 45, 45, kAlwaysAlive));   // 恰好压线不算过期
    CHECK(PeerCardIsStale(card, 1000 + 46, 45, kAlwaysAlive));
}

TEST_CASE("GeneratePeerId:8 位十六进制,连取不撞") {
    const std::string a = GeneratePeerId();
    const std::string b = GeneratePeerId();
    CHECK(a.size() == 8);
    CHECK(b.size() == 8);
    CHECK(a != b);
}
