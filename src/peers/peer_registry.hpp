// 跨会话传话(同机首版):会话名册。
//
// 每场交互会话启动后登记一张小名片(规格 todos/跨会话传话与自然排队.todo
// "会话名册"一节),名片是一份 JSON 文件,放在同一用户的注册目录里:
//   <用户状态目录>/peers/<peer_id>.json
// 名册只准同一系统用户读写——目录本身在用户主目录之下,Windows 的
// Named Pipe / POSIX 的 Unix socket 另有各自的用户隔离(见
// platform/peer_transport_*),这里不再另设密码层。注册文件用原子替换
// (先写临时文件再整份换名),心跳过期或 PID 已死的旧名片在读的时候顺手
// 清掉。
//
// 纯逻辑(卡片 JSON 序列化/解析、陈条判定)与文件操作分开,单测在
// tests/unit/peer/test_peer_registry.cpp。
#pragma once

#include "approval_mode.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::peers {

// 名册协议版本。信封(envelope)另有自己的 version 字段,这里管的是名片
// 的字段布局。
constexpr int kPeerCardProtocolVersion = 1;

struct PeerCard {
    std::string peer_id;      // 短 id(8 位十六进制),重名时拿它定人
    std::string session_id;   // 会话存档 id(没有就是空串)
    std::string name;         // 显示名,可由 /title 派生
    std::string cwd;          // 会话工作目录
    unsigned long pid = 0;    // 进程号,用来判死活
    long long started_at = 0; // unix 秒
    std::string status;       // idle | busy | waiting | closing
    std::string endpoint;     // Windows: \\.\pipe\... ; POSIX: socket 路径
    ApprovalMode permission_mode = ApprovalMode::Default;  // 收件方算默认权限档用
    int protocol_version = kPeerCardProtocolVersion;
    long long last_seen = 0;  // unix 秒,心跳刷新
};

// 名片 <-> JSON(纯函数)。
nlohmann::json PeerCardToJson(const PeerCard& card);
std::optional<PeerCard> PeerCardFromJson(const nlohmann::json& json);

// 心跳超过这个秒数没刷新,名片算陈条(测试里可注入"现在"绕开真钟)。
constexpr int kPeerHeartbeatTtlSeconds = 45;

// 一张名片算不算陈条:心跳过期,或者 PID 已经死了(进程不在了,名片留着
// 也送不到信)。pid_alive 由调用方注入(Windows/POSIX 各自的进程存活探测,
// 测试里注假)。纯函数。
bool PeerCardIsStale(const PeerCard& card, long long now_unix, int ttl_seconds,
                     const std::function<bool(unsigned long)>& pid_alive);

class PeerRegistry {
public:
    explicit PeerRegistry(std::filesystem::path dir);

    // 建目录(幂等)。失败返回 false。
    bool EnsureDir() const;

    // 写自己的名片(临时文件 + 原子替换,半截写的名片读者看不到)。
    bool WriteOwn(const PeerCard& card) const;

    // 摘掉一张名片(正常退出时调;崩溃留下的靠陈条清理)。
    bool Remove(const std::string& peer_id) const;

    // 读全部名片。顺手清陈条:心跳过期 / PID 已死的名片文件直接删掉,
    // 不进结果(注册目录是同一用户自己的,读写都名正言顺)。
    std::vector<PeerCard> ListPeers(long long now_unix,
                                    const std::function<bool(unsigned long)>& pid_alive) const;

    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path dir_;
};

// 生成一个短 peer_id(8 位十六进制,进程内随机源)。
std::string GeneratePeerId();

}  // namespace lubancode::peers
