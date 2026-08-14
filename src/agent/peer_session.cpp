// PeerRuntime 实现。设计见 peer_session.hpp 文件头。

#include "agent/peer_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "platform/process.hpp"

namespace lubancode::agent {

namespace {

long long NowUnix() { return static_cast<long long>(std::time(nullptr)); }

std::string NextMessageId(const std::string& peer_id) {
    static std::atomic<unsigned long long> counter{0};
    std::random_device rd;
    std::ostringstream out;
    out << peer_id << '-' << std::hex << NowUnix() << '-' << (rd() & 0xFFFFU) << '-'
        << ++counter;
    return out.str();
}

bool PidAlive(unsigned long pid) { return platform::IsProcessAlive(pid); }

std::string PermissionModeName(int mode) {
    switch (mode) {
        case 1:
            return "auto";
        case 2:
            return "yolo";
        default:
            return "confirm";
    }
}

int PermissionModeValue(const std::string& name) {
    if (name == "auto") {
        return 1;
    }
    if (name == "yolo") {
        return 2;
    }
    return 0;
}

}  // namespace

std::string DefaultPeerEndpoint(const std::string& peer_id) {
#ifdef _WIN32
    return "\\\\.\\pipe\\lubancode-peer-" + peer_id;
#else
    std::string tmp = "/tmp";
    if (const char* env = std::getenv("TMPDIR"); env != nullptr && *env != '\0') {
        tmp = env;
    }
    while (!tmp.empty() && tmp.back() == '/') {
        tmp.pop_back();
    }
    return tmp + "/lubancode-peer-" + peer_id + ".sock";
#endif
}

PeerRuntime::PeerRuntime(PeerRuntimeOptions options)
    : options_(std::move(options)), registry_(options_.registry_dir) {
    own_.peer_id = GeneratePeerId();
    own_.name = options_.name.empty() ? ("session-" + own_.peer_id) : options_.name;
    own_.session_id = options_.session_id;
    own_.cwd = options_.cwd;
    own_.pid = platform::CurrentProcessId();
    own_.started_at = NowUnix();
    own_.status = "idle";
    own_.endpoint = DefaultPeerEndpoint(own_.peer_id);
    own_.permission_mode = "confirm";
    own_.last_seen = own_.started_at;
}

PeerRuntime::~PeerRuntime() { Stop(); }

bool PeerRuntime::Start(std::string* error) {
    if (running_.load()) {
        return true;
    }
    if (!registry_.EnsureDir()) {
        if (error != nullptr) {
            *error = "peers: cannot create registry dir";
        }
        return false;
    }
    if (!server_.Start(own_.endpoint, [this](const std::string& payload) {
            return HandleRequestOnTransportThread(payload);
        })) {
        if (error != nullptr) {
            *error = server_.last_error();
        }
        return false;
    }
    running_.store(true);
    {
        // 名片上先带上此刻的确认档(收件方算默认权限档要看它),不等第一次
        // 心跳。
        std::lock_guard<std::mutex> lock(card_mutex_);
        if (options_.permission_mode) {
            own_.permission_mode = PermissionModeName(options_.permission_mode());
        }
    }
    RewriteCard();
    heartbeat_thread_ = std::thread([this] {
        while (running_.load()) {
            for (int waited = 0; waited < 100 && running_.load(); ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_.load()) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(card_mutex_);
                own_.last_seen = NowUnix();
                if (options_.permission_mode) {
                    own_.permission_mode = PermissionModeName(options_.permission_mode());
                }
            }
            RewriteCard();
            // 顺手清陈条(心跳过期 / PID 已死),名册不留旧名片。
            static_cast<void>(registry_.ListPeers(NowUnix(), PidAlive));
        }
    });
    return true;
}

void PeerRuntime::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        own_.status = "closing";
        own_.last_seen = NowUnix();
    }
    RewriteCard();
    server_.Stop();          // 先摘服务:此后递来的信连不上,发送方拿 unavailable
    registry_.Remove(own_.peer_id);
}

void PeerRuntime::SetStatus(const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        own_.status = status;
        own_.last_seen = NowUnix();
    }
    RewriteCard();
}

void PeerRuntime::SetName(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        own_.name = name;
        own_.last_seen = NowUnix();
    }
    RewriteCard();
}

void PeerRuntime::SetCwd(const std::string& cwd) {
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        own_.cwd = cwd;
        own_.last_seen = NowUnix();
    }
    RewriteCard();
}

std::vector<PeerCard> PeerRuntime::ListPeers() const {
    std::vector<PeerCard> peers = registry_.ListPeers(NowUnix(), PidAlive);
    std::lock_guard<std::mutex> lock(card_mutex_);
    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [this](const PeerCard& card) { return card.peer_id == own_.peer_id; }),
                peers.end());
    return peers;
}

