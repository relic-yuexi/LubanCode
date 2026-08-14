#include "agent/workflow_recorder.hpp"

#include "platform/json_safe.hpp"  // DumpJsonSanitized:事件行落盘的编码窄边界

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace lubancode::agent {

namespace {

namespace fs = std::filesystem;

// tools/path_utils.hpp 的同款转换。这里不引 tools 头,agent 层文件各自带
// 一份轻量换法是 session_store.cpp 已有的老做法,保持一致。
fs::path Utf8ToPath(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return fs::path(view);
}

std::string PathToUtf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string FormatLocalTime(const char* pattern) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[40]{};
    std::strftime(buffer, sizeof(buffer), pattern, &local);
    return buffer;
}

std::string NowTimestamp() { return FormatLocalTime("%Y-%m-%d %H:%M:%S"); }

std::string NowIdTimestamp() { return FormatLocalTime("%Y%m%d-%H%M%S"); }

// ---------------------------------------------------------------------------
// 脱敏的内部件
// ---------------------------------------------------------------------------

constexpr std::string_view kRedactedMarker = "[已打码]";

// 键形态的敏感词。比对前把文本小写化、'-' 换 '_',再做"包含"判定——
// api-key/api_key/apikey 三种写法归一。
bool ContainsAnyKeyword(const std::string& normalized) {
    static constexpr std::string_view kWords[] = {
        "token", "secret", "password", "passwd", "authorization", "cookie", "api_key", "apikey",
        "private_key", "access_key", "session_key", "client_secret",
    };
    for (const std::string_view word : kWords) {
        if (normalized.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsWordChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-' || c == '.';
}

char LowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// "值游程"的边界:到空白或引号为止(引号外的值不会被截在引号里)。
std::size_t ValueRunEnd(const std::string& text, std::size_t begin) {
    std::size_t end = begin;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' &&
           text[end] != '\r' && text[end] != '"' && text[end] != '\'') {
        ++end;
    }
    return end;
}

// 在 from 起找 "bearer"(词边界),找到返回值起点,找不到 npos。text 已小写。
std::size_t FindBearerWord(const std::string& lower, std::size_t from, std::size_t& word_end_out) {
    const std::size_t at = lower.find("bearer", from);
    if (at == std::string::npos) {
        return std::string::npos;
    }
    const bool left_ok = at == 0 || !IsWordChar(lower[at - 1]);
    std::size_t end = at + 6;
    const bool right_ok = end >= lower.size() || !IsWordChar(lower[end]);
    if (left_ok && right_ok) {
        word_end_out = end;
        return at;
    }
    return FindBearerWord(lower, at + 1, word_end_out);
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

bool IsValidRecorderTransition(RecorderState state, RecorderAction action) {
    switch (action) {
        case RecorderAction::Start:
            return state == RecorderState::Inactive;
        case RecorderAction::Pause:
            return state == RecorderState::Recording;
        case RecorderAction::Resume:
            return state == RecorderState::Paused;
        case RecorderAction::Stop:
        case RecorderAction::Cancel:
            return state == RecorderState::Recording || state == RecorderState::Paused;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 事件序列化
// ---------------------------------------------------------------------------

std::string SerializeRecordEvent(const RecordEvent& event) {
    nlohmann::json root;
    root["seq"] = event.seq;
    root["ts"] = event.ts;
    root["source"] = event.source;
    root["type"] = event.type;
    root["data"] = event.data.is_object() ? event.data : nlohmann::json::object();
    // 窄边界兜底:工具结果摘要万一带着坏字节漏进来,清洗后再落盘,录制件
    // 每行仍是合法 JSON,起草器读得回来(见 platform/json_safe.hpp)。
    return platform::DumpJsonSanitized(root);
}

std::optional<RecordEvent> ParseRecordEvent(const std::string& line) {
    try {
        const nlohmann::json root = nlohmann::json::parse(line);
        if (!root.is_object()) {
            return std::nullopt;
        }
        const auto seq = root.find("seq");
        const auto ts = root.find("ts");
        const auto source = root.find("source");
        const auto type = root.find("type");
        if (seq == root.end() || !seq->is_number_integer() || ts == root.end() || !ts->is_string() ||
            source == root.end() || !source->is_string() || type == root.end() || !type->is_string()) {
            return std::nullopt;
        }
        RecordEvent event;
        event.seq = seq->get<std::int64_t>();
        event.ts = ts->get<std::string>();
        event.source = source->get<std::string>();
        event.type = type->get<std::string>();
        const auto data = root.find("data");
        if (data != root.end() && data->is_object()) {
            event.data = *data;
        }
        return event;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// 脱敏
// ---------------------------------------------------------------------------

std::string RedactSecrets(std::string text) {
    if (text.empty()) {
        return text;
    }
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(LowerAscii(c));
    }

    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // 1) 键形态:敏感词 + 可选空白 + ':'/'=' + 可选空白 + 值。
        //    词边界:词前不是字词字符(大小写按 lower 判,下同)。
        std::size_t match_len = 0;
        if (i == 0 || !IsWordChar(lower[i - 1])) {
            // 逐词试探,取最长命中("client_secret" 压过 "secret")。
            static constexpr std::string_view kKeyWords[] = {
                "authorization", "client_secret", "private_key", "access_key", "session_key",
                "api_key",      "apikey",        "password",    "passwd",     "cookie",
                "token",        "secret",
            };
            for (const std::string_view word : kKeyWords) {
                if (lower.compare(i, word.size(), word) == 0) {
                    match_len = word.size();
                    break;  // 列表已按长度降序,首个命中即最长
                }
            }
        }
        if (match_len > 0) {
            std::size_t p = i + match_len;
            out.append(text, i, p - i);
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) {
                out.push_back(text[p]);
                ++p;
            }
            if (p < text.size() && (text[p] == ':' || text[p] == '=')) {
                out.push_back(text[p]);
                ++p;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) {
                    out.push_back(text[p]);
                    ++p;
                }
                if (p < text.size()) {
                    // 值以 bearer 起头:连同其后的真值一起掩掉。
                    std::size_t bearer_end = 0;
                    if (const std::size_t bearer_at = FindBearerWord(lower, p, bearer_end);
                        bearer_at == p) {
                        out.append(text, p, bearer_end - p);
                        p = bearer_end;
                        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) {
                            out.push_back(text[p]);
                            ++p;
                        }
                    }
                    const std::size_t value_end = ValueRunEnd(text, p);
                    if (value_end > p) {
                        out += kRedactedMarker;
                        i = value_end;
                        continue;
                    }
                }
            }
            // 键后没有 ':'/'=' 或值是空的:只当普通词,从已消费处接着扫
            // (空白已原样发出,i 挪到 p,不能回退重发)。
            i = p;
            continue;
        }

        // 2) 裸形态:"sk-" 起头的 key(token 本体),词边界 + 后面还有字。
        if ((i == 0 || !IsWordChar(lower[i - 1])) && lower.compare(i, 3, "sk-") == 0) {
            std::size_t end = i + 3;
            while (end < text.size() && IsWordChar(text[end])) {
                ++end;
            }
            if (end > i + 5) {  // "sk-" 后至少还有几个字符才像 token,误伤面小
                out += kRedactedMarker;
                i = end;
                continue;
            }
        }

        // 2.5) 复合词形态:字词游程里嵌着 token/secret(如 "test-token-123"、
        // "access_token_value")且带分隔符——裸抄进摘要的密钥长这样,整段掩掉。
        // 宁可多掩,不可漏掩:普通单词("tokens"、"secretly")不带 '-'/'_',
        // 不会命中。
        if (i == 0 || !IsWordChar(lower[i - 1])) {
            std::size_t end = i;
            while (end < text.size() && IsWordChar(text[end])) {
                ++end;
            }
            if (end - i >= 8) {
                bool compound_hit = false;
                for (std::size_t k = i; k + 5 <= end; ++k) {
                    if ((lower.compare(k, 5, "token") == 0 || lower.compare(k, 6, "secret") == 0)) {
                        compound_hit = true;
                        break;
                    }
                }
                if (compound_hit) {
                    bool has_separator = false;
                    for (std::size_t k = i; k < end; ++k) {
                        if (lower[k] == '-' || lower[k] == '_') {
                            has_separator = true;
                            break;
                        }
                    }
                    if (has_separator) {
                        out += kRedactedMarker;
                        i = end;
                        continue;
                    }
                }
            }
        }

        // 3) 裸 bearer:Authorization 头之外的 "Bearer xxx" 也一并掩。
        std::size_t bearer_end = 0;
        if (const std::size_t bearer_at = FindBearerWord(lower, i, bearer_end);
            bearer_at == i && bearer_end > i) {
            out.append(text, i, bearer_end - i);
            std::size_t p = bearer_end;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) {
                out.push_back(text[p]);
                ++p;
            }
            const std::size_t value_end = ValueRunEnd(text, p);
            if (value_end > p) {
                out += kRedactedMarker;
                i = value_end;
                continue;
            }
        }

        out.push_back(text[i]);
        ++i;
    }
    return out;
}

nlohmann::json SanitizeToolInput(const nlohmann::json& input) {
    if (input.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = input.begin(); it != input.end(); ++it) {
            std::string normalized;
            normalized.reserve(it.key().size());
            for (const char c : it.key()) {
                normalized.push_back(c == '-' ? '_' : LowerAscii(c));
            }
            if (ContainsAnyKeyword(normalized)) {
                out[it.key()] = kRedactedMarker;
            } else {
                out[it.key()] = SanitizeToolInput(it.value());
            }
        }
        return out;
    }
    if (input.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : input) {
            out.push_back(SanitizeToolInput(item));
        }
        return out;
    }
    if (input.is_string()) {
        std::string value = input.get<std::string>();
        value = RedactSecrets(std::move(value));
        constexpr std::size_t kMaxParamChars = 1000;  // 长参数只留前段,录制件要小
        if (value.size() > kMaxParamChars) {
            value.resize(kMaxParamChars);
            value += "…";
        }
        return value;
    }
    return input;
}

