// InProcessBridgeEndpoint 册(架构审查 SV-08,三渠道 Bridge 收发机械
// 合并):钉共用件自身的合同——整帧/半帧/粘帧/坏帧、Feed 分派回调里同步
// Reply 不死锁、后台线程 Notify 与宿主 Drain 的队列内容、出站字节与
// bridge_protocol 构造函数逐字节一致。平台业务分派(握手/spool/重连)在
// 各自适配器册(channelqq/channelfeishu/channelwecombot 的 test_*_adapter
// .cpp)原样跑——合并前后字节不变由那三册守门,本册只看共用件本身。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/bridge_endpoint.hpp"
#include "channel/bridge_protocol.hpp"
#include "channel/frame.hpp"

namespace lubancode::channel {
namespace {

// 编一帧"宿主来向"字节(合法 object 正文)。
std::vector<std::byte> HostFrame(const nlohmann::json& payload) {
    const auto encoded = EncodeFrame(payload);
    REQUIRE(encoded.has_value());
    return *encoded;
}

// 手工拼一帧原始字节:4 字节大端长度前缀 + 任意正文(不走 EncodeFrame,
// 用来造"前缀合法、正文非 object"的坏帧)。
std::vector<std::byte> RawFrame(const std::string& body) {
    std::vector<std::byte> frame(4 + body.size());
    const auto len = static_cast<std::uint32_t>(body.size());
    frame[0] = static_cast<std::byte>(len >> 24);
    frame[1] = static_cast<std::byte>(len >> 16);
    frame[2] = static_cast<std::byte>(len >> 8);
    frame[3] = static_cast<std::byte>(len);
    for (std::size_t i = 0; i < body.size(); ++i) {
        frame[4 + i] = static_cast<std::byte>(body[i]);
    }
    return frame;
}

// 把出站字节流整段解开(粘帧也全解);解不开 REQUIRE 红。
std::vector<nlohmann::json> DecodeAll(const std::vector<std::byte>& bytes) {
    FrameDecoder decoder;
    decoder.Feed(bytes.data(), bytes.size());
    std::vector<nlohmann::json> frames;
    while (true) {
        auto next = decoder.TryDecodeNext();
        REQUIRE(next.has_value());
        if (!next->has_value()) {
            break;
        }
        frames.push_back(**next);
    }
    return frames;
}

// 通知帧的 method 名(缺键/错型直接红,nlohmann const 查缺键是 UB,先 contains;
// doctest 断言里不许写 &&,逐条断)。
std::string NotificationMethod(const nlohmann::json& frame) {
    REQUIRE(frame.contains("method"));
    REQUIRE(frame.at("method").is_string());
    return frame.at("method").get<std::string>();
}

TEST_CASE("Feed 整帧分派;半帧等字节,补齐才分派") {
    InProcessBridgeEndpoint endpoint;
    const auto request =
        BuildRequestJson(7, BridgeMethod::Health, nlohmann::json::object());
    const std::vector<std::byte> frame = HostFrame(request);

    std::vector<nlohmann::json> seen;
    endpoint.Feed(frame.data(), frame.size() / 2,
                  [&](const nlohmann::json& f) { seen.push_back(f); });
    CHECK(seen.empty());  // 半帧:不误分派、不报错,等更多字节

    endpoint.Feed(frame.data() + frame.size() / 2, frame.size() - frame.size() / 2,
                  [&](const nlohmann::json& f) { seen.push_back(f); });
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == request);
    CHECK(endpoint.Drain().empty());
}

TEST_CASE("粘帧:一次喂两帧,按序各分派一次") {
    InProcessBridgeEndpoint endpoint;
    const auto first =
        BuildRequestJson(7, BridgeMethod::Health, nlohmann::json::object());
    const auto second =
        BuildRequestJson(8, BridgeMethod::Stop, nlohmann::json::object());

    std::vector<std::byte> stuck = HostFrame(first);
    const std::vector<std::byte> second_frame = HostFrame(second);
    stuck.insert(stuck.end(), second_frame.begin(), second_frame.end());

    std::vector<nlohmann::json> seen;
    endpoint.Feed(stuck.data(), stuck.size(),
                  [&](const nlohmann::json& f) { seen.push_back(f); });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == first);
    CHECK(seen[1] == second);
}

