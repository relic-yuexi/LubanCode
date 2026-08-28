// EvolutionObservation 的实现:序列化/解析、指纹归一、脱敏窄口。纯函数,
// 不碰磁盘、不碰终端。脱敏复用 skills 的打码器(见头注释),这里不另写一套。

#include "evolution/observation.hpp"

#include <cctype>
#include <ctime>

#include "hooks/hash.hpp"
#include "skills/workflow_recorder.hpp"  // RedactSecrets/SanitizeToolInput:密钥这道门的既有设施

namespace lubancode::evolution {

namespace {

// ASCII 小写(中文等非 ASCII 原样;指纹口径只折 ASCII 大小写)。
std::string ToLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// 纯数字词(长度 8 的认日期;其余年份/计数原样)。
bool AllDigits(const std::string& word) {
    if (word.empty()) return false;
    for (const char c : word) {
        if (!IsDigit(c)) return false;
    }
    return true;
}

// yyyy-m(N)-d(N) 或 yyyy/m/d(分隔符混用也认;只看形状,不验月日范围)。
bool LooksLikeDate(const std::string& word) {
    const std::size_t first_sep = word.find_first_of("-/");
    if (first_sep == std::string::npos) return false;
    const std::size_t second_sep = word.find_first_of("-/", first_sep + 1);
    if (second_sep == std::string::npos) return false;
    if (word.find_first_of("-/", second_sep + 1) != std::string::npos) return false;
    if (first_sep != 4) return false;  // 年四位
    const std::size_t month_len = second_sep - first_sep - 1;
    const std::size_t day_len = word.size() - second_sep - 1;
    if (month_len < 1 || month_len > 2 || day_len < 1 || day_len > 2) return false;
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (i == first_sep || i == second_sep) continue;
        if (!IsDigit(word[i])) return false;
    }
    return true;
}

bool LooksLikeAbsolutePath(const std::string& word) {
    if (word.size() >= 3 && word[1] == ':' && (word[2] == '\\' || word[2] == '/')) {
        return IsDigit(static_cast<unsigned char>(word[0])) ||
               (word[0] >= 'a' && word[0] <= 'z');
    }
    if (word.rfind("\\\\", 0) == 0) return true;  // UNC
    if (!word.empty() && word.front() == '/') return true;  // POSIX 绝对
    return false;
}

void PutIfNotEmpty(nlohmann::json& j, const char* key, const std::string& value) {
    if (!value.empty()) {
        j[key] = value;
    }
}

std::string GetStr(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) {
        return j.at(key).get<std::string>();
    }
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举序列化
// ---------------------------------------------------------------------------

std::string ToString(ObservationSource source) {
    switch (source) {
        case ObservationSource::Run: return "run";
        case ObservationSource::Goal: return "goal";
        case ObservationSource::Recording: return "recording";
        case ObservationSource::ToolTrace: return "tooltrace";
        case ObservationSource::Memory: return "memory";
        case ObservationSource::UserFeedback: return "user_feedback";
    }
    return "unknown";
}

bool ParseObservationSource(const std::string& text, ObservationSource& out) {
    if (text == "run") { out = ObservationSource::Run; return true; }
    if (text == "goal") { out = ObservationSource::Goal; return true; }
    if (text == "recording") { out = ObservationSource::Recording; return true; }
    if (text == "tooltrace") { out = ObservationSource::ToolTrace; return true; }
    if (text == "memory") { out = ObservationSource::Memory; return true; }
    if (text == "user_feedback") { out = ObservationSource::UserFeedback; return true; }
    return false;
}

std::string ToString(ObservationOutcome outcome) {
    switch (outcome) {
        case ObservationOutcome::Success: return "success";
        case ObservationOutcome::Failure: return "failure";
        case ObservationOutcome::Partial: return "partial";
        case ObservationOutcome::Unknown: return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 序列化 / 解析
// ---------------------------------------------------------------------------

std::string SerializeObservation(const EvolutionObservation& observation) {
    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = observation.id;
    j["source"] = ToString(observation.source);
    j["source_id"] = observation.source_id;
    PutIfNotEmpty(j, "source_ref", observation.source_ref);
    j["summary"] = observation.summary;
    j["outcome"] = ToString(observation.outcome);
    j["fingerprint"] = observation.fingerprint;
    j["details"] = observation.details.is_object() ? observation.details
                                                   : nlohmann::json::object();
    if (!observation.evidence.empty()) {
        nlohmann::json evidence = nlohmann::json::array();
        for (const EvidenceRef& ref : observation.evidence) {
            evidence.push_back(nlohmann::json{{"ref", ref.ref}, {"note", ref.note}});
        }
        j["evidence"] = std::move(evidence);
    }
    PutIfNotEmpty(j, "created_at", observation.created_at);
    return j.dump();
}

std::optional<EvolutionObservation> ParseObservation(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("schema") || !j.at("schema").is_number_integer() ||
        j.at("schema").get<int>() != 1) {
        return std::nullopt;
    }
    const std::string id = GetStr(j, "id");
    const std::string source_text = GetStr(j, "source");
    const std::string source_id = GetStr(j, "source_id");
    if (id.empty() || source_id.empty()) return std::nullopt;
    ObservationSource source = ObservationSource::Run;
    if (!ParseObservationSource(source_text, source)) return std::nullopt;

    EvolutionObservation observation;
    observation.id = id;
    observation.source = source;
    observation.source_id = source_id;
    observation.source_ref = GetStr(j, "source_ref");
    observation.summary = GetStr(j, "summary");
    observation.fingerprint = GetStr(j, "fingerprint");
    observation.created_at = GetStr(j, "created_at");
    const std::string outcome_text = GetStr(j, "outcome");
    if (outcome_text == "success") {
        observation.outcome = ObservationOutcome::Success;
    } else if (outcome_text == "failure") {
        observation.outcome = ObservationOutcome::Failure;
    } else if (outcome_text == "partial") {
        observation.outcome = ObservationOutcome::Partial;
    } else {
        observation.outcome = ObservationOutcome::Unknown;
    }
    if (j.contains("details") && j.at("details").is_object()) {
        observation.details = j.at("details");
    }
    if (j.contains("evidence") && j.at("evidence").is_array()) {
        for (const auto& item : j.at("evidence")) {
            if (item.is_object()) {
                EvidenceRef ref;
                ref.ref = GetStr(item, "ref");
                ref.note = GetStr(item, "note");
                if (!ref.ref.empty()) {
                    observation.evidence.push_back(std::move(ref));
                }
            }
        }
    }
    return observation;
}

// ---------------------------------------------------------------------------
// 指纹
// ---------------------------------------------------------------------------

std::string NormalizeShapeText(const std::string& text) {
    if (text.empty()) return std::string();
    // 纵深一道:先打码再归一(指纹虽是哈希,归一化中间产物可能进日志)。
    const std::string redacted = skills::RedactSecrets(text);
    const std::string lowered = ToLowerAscii(redacted);

    std::string out;
    out.reserve(lowered.size());
    std::size_t i = 0;
    while (i < lowered.size()) {
        // 切词:空白分隔;中文没有空白,与英文连写一并当一段非空白处理——
        // 归一目标是"形状",不追求分词精确。
        while (i < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[i])) != 0) {
            ++i;
        }
        if (i >= lowered.size()) break;
        const std::size_t begin = i;
        while (i < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[i])) == 0) {
            ++i;
        }
        std::string word = lowered.substr(begin, i - begin);
        // 词尾常沾的标点剥掉再认形状(路径/日期后跟逗号这类)。
        std::string bare = word;
        while (!bare.empty() && (bare.back() == ',' || bare.back() == ';' || bare.back() == '.' ||
                                 bare.back() == ')' || bare.back() == ']' ||
                                 bare.back() == '"' || bare.back() == '\'')) {
            bare.pop_back();
        }
        std::string replacement;
        if (bare.rfind("http://", 0) == 0 || bare.rfind("https://", 0) == 0) {
            replacement = "<url>";
        } else if (LooksLikeAbsolutePath(bare)) {
            replacement = "<path>";
        } else if (LooksLikeDate(bare) || (bare.size() == 8 && AllDigits(bare))) {
            replacement = "<date>";
        }
        if (!out.empty()) out.push_back(' ');
        out += replacement.empty() ? word : replacement;
        if (out.size() >= 2000) break;  // 超长口述截断:形状在前 2000 字符里已经够认
    }
    return out;
}