// ---------------------------------------------------------------------------
// 命名
// ---------------------------------------------------------------------------

std::string MakeRecordingSlug(const std::string& name, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    bool last_dash = false;
    for (std::size_t i = 0; i < name.size() && chars < max_chars;) {
        const unsigned char lead = static_cast<unsigned char>(name[i]);
        std::size_t len = 1;
        if ((lead & 0x80) == 0) {
            len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > name.size()) {
            break;  // 截断的多字节尾巴不硬拆
        }
        const std::string_view piece(name.data() + i, len);
        bool keep = false;
        if (len == 1) {
            const char c = name[i];
            keep = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
        } else {
            keep = true;  // 中文等多字节字符原样留(逐码点,不掐半截)
        }
        if (keep) {
            out.append(piece);
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
        i += len;
        ++chars;
    }
    // 首尾剥 '-'
    std::size_t begin = 0;
    while (begin < out.size() && out[begin] == '-') {
        ++begin;
    }
    std::size_t end = out.size();
    while (end > begin && out[end - 1] == '-') {
        --end;
    }
    out = out.substr(begin, end - begin);
    // 情性残留:点打头的名字(".demo")别当隐藏目录
    if (out.empty() || out == "." || out == "..") {
        return "recording";
    }
    return out;
}

// ---------------------------------------------------------------------------
// WorkflowRecorder
// ---------------------------------------------------------------------------

WorkflowRecorder::WorkflowRecorder(std::filesystem::path dir, std::string name, std::string id,
                                   std::ofstream out)
    : dir_(std::move(dir)), name_(std::move(name)), id_(std::move(id)), out_(std::move(out)) {}

std::expected<WorkflowRecorder, std::string> WorkflowRecorder::Start(const fs::path& recordings_root,
                                                                     const RecordingStartInfo& info) {
    if (recordings_root.empty()) {
        return std::unexpected("录制目录为空(找不到主目录)");
    }
    const std::string id = NowIdTimestamp() + "-" + MakeRecordingSlug(info.name);
    const fs::path dir = recordings_root / Utf8ToPath(id);

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("建录制目录失败: " + PathToUtf8(dir) + ": " + ec.message());
    }

    nlohmann::json manifest;
    manifest["version"] = 1;
    manifest["id"] = id;
    manifest["name"] = info.name;
    manifest["cwd"] = info.cwd;
    manifest["started_at"] = NowTimestamp();
    {
        std::ofstream manifest_file(dir / "manifest.json", std::ios::binary | std::ios::trunc);
        if (!manifest_file.is_open()) {
            return std::unexpected("写录制 manifest 失败: " + PathToUtf8(dir / "manifest.json"));
        }
        manifest_file << manifest.dump(2) << "\n";
        if (!manifest_file.good()) {
            return std::unexpected("写录制 manifest 失败: " + PathToUtf8(dir / "manifest.json"));
        }
    }

    std::ofstream events(dir / "events.jsonl", std::ios::binary | std::ios::app);
    if (!events.is_open()) {
        return std::unexpected("打不开录制事件文件: " + PathToUtf8(dir / "events.jsonl"));
    }

    WorkflowRecorder recorder(dir, info.name, id, std::move(events));

    nlohmann::json start_data;
    start_data["name"] = info.name;
    start_data["cwd"] = info.cwd;
    start_data["goal"] = info.goal;
    start_data["variables"] = info.variables;
    start_data["acceptance"] = info.acceptance;
    recorder.AppendEvent("user", kEventRecordStart, std::move(start_data));
    if (!info.goal.empty()) {
        recorder.AppendEvent("user", kEventGoal, {{"text", info.goal}});
    }
    for (const std::string& variable : info.variables) {
        if (!variable.empty()) {
            recorder.AppendEvent("user", kEventVariable, {{"name", variable}});
        }
    }
    if (recorder.broken_) {
        return std::unexpected("写录制事件失败: " + PathToUtf8(dir / "events.jsonl"));
    }
    return std::move(recorder);
}

