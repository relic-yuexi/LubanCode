#include "hooks/outbox.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::hooks {

namespace {

std::int64_t UnixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::optional<std::filesystem::path> HookOutbox::DefaultPath() {
    const auto home = platform::HomeDir();
    if (!home.has_value() || home->empty()) {
        return std::nullopt;
    }
    std::filesystem::path dir(*home);
    dir /= ".lubancode";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // 已在也成功;失败交给 Open 报
    return dir / "hooks-outbox.jsonl";
}

std::shared_ptr<HookOutbox> HookOutbox::Open(const std::filesystem::path& path) {
    auto outbox = std::shared_ptr<HookOutbox>(new HookOutbox());
    outbox->path_ = path;
    outbox->LoadAndCompact();
    // 追加流:压不住盘(目录建不出/盘只读)就回 nullptr,调用方降级。
    outbox->append_.open(path, std::ios::app);
    if (!outbox->append_.is_open()) {
        return nullptr;
    }
    return outbox;
}

void HookOutbox::LoadAndCompact() {
    std::ifstream read(path_);
    if (!read.is_open()) {
        return;  // 没有旧账,从空账开张
    }
    struct PendingLine {
        std::uint64_t id = 0;
        std::string event_id;
        std::string handler_hash;
        std::string event_name;
    };
    std::vector<PendingLine> still_pending;
    std::string line;
    std::set<std::uint64_t> acked;
    while (std::getline(read, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const nlohmann::json row = nlohmann::json::parse(line);
            const std::string kind = row.at("kind").get<std::string>();
            const std::uint64_t id = row.at("id").get<std::uint64_t>();
            if (kind == "ack") {
                acked.insert(id);
                continue;
            }
            if (kind != "pending") {
                ++dropped_lines_;
                continue;
            }
            PendingLine parsed;
            parsed.id = id;
            parsed.event_id = row.at("event_id").get<std::string>();
            parsed.handler_hash = row.at("handler_hash").get<std::string>();
            parsed.event_name = row.value("event", std::string());
            still_pending.push_back(std::move(parsed));
        } catch (const nlohmann::json::exception&) {
            ++dropped_lines_;  // 坏行(截断/手改):计数丢弃,不拦开张
        } catch (const std::exception&) {
            ++dropped_lines_;
        }
    }
    // 账面重建:pending 行进 keys_/pending_;acked 只推高 next_id_ 防重号。
    for (const auto& row : still_pending) {
        keys_[{row.event_id, row.handler_hash}] = row.id;
        pending_.insert(row.id);
        if (row.id >= next_id_) {
            next_id_ = row.id + 1;
        }
    }
    if (!acked.empty()) {
        next_id_ = (std::max)(next_id_, *acked.rbegin() + 1);
    }
    // 压实:重写只留 pending(acked 行出账)。重写失败不拦——追加流照开,
    // 账面以内存为准,坏的那份下次再试。
    std::ofstream rewrite(path_, std::ios::trunc);
    if (!rewrite.is_open()) {
        return;
    }
    for (const auto& row : still_pending) {
        rewrite << nlohmann::json{{"kind", "pending"},
                                  {"id", row.id},
                                  {"event_id", row.event_id},
                                  {"handler_hash", row.handler_hash},
                                  {"event", row.event_name},
                                  {"ts", UnixNowMs()}}
                       .dump()
                << "\n";
    }
}

std::uint64_t HookOutbox::RecordPending(const std::string& event_id, const std::string& handler_definition_hash,
                                        const std::string& event_name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::pair<std::string, std::string> key(event_id, handler_definition_hash);
    const auto existing = keys_.find(key);
    if (existing != keys_.end()) {
        return 0;  // 幂等键已在账:重放只记一次,不重复落盘
    }
    const std::uint64_t id = next_id_++;
    if (append_.is_open()) {
        append_ << nlohmann::json{{"kind", "pending"},
                                  {"id", id},
                                  {"event_id", event_id},
                                  {"handler_hash", handler_definition_hash},
                                  {"event", event_name},
                                  {"ts", UnixNowMs()}}
                       .dump()
                << "\n";
        append_.flush();
    }
    keys_[key] = id;
    pending_.insert(id);
    return id;
}

void HookOutbox::Ack(std::uint64_t entry_id) {
    if (entry_id == 0) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.erase(entry_id) == 0) {
        return;  // 认不得/已销:幂等安静忽略
    }
    if (append_.is_open()) {
        append_ << nlohmann::json{{"kind", "ack"}, {"id", entry_id}, {"ts", UnixNowMs()}}.dump() << "\n";
        append_.flush();
    }
}

bool HookOutbox::Contains(const std::string& event_id, const std::string& handler_definition_hash) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return keys_.count({event_id, handler_definition_hash}) > 0;
}

std::size_t HookOutbox::pending_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::size_t HookOutbox::total_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return keys_.size();
}

std::size_t HookOutbox::dropped_lines() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_lines_;
}

}  // namespace lubancode::hooks
