// ChannelWorkLedger 实现(QQ 接入单 Q2)。合同见头文件。
#include "channel/work_ledger.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::channel {

namespace {
constexpr int kWorkLedgerSchema = 1;
}  // namespace

ChannelWorkLedger::OpenResult ChannelWorkLedger::Open(
    ChannelWorkLedger* out, const std::filesystem::path& ledger_file) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "work ledger 装配:out 为空";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(ledger_file.parent_path(), ec);
    if (ec && !ledger_file.parent_path().empty()) {
        result.error = "建 work ledger 目录失败: " + ec.message();
        return result;
    }
    out->ledger_file_ = ledger_file;
    out->bound_.clear();
    out->broken_ = false;
    if (std::filesystem::is_regular_file(ledger_file, ec) && !ec) {
        std::ifstream stream(ledger_file, std::ios::binary);
        std::string text_line;
        while (std::getline(stream, text_line)) {
            if (text_line.empty()) continue;
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(text_line);
            } catch (const nlohmann::json::exception&) {
                continue;
            }
            // json 缺键一律 contains()(const operator[] 查缺键是 UB)。
            if (!parsed.is_object() || !parsed.contains("t") || !parsed["t"].is_string() ||
                parsed["t"].get<std::string>() != "bound" || !parsed.contains("sid") ||
                !parsed["sid"].is_number_integer()) {
                continue;
            }
            BoundWork work;
            work.sid = parsed["sid"].get<std::int64_t>();
            if (parsed.contains("sessionKey") && parsed["sessionKey"].is_string()) {
                work.session_key = parsed["sessionKey"].get<std::string>();
            }
            if (parsed.contains("sessionId") && parsed["sessionId"].is_string()) {
                work.session_id = parsed["sessionId"].get<std::string>();
            }
            if (parsed.contains("turnId") && parsed["turnId"].is_string()) {
                work.turn_id = parsed["turnId"].get<std::string>();
            }
            if (parsed.contains("atMs") && parsed["atMs"].is_number_integer()) {
                work.at_ms = parsed["atMs"].get<std::int64_t>();
            }
            out->bound_.emplace(work.sid, std::move(work));
        }
    }
    result.ok = true;
    return result;
}

bool ChannelWorkLedger::Bind(std::int64_t sid, const std::string& session_key,
                             const std::string& session_id, const std::string& turn_id,
                             std::int64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (broken_) {
        return false;
    }
    if (bound_.find(sid) != bound_.end()) {
        return true;  // 幂等:首笔为准
    }
    if (!writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(
            ledger_file_, trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) {
            broken_ = true;
            last_error_ = writer.error();
            return false;
        }
        writer_ = std::move(*writer);
    }
    nlohmann::json line = nlohmann::json::object();
    line["schema"] = kWorkLedgerSchema;
    line["t"] = "bound";
    line["sid"] = sid;
    line["sessionKey"] = session_key;
    line["sessionId"] = session_id;
    line["turnId"] = turn_id;
    line["atMs"] = at_ms;
    if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        broken_ = true;
        last_error_ = "work ledger 落盘失败: " + platform::PathToUtf8(ledger_file_);
        return false;
    }
    BoundWork work;
    work.sid = sid;
    work.session_key = session_key;
    work.session_id = session_id;
    work.turn_id = turn_id;
    work.at_ms = at_ms;
    bound_.emplace(sid, std::move(work));
    return true;
}

std::optional<ChannelWorkLedger::BoundWork> ChannelWorkLedger::FindBound(
    std::int64_t sid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = bound_.find(sid);
    if (found == bound_.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace lubancode::channel