void WorkflowRecorder::AppendEvent(const char* source, const char* type, nlohmann::json data) {
    if (broken_ || !out_.is_open()) {
        return;
    }
    RecordEvent event;
    event.seq = next_seq_++;
    event.ts = NowTimestamp();
    event.source = source;
    event.type = type;
    event.data = std::move(data);
    out_ << SerializeRecordEvent(event) << "\n";
    out_.flush();
    if (!out_.good()) {
        broken_ = true;
        out_.close();
        std::cerr << "[record] 写录制事件失败,本场录制件不完整\n";
    }
}

std::expected<void, std::string> WorkflowRecorder::Pause() {
    if (!IsValidRecorderTransition(state_, RecorderAction::Pause)) {
        return std::unexpected("录制进行中才能暂停");
    }
    AppendEvent("system", kEventPause, nlohmann::json::object());
    state_ = RecorderState::Paused;
    return {};
}

std::expected<void, std::string> WorkflowRecorder::Resume() {
    if (!IsValidRecorderTransition(state_, RecorderAction::Resume)) {
        return std::unexpected("暂停中才能续录");
    }
    AppendEvent("system", kEventResume, nlohmann::json::object());
    state_ = RecorderState::Recording;
    return {};
}

std::expected<fs::path, std::string> WorkflowRecorder::Stop(const std::string& final_verification) {
    if (!IsValidRecorderTransition(state_, RecorderAction::Stop)) {
        return std::unexpected("没有在录,谈不上停止");
    }
    if (!final_verification.empty()) {
        AppendEvent("user", kEventVerification, {{"text", final_verification}, {"ok", true}});
    }
    AppendEvent("system", kEventRecordStop, nlohmann::json::object());
    out_.close();
    state_ = RecorderState::Inactive;
    if (broken_) {
        return std::unexpected("录制件落盘失败过,内容不完整");
    }
    return dir_;
}

