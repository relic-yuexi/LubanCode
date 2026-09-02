// 会话名册实现。设计见 peer_registry.hpp 文件头。

#include "peers/peer_registry.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>

#include "platform/paths.hpp"  // ReplaceFileAtomically

namespace lubancode::peers {

namespace {

std::string ReadFileToString(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

nlohmann::json PeerCardToJson(const PeerCard& card) {
    nlohmann::json json;
    json["peer_id"] = card.peer_id;
    json["session_id"] = card.session_id;
    json["name"] = card.name;
    json["cwd"] = card.cwd;
    json["pid"] = card.pid;
    json["started_at"] = card.started_at;
    json["status"] = card.status;
    json["endpoint"] = card.endpoint;
    json["permission_mode"] = ApprovalModeMachineName(card.permission_mode);
    json["protocol_version"] = card.protocol_version;
    json["last_seen"] = card.last_seen;
    return json;
}

std::optional<PeerCard> PeerCardFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    // 必填字段缺失/类型不对就整张不要——名册是本机自产自销的,不存在
    // "字段将就着用"的余地。
    const auto peer_id = json.find("peer_id");
    const auto endpoint = json.find("endpoint");
    if (peer_id == json.end() || !peer_id->is_string() || peer_id->get<std::string>().empty() ||
        endpoint == json.end() || !endpoint->is_string()) {
        return std::nullopt;
    }
    PeerCard card;
    card.peer_id = peer_id->get<std::string>();
    card.endpoint = endpoint->get<std::string>();
    const auto read_string = [&](std::string_view key, std::string& out) {
        if (const auto it = json.find(key); it != json.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    read_string("session_id", card.session_id);
    read_string("name", card.name);
    read_string("cwd", card.cwd);
    read_string("status", card.status);
    if (const auto it = json.find("permission_mode"); it != json.end() && it->is_string()) {
        card.permission_mode = ParseApprovalModeOrDefault(it->get<std::string>());
    }
    const auto read_number = [&](std::string_view key, long long& out) {
        if (const auto it = json.find(key); it != json.end() && it->is_number()) {
            out = it->get<long long>();
        }
    };
    long long pid = 0;
    read_number("pid", pid);
    card.pid = static_cast<unsigned long>(pid);
    read_number("started_at", card.started_at);
    read_number("last_seen", card.last_seen);
    if (const auto it = json.find("protocol_version"); it != json.end() && it->is_number()) {
        card.protocol_version = it->get<int>();
    }
    return card;
}

bool PeerCardIsStale(const PeerCard& card, long long now_unix, int ttl_seconds,
                     const std::function<bool(unsigned long)>& pid_alive) {
    if (card.last_seen <= 0 || now_unix - card.last_seen > ttl_seconds) {
        return true;  // 没写过心跳,或心跳过期
    }
    if (card.pid != 0 && pid_alive && !pid_alive(card.pid)) {
        return true;  // 进程已经死了
    }
    return false;
}

PeerRegistry::PeerRegistry(std::filesystem::path dir) : dir_(std::move(dir)) {}

bool PeerRegistry::EnsureDir() const {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    return !ec;
}

bool PeerRegistry::WriteOwn(const PeerCard& card) const {
    if (!EnsureDir()) {
        return false;
    }
    const std::filesystem::path target = dir_ / (card.peer_id + ".json");
    const std::filesystem::path temp = dir_ / (card.peer_id + ".json.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << PeerCardToJson(card).dump();
        out.flush();
        if (!out.good()) {
            return false;
        }
    }
    if (!platform::ReplaceFileAtomically(temp, target).has_value()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool PeerRegistry::Remove(const std::string& peer_id) const {
    if (peer_id.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path target = dir_ / (peer_id + ".json");
    return std::filesystem::remove(target, ec) && !ec;
}

std::vector<PeerCard> PeerRegistry::ListPeers(long long now_unix,
                                              const std::function<bool(unsigned long)>& pid_alive) const {
    std::vector<PeerCard> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const std::string filename = lubancode::platform::PathToUtf8(entry.path().filename());
        if (filename.size() < 6 || filename.compare(filename.size() - 5, 5, ".json") != 0 ||
            (filename.size() >= 9 && filename.compare(filename.size() - 9, 9, ".json.tmp") == 0)) {
            continue;  // 不是名片(可能是写到一半的临时文件)
        }
        try {
            const nlohmann::json json = nlohmann::json::parse(ReadFileToString(entry.path()));
            std::optional<PeerCard> card = PeerCardFromJson(json);
            if (!card.has_value()) {
                continue;
            }
            if (PeerCardIsStale(*card, now_unix, kPeerHeartbeatTtlSeconds, pid_alive)) {
                std::filesystem::remove(entry.path(), ec);
                continue;
            }
            out.push_back(std::move(*card));
        } catch (const nlohmann::json::exception&) {
            // 坏文件(半截写在原子替换之外的场景,比如进程被硬杀):删掉,
            // 不让一张坏名片拖垮整个名册。
            std::filesystem::remove(entry.path(), ec);
        }
    }
    return out;
}

std::string GeneratePeerId() {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    std::ostringstream out;
    out << std::hex;
    for (int i = 0; i < 2; ++i) {
        const unsigned value = (rd() ^ (++counter * 0x9E3779B9U)) & 0xFFFFU;
        out.width(4);
        out.fill('0');
        out << value;
    }
    return out.str();
}

}  // namespace lubancode::peers
