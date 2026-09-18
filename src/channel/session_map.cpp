// ChannelSessionMap 实现(QQ 接入单 Q2)。合同见头文件。
#include "channel/session_map.hpp"

#include <fstream>
#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::channel {

namespace {

constexpr int kSessionMapSchema = 1;

}  // namespace

ChannelSessionMap::OpenResult ChannelSessionMap::Open(ChannelSessionMap* out,
                                                      const std::filesystem::path& map_file) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "session map 装配:out 为空";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(map_file.parent_path(), ec);
    if (ec && !map_file.parent_path().empty()) {
        result.error = "建 session map 目录失败: " + ec.message();
        return result;
    }
    out->map_file_ = map_file;
    out->latest_.clear();
    out->slots_.clear();
    out->active_slots_.clear();
    out->broken_ = false;
    // 重放:逐行取最后一笔(半行/坏行跳过,不猜)。
    if (std::filesystem::is_regular_file(map_file, ec) && !ec) {
        std::ifstream stream(map_file, std::ios::binary);
        std::string text_line;
        while (std::getline(stream, text_line)) {
            if (text_line.empty()) continue;
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(text_line);
            } catch (const nlohmann::json::exception&) {
                continue;
            }
            if (parsed.is_object() && parsed.contains("t") && parsed["t"].is_string() && parsed["t"] == "selected" &&
                parsed.contains("sessionKey") && parsed["sessionKey"].is_string() &&
                parsed.contains("workspaceKey") && parsed["workspaceKey"].is_string() &&
                parsed.contains("slot") && parsed["slot"].is_string()) {
                const auto key = std::make_pair(parsed["sessionKey"].get<std::string>(),
                                                parsed["workspaceKey"].get<std::string>());
                const auto slot = parsed["slot"].get<std::string>();
                auto& slots = out->slots_[key];
                if (std::find(slots.begin(), slots.end(), slot) == slots.end()) slots.push_back(slot);
                out->active_slots_[key] = slot;
                continue;
            }
            if (!parsed.is_object() || !parsed.contains("t") || !parsed["t"].is_string() ||
                parsed["t"].get<std::string>() != "mapped") {
                continue;
            }
            // json 缺键一律 contains()(const operator[] 查缺键是 UB)。
            if (!parsed.contains("sessionKey") || !parsed["sessionKey"].is_string() ||
                !parsed.contains("workspaceKey") || !parsed["workspaceKey"].is_string() ||
                !parsed.contains("sessionId") || !parsed["sessionId"].is_string()) {
                continue;
            }
            out->latest_[std::make_pair(parsed["sessionKey"].get<std::string>(),
                                        parsed["workspaceKey"].get<std::string>())] =
                parsed["sessionId"].get<std::string>();
        }
    }
    // 写柄:首笔才开(占位文件零写盘)。
    result.ok = true;
    return result;
}

bool ChannelSessionMap::Map(const std::string& session_key, const std::string& workspace_key,
                            const std::string& session_id, std::int64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (broken_) {
        return false;
    }
    const auto key = std::make_pair(session_key, workspace_key);
    const auto existing = latest_.find(key);
    if (existing != latest_.end() && existing->second == session_id) {
        return true;  // 幂等:映射没变不追加
    }
    if (!writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(
            map_file_, trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) {
            broken_ = true;
            last_error_ = writer.error();
            return false;
        }
        writer_ = std::move(*writer);
    }
    nlohmann::json line = nlohmann::json::object();
    line["schema"] = kSessionMapSchema;
    line["t"] = "mapped";
    line["sessionKey"] = session_key;
    line["workspaceKey"] = workspace_key;
    line["sessionId"] = session_id;
    line["atMs"] = at_ms;
    if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        broken_ = true;
        last_error_ = "session map 落盘失败: " + platform::PathToUtf8(map_file_);
        return false;
    }
    latest_[key] = session_id;
    return true;
}

std::optional<std::string> ChannelSessionMap::Find(const std::string& session_key,
                                                   const std::string& workspace_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = latest_.find(std::make_pair(session_key, workspace_key));
    if (found == latest_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t ChannelSessionMap::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_.size();
}

std::string ChannelSessionMap::SlotKey(const std::string& session_key, const std::string& slot) {
    return slot == "default" ? session_key : session_key + ":slot:" + slot;
}

std::string ChannelSessionMap::ActiveSlot(const std::string& session_key,
                                         const std::string& workspace_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_slots_.find({session_key, workspace_key});
    return it == active_slots_.end() ? "default" : it->second;
}

std::vector<std::string> ChannelSessionMap::Slots(const std::string& session_key,
                                                const std::string& workspace_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result{"default"};
    const auto it = slots_.find({session_key, workspace_key});
    if (it != slots_.end()) {
        for (const auto& slot : it->second) if (slot != "default") result.push_back(slot);
    }
    return result;
}

bool ChannelSessionMap::SelectSlot(const std::string& session_key, const std::string& workspace_key,
                                   const std::string& slot, bool create, std::int64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (broken_ || slot.empty() || slot.size() > 64 ||
        slot.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos) return false;
    const auto key = std::make_pair(session_key, workspace_key);
    auto& slots = slots_[key];
    const bool known = slot == "default" || std::find(slots.begin(), slots.end(), slot) != slots.end();
    if (!create && !known) return false;
    if (!writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(map_file_, trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) { broken_ = true; last_error_ = writer.error(); return false; }
        writer_ = std::move(*writer);
    }
    const nlohmann::json line = {{"schema", kSessionMapSchema}, {"t", "selected"},
        {"sessionKey", session_key}, {"workspaceKey", workspace_key}, {"slot", slot}, {"atMs", at_ms}};
    if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        broken_ = true; last_error_ = "会话选择落盘失败"; return false;
    }
    if (!known) slots.push_back(slot);
    active_slots_[key] = slot;
    return true;
}

}  // namespace lubancode::channel