std::expected<void, std::string> WorkflowRecorder::Cancel() {
    if (!IsValidRecorderTransition(state_, RecorderAction::Cancel)) {
        return std::unexpected("没有在录,谈不上取消");
    }
    out_.close();
    state_ = RecorderState::Inactive;
    std::error_code ec;
    fs::remove_all(dir_, ec);
    if (ec) {
        return std::unexpected("删录制件失败: " + PathToUtf8(dir_) + ": " + ec.message());
    }
    return {};
}

std::expected<void, std::string> WorkflowRecorder::Note(const std::string& text) {
    if (text.empty()) {
        return std::unexpected("备注是空的");
    }
    if (state_ == RecorderState::Inactive) {
        return std::unexpected("没有在录");
    }
    AppendEvent("user", kEventUserNote, {{"text", RedactSecrets(text)}});
    if (broken_) {
        return std::unexpected("录制件落盘失败过,内容不完整");
    }
    return {};
}

void WorkflowRecorder::RecordToolCall(const std::string& tool_name, const nlohmann::json& input) {
    if (state_ != RecorderState::Recording) {
        return;  // 暂停期间的动作不录;静默跳过,不算错
    }
    AppendEvent("model", kEventToolCall,
                {{"tool", tool_name}, {"input", SanitizeToolInput(input)}});
}

