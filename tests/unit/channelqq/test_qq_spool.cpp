// QQ sidecar spool 册(QQ 机器人接入单 Q1):先落盘再上报、ACK 后清理、
// 重启重投的耐久账(Q1 验收第二行的 spool 面)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "channel/qq/qq_spool.hpp"

namespace lubancode::channel::qq {
namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-qq-spool-test-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

nlohmann::json EventJson(const std::string& text) {
    return nlohmann::json{{"delivery_id", "d"},
                          {"parts", nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                                         {"text", text}}})}};
}

}  // namespace

TEST_CASE("qq_spool: 落盘-重投-ACK 清理全流程") {
    const auto dir = MakeTempDir("flow");
    auto spool = QqSpoolStore::Open(dir);
    REQUIRE(spool.has_value());
    CHECK(spool->pending_count() == 0);

    REQUIRE(spool->AppendPending("qq-del-1", EventJson("first")).has_value() == false);
    REQUIRE(spool->AppendPending("qq-del-2", EventJson("second")).has_value() == false);
    CHECK(spool->pending_count() == 2);

    // 重启语义:重新 Open 同一目录,ListPending 全量带回。
    auto reopened = QqSpoolStore::Open(dir);
    REQUIRE(reopened.has_value());
    const auto pending = reopened->ListPending();
    REQUIRE(pending.size() == 2);
    CHECK(pending[0].first == "qq-del-1");  // 排序稳定(重投次序确定)
    CHECK(pending[1].first == "qq-del-2");
    CHECK(pending[0].second.at("parts").at(0).at("text") == "first");

    // ACK 清理:清一条,另一条保留(未 ACK 不丢)。
    REQUIRE(reopened->RemoveAcked("qq-del-1").has_value() == false);
    CHECK(reopened->pending_count() == 1);
    // 重复 ACK 幂等。
    REQUIRE(reopened->RemoveAcked("qq-del-1").has_value() == false);
    CHECK(reopened->pending_count() == 1);
}

TEST_CASE("qq_spool: 非法 delivery_id(路径注入)拒绝") {
    const auto dir = MakeTempDir("invalid");
    auto spool = QqSpoolStore::Open(dir);
    REQUIRE(spool.has_value());
    CHECK(spool->AppendPending("../escape", EventJson("x")).has_value() != false);
    CHECK(spool->AppendPending("a/b", EventJson("x")).has_value() != false);
    CHECK(spool->AppendPending("", EventJson("x")).has_value() != false);
    CHECK(spool->AppendPending("ok-id_1.2", EventJson("x")).has_value() == false);
}

TEST_CASE("qq_spool: 坏账文件跳过,不炸重投") {
    const auto dir = MakeTempDir("corrupt");
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir / "good.json", std::ios::binary)
            << EventJson("good").dump();
        std::ofstream(dir / "bad.json", std::ios::binary) << "not json";
        std::ofstream(dir / "partial.json", std::ios::binary) << R"({"broken":)";
    }
    auto spool = QqSpoolStore::Open(dir);
    REQUIRE(spool.has_value());
    const auto pending = spool->ListPending();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].first == "good");
}

}  // namespace lubancode::channel::qq