TEST_CASE("坏帧自发 Fatal 且解码器粘性错") {
    InProcessBridgeEndpoint endpoint;
    std::vector<nlohmann::json> seen;
    const auto on_frame = [&](const nlohmann::json& f) { seen.push_back(f); };

    // 前缀合法、正文非 object:FrameDecoder 报 InvalidFrame(粘性)。
    const std::vector<std::byte> bad = RawFrame("[1,2,3]");
    endpoint.Feed(bad.data(), bad.size(), on_frame);
    CHECK(seen.empty());  // 坏帧不进业务分派

    const auto fatal_frames = DecodeAll(endpoint.Drain());
    REQUIRE(fatal_frames.size() == 1);
    CHECK(NotificationMethod(fatal_frames[0]) == BridgeMethodName(BridgeMethod::Fatal));
    REQUIRE(fatal_frames[0].contains("params"));
    REQUIRE(fatal_frames[0].at("params").is_object());
    const auto& params = fatal_frames[0].at("params");
    REQUIRE(params.contains("reason"));
    REQUIRE(params.at("reason").is_string());
    CHECK(params.at("reason").get<std::string>() == "invalid_frame");
    CHECK(params.contains("detail"));  // 帧错说明在 detail(detail 非空)

    // 粘性错:再喂好帧也不分派,只再报一次 Fatal(照原三份机械)。
    const auto good =
        BuildRequestJson(9, BridgeMethod::Health, nlohmann::json::object());
    const std::vector<std::byte> good_frame = HostFrame(good);
    endpoint.Feed(good_frame.data(), good_frame.size(), on_frame);
    CHECK(seen.empty());
    const auto again = DecodeAll(endpoint.Drain());
    REQUIRE(again.size() == 1);
    CHECK(NotificationMethod(again[0]) == BridgeMethodName(BridgeMethod::Fatal));
}

TEST_CASE("Feed 分派回调里同步 Reply 不死锁;出站字节逐帧对得上") {
    InProcessBridgeEndpoint endpoint;
    const auto request =
        BuildRequestJson(11, BridgeMethod::Initialize, nlohmann::json::object());
    const std::vector<std::byte> frame = HostFrame(request);

    // 宿主锁内典型路径:收到请求帧,分派回调里同步回 response + 发通知。
    // Feed 若持输出锁调回调,这里就死锁(测试超时红)——SV-08 验收门。
    endpoint.Feed(frame.data(), frame.size(), [&](const nlohmann::json&) {
        endpoint.ReplyResult(11, nlohmann::json{{"ok", true}});
        endpoint.ReplyDomainError(12, DomainErrorName::NotCapable, "nope");
        endpoint.Notify(BridgeMethod::Status, nlohmann::json{{"state", "running"}});
    });

    const std::vector<std::byte> out = endpoint.Drain();
    CHECK(endpoint.Drain().empty());  // Drain 取后清空

    // 出站字节与 bridge_protocol 构造 + EncodeFrame 的产物逐字节一致
    //(合并件不改写编码路,字节面与抽取前三份相同)。
    std::vector<std::byte> expected = *EncodeFrame(
        BuildResultResponseJson(11, nlohmann::json{{"ok", true}}));
    const auto second = *EncodeFrame(
        BuildDomainErrorResponseJson(12, DomainErrorName::NotCapable, "nope"));
    expected.insert(expected.end(), second.begin(), second.end());
    const auto third = *EncodeFrame(
        BuildNotificationJson(BridgeMethod::Status, nlohmann::json{{"state", "running"}}));
    expected.insert(expected.end(), third.begin(), third.end());
    CHECK(out == expected);
}

TEST_CASE("后台线程 Notify 与宿主 Drain:内容不丢不重") {
    InProcessBridgeEndpoint endpoint;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                endpoint.Notify(BridgeMethod::Status,
                                nlohmann::json{{"seq", t * kPerThread + i}});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto frames = DecodeAll(endpoint.Drain());
    REQUIRE(frames.size() == kThreads * kPerThread);
    // 顺序不限(四线程交错),内容不丢不重:seq 恰为 0..199 一轮。
    std::vector<int> seqs;
    seqs.reserve(frames.size());
    for (const auto& f : frames) {
        REQUIRE(f.contains("params"));
        REQUIRE(f.at("params").is_object());
        const auto& params = f.at("params");
        REQUIRE(params.contains("seq"));
        REQUIRE(params.at("seq").is_number_integer());
        seqs.push_back(params.at("seq").get<int>());
    }
    std::sort(seqs.begin(), seqs.end());
    std::vector<int> want(seqs.size());
    std::iota(want.begin(), want.end(), 0);
    CHECK(seqs == want);
}

}  // namespace
}  // namespace lubancode::channel
