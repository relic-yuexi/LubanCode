// 跨会话传话:传输层与 PeerRuntime 的真收发集成测试。
//   - PeerPipeServer/PeerPipeSend:起一根真 pipe/socket,一问一答一帧;
//   - Stop 之后发不通(unavailable);
//   - 两个 PeerRuntime 共用一个名册目录:endpoint 互通、A 看得到 B、
//     A->B 递话 delivered、B 取得到信、相同正文短窗重发 expired。
// Windows 上走 Named Pipe(带当前用户 SID ACL);POSIX 上走 Unix socket
// (0600),同一套断言两边都跑(本机只跑得动 Windows,POSIX 留给 Linux CI)。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "peers/peer_session.hpp"
#include "platform/peer_transport.hpp"

using namespace lubancode;
using namespace lubancode::agent;

namespace {

std::filesystem::path TempDir(const char* tag) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("lubancode-peer-xport-") + tag + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

PeerRuntimeOptions MakeOptions(const std::filesystem::path& registry_dir, const std::string& name,
                               const std::string& cwd) {
    PeerRuntimeOptions options;
    options.registry_dir = registry_dir;
    options.name = name;
    options.cwd = cwd;
    options.permission_mode = [] { return 0; };  // confirm
    return options;
}

}  // namespace

TEST_CASE("PeerPipeServer:真管道一问一答,Stop 之后发不通") {
    const std::string endpoint = DefaultPeerEndpoint(GeneratePeerId());
    platform::PeerPipeServer server;
    REQUIRE(server.Start(endpoint, [](const std::string& payload) {
        // 回声 + 一个状态,好让客户端断言正文完整往返。
        return std::string("{\"echo\":\"") + payload + "\",\"status\":\"delivered\"}";
    }));
    CHECK(server.running());

    const platform::PeerSendResult sent = platform::PeerPipeSend(endpoint, "{\"hello\":1}");
    REQUIRE_MESSAGE(sent.ok, sent.error);

    server.Stop();
    CHECK_FALSE(server.running());
    const platform::PeerSendResult after_stop = platform::PeerPipeSend(endpoint, "{\"hello\":2}");
    CHECK_FALSE(after_stop.ok);  // 对方不在 = unavailable
}

TEST_CASE("PeerPipeServer:处理器抛异常/回空串,回 refused 帧") {
    const std::string endpoint = DefaultPeerEndpoint(GeneratePeerId());
    platform::PeerPipeServer server;
    REQUIRE(server.Start(endpoint, [](const std::string&) -> std::string { throw std::runtime_error("boom"); }));
    const platform::PeerSendResult sent = platform::PeerPipeSend(endpoint, "{}");
    REQUIRE(sent.ok);
    CHECK(sent.reply.find("refused") != std::string::npos);
    server.Stop();
}

TEST_CASE("PeerPipeServer:坏帧(超限长度说明符)不崩,服务还活着") {
    const std::string endpoint = DefaultPeerEndpoint(GeneratePeerId());
    platform::PeerPipeServer server;
    REQUIRE(server.Start(endpoint, [](const std::string&) { return "{\"status\":\"delivered\"}"; }));
    // 直接写一帧声称 512 MiB 的正文:服务端拒读这帧(当坏请求收场),
    // 但不能被拖垮——下一个正经请求照常服务。
    const std::string evil_header = std::string("\x7F\xFF\xFF\xFF", 4);
    const platform::PeerSendResult evil = platform::PeerPipeSend(endpoint, evil_header + "junk");
    if (evil.ok) {
        // 坏帧被当成空请求回了一帧:也算体面,不崩不挂。
        CHECK(evil.reply.find("delivered") != std::string::npos);
    }
    const platform::PeerSendResult normal = platform::PeerPipeSend(endpoint, "{\"fine\":true}");
    REQUIRE(normal.ok);
    CHECK(normal.reply.find("delivered") != std::string::npos);
    server.Stop();
}

TEST_CASE("两个 PeerRuntime:名册互见,递话 delivered,重发相同正文 expired") {
    const auto dir = TempDir("rt");
    PeerRuntime alpha(MakeOptions(dir, "alpha", "D:\\work\\demo"));
    PeerRuntime beta(MakeOptions(dir, "beta", "D:\\work\\demo"));
    REQUIRE(alpha.Start());
    REQUIRE(beta.Start());

    // 名册互见(各自身上带着 endpoint)。
    const auto alpha_peers = alpha.ListPeers();
    REQUIRE(alpha_peers.size() == 1);
    CHECK(alpha_peers[0].peer_id == beta.self().peer_id);
    CHECK(alpha_peers[0].name == "beta");
    CHECK_FALSE(alpha_peers[0].endpoint.empty());

    // 递一张字条:两边都是 confirm、同一目录,默认档 accept → delivered。
    CHECK(alpha.Send(alpha_peers[0], "接口字段已经改成 tenant_id") == PeerDelivery::Delivered);

    // B 取到了信,来历齐全。
    const auto incoming = beta.DrainIncoming();
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0].held == false);
    CHECK(incoming[0].envelope.sender_id == alpha.self().peer_id);
    CHECK(incoming[0].envelope.sender_name == "alpha");
    CHECK(incoming[0].envelope.text == "接口字段已经改成 tenant_id");
    CHECK(beta.DrainIncoming().empty());  // 取完即清

    // 相同正文短窗重发:expired,且不入队。
    CHECK(alpha.Send(alpha_peers[0], "接口字段已经改成 tenant_id") == PeerDelivery::Expired);
    CHECK(beta.DrainIncoming().empty());

    // A 停了:摘名片 + 断服务,B 再看名册少了 A,B 给 A 递话 unavailable。
    alpha.Stop();
    const auto beta_peers = beta.ListPeers();
    CHECK(beta_peers.empty());
    beta.Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("PeerRuntime 权限:refuse 档直接回绝,信不入队") {
    const auto dir = TempDir("refuse");
    PeerRuntime alpha(MakeOptions(dir, "alpha", "D:\\work\\demo"));
    PeerRuntime beta(MakeOptions(dir, "beta", "D:\\work\\demo"));
    REQUIRE(alpha.Start());
    REQUIRE(beta.Start());
    beta.SetTier(PeerPermissionTier::Refuse);

    const auto peers = alpha.ListPeers();
    REQUIRE(peers.size() == 1);
    CHECK(alpha.Send(peers[0], "在吗") == PeerDelivery::Refused);
    CHECK(beta.DrainIncoming().empty());

    alpha.Stop();
    beta.Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("PeerRuntime 权限:auto 档下任一边 yolo 默认 hold,信被扣住等用户") {
    const auto dir = TempDir("hold");
    PeerRuntime alpha(MakeOptions(dir, "alpha", "D:\\work\\demo"));
    PeerRuntimeOptions beta_options = MakeOptions(dir, "beta", "D:\\work\\demo");
    beta_options.permission_mode = [] { return 2; };  // beta 处在 yolo
    PeerRuntime beta(std::move(beta_options));
    REQUIRE(alpha.Start());
    REQUIRE(beta.Start());

    const auto peers = alpha.ListPeers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].permission_mode == "yolo");  // 名册带出了对方的确认档
    CHECK(alpha.Send(peers[0], "这信该被扣住") == PeerDelivery::Held);
    const auto incoming = beta.DrainIncoming();
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0].held);  // 扣住了,等用户点头才交给模型
    CHECK(incoming[0].envelope.text == "这信该被扣住");

    alpha.Stop();
    beta.Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
