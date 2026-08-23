// Run Journal 实现(自然语言编排单第 2 批)。
//
// 落盘次序钉死:节点 output 先写临时件、算 hash、原子 rename,再追加
// node_completed——恢复不会捡到半份结果(checkpoint 走同一次序)。事件
// 行 append+flush,崩溃截断的半截尾行由 ParseJournalEvent 跳过。

#include "workflow/journal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/json_safe.hpp"
#include "platform/paths.hpp"

namespace lubancode::workflow {

namespace {

std::string NowIsoLike() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buffer;
}

bool IsSecretKey(std::string key) {
    for (char& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-') c = '_';
    }
    const char* kSecretWords[] = {"token", "secret", "password", "passwd", "authorization", "cookie",
                                  "api_key", "apikey", "private_key"};
    for (const char* word : kSecretWords) {
        if (key.find(word) != std::string::npos) return true;
    }
    return false;
}

std::string GetStr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

}  // namespace

// ---- 时钟 -------------------------------------------------------------------

std::int64_t JournalClock::NowMs() const {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// ---- 脱敏(纯函数) ----------------------------------------------------------

nlohmann::json SanitizeJournalPayload(const nlohmann::json& payload) {
    if (payload.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            if (it.value().is_string() && IsSecretKey(it.key())) {
                out[it.key()] = "[已打码]";
            } else {
                out[it.key()] = SanitizeJournalPayload(it.value());
            }
        }
        return out;
    }
    if (payload.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : payload) out.push_back(SanitizeJournalPayload(item));
        return out;
    }
    if (payload.is_string()) {
        return RedactJournalText(payload.get<std::string>());
    }
    return payload;
}

std::string RedactJournalText(const std::string& text) {
    // bearer/sk- 起头的词整体打码;"key: value"/"key=value" 里键名敏感的
    // 打值。与 recorder 的 RedactSecrets 同规矩,这边独立维护(不 include
    // agent/*,依赖单向),单测钉共同样本。
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // 找最近的 "key" 赋值形态。
        const std::size_t colon = text.find(':', i);
        const std::size_t eq = text.find('=', i);
        const std::size_t mark = (colon == std::string::npos) ? eq
                             : (eq == std::string::npos) ? colon
                             : std::min(colon, eq);
        if (mark == std::string::npos) {
            out += text.substr(i);
            break;
        }
        // key 是 mark 之前最近的非空白游程。
        std::size_t key_end = mark;
        while (key_end > i && (text[key_end - 1] == ' ' || text[key_end - 1] == '\t')) --key_end;
        std::size_t key_start = key_end;
        while (key_start > i) {
            const char c = text[key_start - 1];
            const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '_' || c == '-';
            if (!word) break;
            --key_start;
        }
        std::string key = text.substr(key_start, key_end - key_start);
        std::string lower;
        for (const char c : key) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const char* kWords[] = {"token", "secret", "password", "passwd", "authorization", "cookie", "api_key",
                                "apikey"};
        bool sensitive = false;
        for (const char* w : kWords) {
            if (lower.find(w) != std::string::npos) sensitive = true;
        }
        out += text.substr(i, key_start - i);
        out += key;
        out += text.substr(key_start + key.size(), mark + 1 - (key_start + key.size()));
        if (sensitive) {
            // 值到行尾/空白停。
            std::size_t value_end = mark + 1;
            while (value_end < text.size() && text[value_end] != '\n' && text[value_end] != ' ' &&
                   text[value_end] != '\t' && text[value_end] != ';') {
                ++value_end;
            }
            out += "[已打码]";
            i = value_end;
        } else {
            i = mark + 1;
        }
    }
    return out;
}

// ---- 事件序列化 -------------------------------------------------------------

std::string SerializeJournalEvent(const JournalEvent& event) {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["seq"] = event.seq;
    j["ts"] = event.ts_ms;
    j["run_id"] = event.run_id;
    j["workflow_id"] = event.workflow_id;
    j["node_id"] = event.node_id;
    j["attempt"] = event.attempt;
    j["type"] = event.type;
    j["data"] = SanitizeJournalPayload(event.data);
    return platform::DumpJsonSanitized(j);
}

std::optional<JournalEvent> ParseJournalEvent(const std::string& line) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return std::nullopt;
    }
    if (!j.is_object()) return std::nullopt;
    JournalEvent event;
    const auto seq = j.find("seq");
    const auto ts = j.find("ts");
    const auto type = j.find("type");
    if (seq == j.end() || !seq->is_number_unsigned()) return std::nullopt;
    if (ts == j.end() || !ts->is_number()) return std::nullopt;
    if (type == j.end() || !type->is_string()) return std::nullopt;
    event.seq = seq->get<std::uint64_t>();
    event.ts_ms = ts->get<std::int64_t>();
    event.run_id = GetStr(j, "run_id");
    event.workflow_id = GetStr(j, "workflow_id");
    event.node_id = GetStr(j, "node_id");
    if (const auto attempt = j.find("attempt"); attempt != j.end() && attempt->is_number_integer()) {
        event.attempt = attempt->get<int>();
    }
    event.type = type->get<std::string>();
    if (const auto data = j.find("data"); data != j.end() && data->is_object()) {
        event.data = *data;
    }
    return event;
}