PeerDelivery PeerRuntime::Send(const PeerCard& target, const std::string& text,
                               const std::optional<std::string>& reply_to) {
    if (!running_.load()) {
        return PeerDelivery::Unavailable;
    }
    PeerEnvelope envelope;
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        envelope.sender_id = own_.peer_id;
        envelope.sender_name = own_.name;
    }
    envelope.message_id = NextMessageId(envelope.sender_id);
    envelope.target_id = target.peer_id;
    envelope.sent_at = NowUnix();
    envelope.reply_to = reply_to;
    envelope.text = text;

    const platform::PeerSendResult sent = platform::PeerPipeSend(target.endpoint, PeerEnvelopeToJson(envelope).dump());
    if (!sent.ok) {
        return PeerDelivery::Unavailable;  // 对方不在(没监听、已退出)
    }
    try {
        const nlohmann::json reply = nlohmann::json::parse(sent.reply);
        const std::string status = reply.value("status", std::string());
        if (status == "delivered") {
            return PeerDelivery::Delivered;
        }
        if (status == "held") {
            return PeerDelivery::Held;
        }
        if (status == "expired") {
            return PeerDelivery::Expired;
        }
        return PeerDelivery::Refused;
    } catch (const nlohmann::json::exception&) {
        return PeerDelivery::Refused;
    }
}

std::vector<PeerIncoming> PeerRuntime::DrainIncoming() {
    std::vector<PeerEnvelope> envelopes = mailbox_.Drain();
    std::vector<PeerIncoming> out;
    out.reserve(envelopes.size());
    {
        std::lock_guard<std::mutex> lock(held_mutex_);
        for (auto& envelope : envelopes) {
            PeerIncoming incoming;
            incoming.held = held_ids_.count(envelope.message_id) != 0;
            held_ids_.erase(envelope.message_id);
            incoming.envelope = std::move(envelope);
            out.push_back(std::move(incoming));
        }
    }
    return out;
}

std::string PeerRuntime::HandleRequestOnTransportThread(const std::string& payload) {
    if (!running_.load()) {
        return "{\"status\":\"unavailable\"}";
    }
    const std::optional<PeerEnvelope> parsed = PeerEnvelopeFromJson(payload);
    if (!parsed.has_value()) {
        return "{\"status\":\"refused\"}";  // 信封不合法,回绝
    }

    // 权限档:用户显式设的优先,否则按两边模式与 cwd 距离算默认。
    PeerPermissionTier effective = tier_.load();
    if (effective == PeerPermissionTier::Auto) {
        const std::vector<PeerCard> peers = registry_.ListPeers(NowUnix(), PidAlive);
        int remote_mode = 0;
        std::string remote_cwd;
        for (const auto& card : peers) {
            if (card.peer_id == parsed->sender_id) {
                remote_mode = PermissionModeValue(card.permission_mode);
                remote_cwd = card.cwd;
                break;
            }
        }
        const int local_mode = options_.permission_mode ? options_.permission_mode() : 0;
        bool far_apart = true;
        {
            std::lock_guard<std::mutex> lock(card_mutex_);
            far_apart = PeerCwdFarApart(own_.cwd, remote_cwd);
        }
        effective = DefaultReceiveTier(local_mode, remote_mode, far_apart);
    }
    if (effective == PeerPermissionTier::Refuse) {
        return "{\"status\":\"refused\"}";
    }

    const PeerOfferStatus offered = mailbox_.Offer(*parsed, NowUnix());
    switch (offered) {
        case PeerOfferStatus::Accepted: {
            if (effective == PeerPermissionTier::Hold) {
                std::lock_guard<std::mutex> lock(held_mutex_);
                held_ids_.insert(parsed->message_id);
                return "{\"status\":\"held\"}";
            }
            return "{\"status\":\"delivered\"}";
        }
        case PeerOfferStatus::Duplicate:
            return "{\"status\":\"delivered\"}";  // 同一封已收过,不重复入队也不算失败
        case PeerOfferStatus::RateLimited:
        case PeerOfferStatus::DuplicateText:
        case PeerOfferStatus::QueueFull:
            return "{\"status\":\"expired\"}";
    }
    return "{\"status\":\"refused\"}";
}

void PeerRuntime::RewriteCard() {
    PeerCard snapshot;
    {
        std::lock_guard<std::mutex> lock(card_mutex_);
        snapshot = own_;
    }
    static_cast<void>(registry_.WriteOwn(snapshot));
}

}  // namespace lubancode::agent