std::string ComputeFingerprint(ObservationSource source, const std::string& shape_text) {
    const std::string canonical = "v1|" + ToString(source) + "|" + shape_text;
    const std::string sha = hooks::Sha256Hex(canonical);
    return "fp-" + sha.substr(0, 16);
}

std::string MakeObservationId(ObservationSource source, const std::string& source_id) {
    const std::string sha = hooks::Sha256Hex(ToString(source) + "|" + source_id);
    return "obs-" + sha.substr(0, 16);
}

// ---------------------------------------------------------------------------
// 脱敏窄口与时间
// ---------------------------------------------------------------------------

std::string SanitizeObservationText(const std::string& text, std::size_t cap) {
    std::string sanitized = skills::RedactSecrets(text);
    // 掐到完整 UTF-8 序列边界(不把多字节字符劈成半个)。
    if (sanitized.size() > cap) {
        std::size_t end = cap;
        while (end > 0 && (static_cast<unsigned char>(sanitized[end]) & 0xC0) == 0x80) {
            --end;
        }
        sanitized.resize(end);
    }
    return sanitized;
}

nlohmann::json SanitizeObservationJson(const nlohmann::json& value) {
    return skills::SanitizeToolInput(value);
}

std::string FormatEpochMsLocal(std::int64_t epoch_ms) {
    if (epoch_ms <= 0) return std::string();
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &seconds) != 0) return std::string();
#else
    if (localtime_r(&seconds, &parts) == nullptr) return std::string();
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
        return std::string();
    }
    return buffer;
}

}  // namespace lubancode::evolution