void WorkflowRecorder::RecordToolResult(const std::string& tool_name, bool is_error,
                                        const std::string& content) {
    if (state_ != RecorderState::Recording) {
        return;
    }
    // 只留短摘要:首行,截 200 字符,过一遍脱敏。原始大输出仍在会话存档里,
    // 录制件只存这份引用性质的摘要——草稿小,泄密面也小。
    std::size_t line_end = content.find('\n');
    if (line_end == std::string::npos) {
        line_end = content.size();
    }
    std::string summary = content.substr(0, (std::min)(line_end, std::size_t{200}));
    AppendEvent("model", kEventToolResult,
                {{"tool", tool_name}, {"ok", !is_error}, {"summary", RedactSecrets(std::move(summary))}});
}

// ---------------------------------------------------------------------------
// 盘点 / 恢复
// ---------------------------------------------------------------------------

std::vector<RecordEvent> ReadRecordingEvents(const fs::path& recording_dir) {
    std::vector<RecordEvent> events;
    std::ifstream file(recording_dir / "events.jsonl", std::ios::binary);
    if (!file.is_open()) {
        return events;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (const auto event = ParseRecordEvent(line); event.has_value()) {
            events.push_back(std::move(*event));
        }
        // 坏行/半截行(崩溃截断)直接跳过——一场录制不因末尾半行废掉。
    }
    return events;
}

std::vector<RecordingStatus> ListRecordings(const fs::path& recordings_root) {
    std::vector<RecordingStatus> out;
    std::error_code ec;
    if (!fs::exists(recordings_root, ec) || ec || !fs::is_directory(recordings_root, ec)) {
        return out;
    }
    for (fs::directory_iterator it(recordings_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory()) {
            continue;
        }
        const fs::path dir = it->path();
        const fs::path events_path = dir / "events.jsonl";
        const fs::path manifest_path = dir / "manifest.json";
        if (!fs::exists(events_path) && !fs::exists(manifest_path)) {
            continue;  // 两个都没有,不是录制件目录
        }
        RecordingStatus status;
        status.id = PathToUtf8(dir.filename());
        status.dir = dir;
        if (std::ifstream manifest_file(manifest_path, std::ios::binary); manifest_file.is_open()) {
            try {
                const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
                if (manifest.is_object()) {
                    if (const auto it2 = manifest.find("name"); it2 != manifest.end() && it2->is_string()) {
                        status.name = it2->get<std::string>();
                    }
                    if (const auto it2 = manifest.find("started_at"); it2 != manifest.end() && it2->is_string()) {
                        status.started_at = it2->get<std::string>();
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // manifest 坏了不当错误:事件流才是正主
            }
        }
        if (status.name.empty()) {
            status.name = status.id;
        }
        for (const RecordEvent& event : ReadRecordingEvents(dir)) {
            if (event.type == kEventRecordStop) {
                status.finished = true;
                break;
            }
        }
        std::error_code draft_ec;
        status.has_draft = fs::is_regular_file(dir / "draft" / "SKILL.md", draft_ec) && !draft_ec;
        out.push_back(std::move(status));
    }
    std::sort(out.begin(), out.end(),
              [](const RecordingStatus& left, const RecordingStatus& right) { return left.id > right.id; });
    return out;
}

std::expected<void, std::string> DiscardRecording(const fs::path& recordings_root, const std::string& id) {
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
        id.find("..") != std::string::npos || id.find('\0') != std::string::npos) {
        return std::unexpected("录制件编号不合法: " + id);
    }
    const fs::path dir = recordings_root / Utf8ToPath(id);
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return std::unexpected("录制件不存在: " + id);
    }
    fs::remove_all(dir, ec);
    if (ec) {
        return std::unexpected("删录制件失败: " + PathToUtf8(dir) + ": " + ec.message());
    }
    return {};
}

}  // namespace lubancode::agent