// ---- RunJournal -------------------------------------------------------------

RunJournal::RunJournal(std::filesystem::path dir, std::string run_id, std::ofstream out, const JournalClock* clock,
                       nlohmann::json start_manifest)
    : dir_(std::move(dir)),
      run_id_(std::move(run_id)),
      out_(std::move(out)),
      clock_(clock),
      start_manifest_(std::move(start_manifest)) {}

RunJournal::RunJournal(RunJournal&& other) noexcept
    : dir_(std::move(other.dir_)),
      run_id_(std::move(other.run_id_)),
      out_(std::move(other.out_)),
      clock_(other.clock_),
      start_manifest_(std::move(other.start_manifest_)),
      next_seq_(other.next_seq_),
      broken_(other.broken_),
      finish_called_(other.finish_called_) {
    other.finish_called_ = true;  // 析构不重复 Finish
}

RunJournal& RunJournal::operator=(RunJournal&& other) noexcept {
    if (this != &other) {
        dir_ = std::move(other.dir_);
        run_id_ = std::move(other.run_id_);
        out_ = std::move(other.out_);
        clock_ = other.clock_;
        start_manifest_ = std::move(other.start_manifest_);
        next_seq_ = other.next_seq_;
        broken_ = other.broken_;
        finish_called_ = other.finish_called_;
        other.finish_called_ = true;
    }
    return *this;
}

RunJournal::~RunJournal() {
    if (!finish_called_ && !dir_.empty() && !run_id_.empty() && !broken_) {
        Finish("interrupted", nlohmann::json::object());
    } else if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

std::expected<RunJournal, std::string> RunJournal::Start(const std::filesystem::path& runs_root,
                                                         const StartInfo& info, const JournalClock* clock) {
    std::error_code ec;
    const std::filesystem::path dir = runs_root / info.run_id;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("run 目录建不成: " + lubancode::platform::PathToUtf8(dir) + ": " + ec.message());
    }
    // definition 快照(原子):先临时件再 rename。
    const std::filesystem::path def_tmp = dir / ".definition.json.tmp";
    const std::filesystem::path def_path = dir / "definition.json";
    {
        std::ofstream def_file(def_tmp, std::ios::binary | std::ios::trunc);
        if (!def_file) {
            return std::unexpected("definition 快照写不开: " + lubancode::platform::PathToUtf8(def_tmp));
        }
        def_file << info.definition_json;
    }
    std::filesystem::rename(def_tmp, def_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(def_tmp, remove_ec);
        return std::unexpected("definition 快照换名不成: " + ec.message());
    }

    nlohmann::json manifest = info.ToManifestJson();
    manifest["started_at"] = NowIsoLike();
    RunJournal journal(dir, info.run_id, std::ofstream(), clock, std::move(manifest));
    journal.WriteManifest(std::string(), nlohmann::json::object());

    std::ofstream events(dir / "events.jsonl", std::ios::binary | std::ios::app);
    if (!events) {
        return std::unexpected("events.jsonl 打不开: " + lubancode::platform::PathToUtf8(dir / "events.jsonl"));
    }
    journal.out_ = std::move(events);

    JournalEvent started;
    started.type = kEventRunStarted;
    started.data = nlohmann::json{{"workflow_version", info.workflow_version},
                                  {"content_hash", info.content_hash},
                                  {"cwd", info.cwd}};
    journal.Append(started.type, started.node_id, 0, started.data);
    return journal;
}

void RunJournal::Append(const std::string& type, const std::string& node_id, int attempt, nlohmann::json data) {
    if (broken_ || !out_.is_open()) return;
    JournalEvent event;
    event.seq = next_seq_++;
    event.ts_ms = clock_ != nullptr ? clock_->NowMs() : JournalClock().NowMs();
    event.run_id = run_id_;
    event.node_id = node_id;
    event.attempt = attempt;
    event.type = type;
    event.data = std::move(data);
    out_ << SerializeJournalEvent(event) << "\n";
    out_.flush();
    if (!out_) {
        broken_ = true;
        std::cerr << "[workflow-journal] 事件落盘失败,后续事件不再写盘(run " << run_id_ << ")\n";
    }
}

void RunJournal::SaveCheckpoint(std::uint64_t at_seq, const nlohmann::json& store_json) {
    if (broken_ || dir_.empty()) return;
    std::error_code ec;
    const std::filesystem::path cp_dir = dir_ / "checkpoints";
    std::filesystem::create_directories(cp_dir, ec);
    const std::filesystem::path tmp = cp_dir / (".cp.tmp");
    const std::filesystem::path final_path = cp_dir / (std::to_string(at_seq) + ".json");
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "[workflow-journal] checkpoint 写不开(" << lubancode::platform::PathToUtf8(tmp) << ")\n";
            return;
        }
        file << SanitizeJournalPayload(store_json).dump();
    }
    std::filesystem::rename(tmp, final_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        std::cerr << "[workflow-journal] checkpoint 换名不成: " << ec.message() << "\n";
        return;
    }
    Append(kEventCheckpointSaved, std::string(), 0, nlohmann::json{{"seq", at_seq}});
}

void RunJournal::Finish(const std::string& final_state, const nlohmann::json& summary) {
    if (finish_called_) return;
    finish_called_ = true;
    Append(kEventRunCompleted, std::string(), 0,
           nlohmann::json{{"state", final_state}, {"summary", SanitizeJournalPayload(summary)}});
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    WriteManifest(final_state, summary);
}

void RunJournal::WriteManifest(const std::string& final_state, const nlohmann::json& summary) {
    // Start 落基础字段(start_manifest_ 存着),Finish 补终态,同一套原子写。
    std::error_code ec;
    nlohmann::json manifest =
        start_manifest_.is_object() ? start_manifest_ : nlohmann::json::object();
    if (!final_state.empty()) {
        manifest["final_state"] = final_state;
        manifest["finished_at"] = NowIsoLike();
        if (summary.is_object()) manifest["summary"] = SanitizeJournalPayload(summary);
    }
    const std::filesystem::path tmp = dir_ / ".manifest.json.tmp";
    const std::filesystem::path path = dir_ / "manifest.json";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << manifest.dump(2);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
    }
}

// ---- 盘点与恢复 -------------------------------------------------------------

std::vector<RunStatus> ListRuns(const std::filesystem::path& runs_root) {
    std::vector<RunStatus> out;
    std::error_code ec;
    if (!std::filesystem::exists(runs_root, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(runs_root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }
        ec.clear();
        RunStatus status;
        status.run_id = lubancode::platform::PathToUtf8(entry.path().filename());
        status.dir = entry.path();
        // manifest。
        std::ifstream manifest(entry.path() / "manifest.json", std::ios::binary);
        if (manifest) {
            try {
                const nlohmann::json j = nlohmann::json::parse(manifest);
                status.workflow_id = GetStr(j, "workflow_id");
                status.workflow_version = GetStr(j, "workflow_version");
                status.content_hash = GetStr(j, "content_hash");
                status.cwd = GetStr(j, "cwd");
                status.started_at = GetStr(j, "started_at");
                status.final_state = GetStr(j, "final_state");
            } catch (...) {
            }
        }
        // definition 快照。
        std::ifstream def(entry.path() / "definition.json", std::ios::binary);
        if (def) {
            try {
                status.definition = nlohmann::json::parse(def);
            } catch (...) {
            }
        }
        out.push_back(std::move(status));
    }
    std::sort(out.begin(), out.end(),
              [](const RunStatus& a, const RunStatus& b) { return a.run_id > b.run_id; });
    return out;
}

std::vector<JournalEvent> ReadJournalEvents(const std::filesystem::path& run_dir) {
    std::vector<JournalEvent> out;
    std::ifstream file(run_dir / "events.jsonl", std::ios::binary);
    if (!file) return out;
    std::string line;
    while (std::getline(file, line)) {
        if (auto event = ParseJournalEvent(line)) {
            out.push_back(std::move(*event));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const JournalEvent& a, const JournalEvent& b) { return a.seq < b.seq; });
    return out;
}

std::optional<nlohmann::json> ReadLatestCheckpoint(const std::filesystem::path& run_dir) {
    std::error_code ec;
    const std::filesystem::path cp_dir = run_dir / "checkpoints";
    if (!std::filesystem::exists(cp_dir, ec)) return std::nullopt;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(cp_dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
        ec.clear();
    }
    if (files.empty()) return std::nullopt;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        const std::string an = lubancode::platform::PathToUtf8(a.filename());
        const std::string bn = lubancode::platform::PathToUtf8(b.filename());
        std::uint64_t ai = 0;
        std::uint64_t bi = 0;
        try {
            ai = std::stoull(an.substr(0, an.find('.')));
        } catch (...) {
        }
        try {
            bi = std::stoull(bn.substr(0, bn.find('.')));
        } catch (...) {
        }
        return ai < bi;
    });
    std::ifstream file(files.back(), std::ios::binary);
    if (!file) return std::nullopt;
    try {
        return nlohmann::json::parse(file);
    } catch (...) {
        return std::nullopt;
    }
}

std::map<std::string, ReplayedNode> ReplayNodes(const std::vector<JournalEvent>& events) {
    std::map<std::string, ReplayedNode> out;
    for (const auto& event : events) {
        if (event.node_id.empty()) continue;
        ReplayedNode& node = out[event.node_id];
        if (event.type == kEventNodeCompleted) {
            node.state = "succeeded";
            if (event.data.contains("output")) node.output = event.data["output"];
        } else if (event.type == kEventNodeSkipped) {
            node.state = "skipped";
            if (event.data.contains("reason")) node.meta["reason"] = event.data["reason"];
        } else if (event.type == kEventNodeStarted) {
            if (node.state.empty()) node.state = "running";
        }
    }
    return out;
}

}  // namespace lubancode::workflow
