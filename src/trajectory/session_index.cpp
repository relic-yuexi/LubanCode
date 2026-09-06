// session_index.hpp 的实现:可重建索引的扫场、指纹对账与查询。

#include "trajectory/session_index.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "trajectory/directory.hpp"
#include "workspace/index.hpp"  // 账本制:房门按 key 反查各房 manifest
#include "trajectory/safety.hpp"
#include "trajectory/session_manager.hpp"  // SessionStatusName(索引行状态)

namespace lubancode::trajectory {
namespace {

// 索引文件的 schema 标识(合同:派生物可整份丢弃重建,version 只升不降)。
constexpr const char* kIndexSchema = "lubancode.workspace.session-index";
constexpr int kIndexVersion = 1;
// 提问历史每 workspace 最多留多少行(新→旧截尾;Ctrl+R 一次也只看几百条)。
constexpr std::size_t kPromptHistoryCap = 2000;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    // 统一原子写(审计 P1):替掉本文件自备的固定 .tmp 协议。
    return platform::AtomicWriteFile(path, content).has_value();
}

std::string GetJsonString(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

std::int64_t GetJsonInt(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_integer()) {
        return 0;
    }
    return it->get<std::int64_t>();
}

// 一场 main.jsonl 单遍扫描的产物(摘要增量 + 提问行)。
struct SessionScan {
    WorkspaceSessionSummary summary;
    std::vector<PromptHistoryLine> prompts;
};

// payload.content 数组里第一段文本块(blocks 投影形状 {"type":"text","text"})。
std::string FirstTextOfContent(const nlohmann::json& content) {
    if (!content.is_array()) {
        return std::string();
    }
    for (const auto& block : content) {
        if (!block.is_object()) {
            continue;
        }
        if (GetJsonString(block, "type") == "text") {
            std::string text = GetJsonString(block, "text");
            if (!text.empty()) {
                return text;
            }
        }
    }
    return std::string();
}

SessionScan ScanSession(const std::filesystem::path& session_dir, const std::string& workspace_key) {
    SessionScan scan;
    WorkspaceSessionSummary& summary = scan.summary;
    summary.workspace_key = workspace_key;
    summary.session_dir = platform::PathToUtf8(session_dir);
    summary.session_id = platform::PathToUtf8(session_dir.filename());

    const auto manifest = ReadSessionJson(session_dir);
    if (!manifest.has_value()) {
        // session.json 读不动:目录占位/写坏。照列(可被 doctor 盯上),
        // 标 damaged,不给假摘要。
        summary.damaged = true;
        summary.status = "unknown";
        return scan;
    }
    summary.status = manifest->status;
    summary.archived = manifest->status == SessionStatusName(SessionStatus::Archived);
    // run_kind:manifest 钉的 main run 种类(单发轨迹断档单);旧档没这键
    // 保持默认 main_session。run.started 的信封同值,双源对得上。
    if (!manifest->run_kind.empty()) {
        summary.run_kind = manifest->run_kind;
    }
    summary.misplaced_v1 = manifest->schema_version < 2;
    summary.created_at_ms = manifest->created_at_ms;
    summary.cwd = manifest->launch_cwd;
    if (manifest->schema_version < 2) {
        summary.damaged = true;  // 新根下的 v1 = 旧档搬错家(合同 §三)
    }

    const std::filesystem::path main_path = session_dir / "main.jsonl";
    std::ifstream file(main_path, std::ios::binary);
    if (!file.is_open()) {
        // 没 main.jsonl:刚占位或只剩 manifest,按零事件列。
        summary.updated_at_ms = summary.created_at_ms;
        return scan;
    }
    std::string line;
    std::uint64_t prompt_seq = 0;
    bool saw_any = false;
    bool tail_broken = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const nlohmann::json event = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (event.is_discarded() || !event.is_object()) {
            // 坏行:整行跳过并标记。尾行截断是"不完整前缀"的常态,摘要
            // 照出(末事件用前一行),损坏如实报。
            tail_broken = true;
            continue;
        }
        saw_any = true;
        ++summary.event_count;
        const std::string kind = GetJsonString(event, "kind");
        const nlohmann::json empty = nlohmann::json::object();
        const nlohmann::json payload = event.contains("payload") && event["payload"].is_object()
                                           ? event["payload"]
                                           : empty;
        const std::int64_t wall = GetJsonInt(event, "wall_time_ms");
        if (wall > 0) {
            summary.updated_at_ms = wall;
        }
        if (kind == "input.received") {
            ++summary.message_count;
            const std::string text = FirstTextOfContent(payload.contains("content")
                                                             ? payload["content"]
                                                             : nlohmann::json::array());
            if (summary.first_user_text.empty() && !text.empty()) {
                summary.first_user_text = text;
            }
            if (!text.empty()) {
                ++prompt_seq;
                PromptHistoryLine prompt;
                prompt.workspace_key = workspace_key;
                prompt.session_id = summary.session_id;
                prompt.text = text;
                prompt.ts_ms = wall;
                prompt.seq = prompt_seq;
                scan.prompts.push_back(std::move(prompt));
            }
        } else if (kind == "model.output.completed") {
            ++summary.message_count;
        } else if (kind == "control.title.changed") {
            summary.title = GetJsonString(payload, "title");
        } else if (kind == "control.cwd.changed") {
            const std::string cwd = GetJsonString(payload, "cwd");
            if (!cwd.empty()) {
                summary.cwd = cwd;
            }
        } else if (kind == "model.request.prepared") {
            if (summary.model.empty()) {
                summary.model = GetJsonString(payload, "model");
            }
        } else if (kind == "run.started") {
            // 信封的 run_kind(main stream 的种类;manifest 同值,谁先见
            // 都一样——防 manifest 被手改后索引跟错)。
            const std::string stream_kind = GetJsonString(event, "run_kind");
            if (!stream_kind.empty()) {
                summary.run_kind = stream_kind;
            }
        }
    }
    if (tail_broken) {
        summary.damaged = true;
    }
    if (!saw_any) {
        summary.updated_at_ms = summary.created_at_ms;
    }
    return scan;
}

// 指纹:session.json 字节数 + mtime + main.jsonl 字节数。status 翻转
//(running→closed→archived)只动 session.json,字节数可能不变,故加 mtime。
struct SessionFingerprint {
    std::uintmax_t session_json_bytes = 0;
    std::int64_t session_json_mtime_ms = 0;
    std::uintmax_t main_bytes = 0;

    nlohmann::json ToJson() const {
        return nlohmann::json{{"session_json_bytes", session_json_bytes},
                              {"session_json_mtime_ms", session_json_mtime_ms},
                              {"main_bytes", main_bytes}};
    }
    static SessionFingerprint FromJson(const nlohmann::json& json) {
        SessionFingerprint fp;
        if (!json.is_object()) {
            return fp;
        }
        if (json.contains("session_json_bytes") && json["session_json_bytes"].is_number_unsigned()) {
            fp.session_json_bytes = json["session_json_bytes"].get<std::uintmax_t>();
        }
        if (json.contains("session_json_mtime_ms") && json["session_json_mtime_ms"].is_number_integer()) {
            fp.session_json_mtime_ms = json["session_json_mtime_ms"].get<std::int64_t>();
        }
        if (json.contains("main_bytes") && json["main_bytes"].is_number_unsigned()) {
            fp.main_bytes = json["main_bytes"].get<std::uintmax_t>();
        }
        return fp;
    }
};

std::int64_t MtimeMs(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

SessionFingerprint FingerprintOf(const std::filesystem::path& session_dir) {
    SessionFingerprint fp;
    std::error_code ec;
    if (const auto size = std::filesystem::file_size(session_dir / "session.json", ec); !ec) {
        fp.session_json_bytes = size;
        fp.session_json_mtime_ms = MtimeMs(session_dir / "session.json");
    } else {
        fp.session_json_bytes = 0;
        fp.session_json_mtime_ms = 0;
    }
    ec.clear();
    if (const auto size = std::filesystem::file_size(session_dir / "main.jsonl", ec); !ec) {
        fp.main_bytes = size;
    }
    return fp;
}

nlohmann::json SummaryToJson(const WorkspaceSessionSummary& summary, const SessionFingerprint& fp) {
    return nlohmann::json{{"workspace_key", summary.workspace_key},
                          {"session_id", summary.session_id},
                          {"status", summary.status},
                          {"archived", summary.archived},
                          {"run_kind", summary.run_kind},
                          {"title", summary.title},
                          {"first_user_text", summary.first_user_text},
                          {"cwd", summary.cwd},
                          {"model", summary.model},
                          {"created_at_ms", summary.created_at_ms},
                          {"updated_at_ms", summary.updated_at_ms},
                          {"message_count", summary.message_count},
                          {"event_count", summary.event_count},
                          {"damaged", summary.damaged},
                          {"misplaced_v1", summary.misplaced_v1},
                          {"session_dir", summary.session_dir},
                          {"fp", fp.ToJson()}};
}

WorkspaceSessionSummary SummaryFromJson(const nlohmann::json& json) {
    WorkspaceSessionSummary summary;
    summary.workspace_key = GetJsonString(json, "workspace_key");
    summary.session_id = GetJsonString(json, "session_id");
    summary.status = GetJsonString(json, "status");
    summary.archived = json.contains("archived") && json["archived"].is_boolean() &&
                       json["archived"].get<bool>();
    summary.title = GetJsonString(json, "title");
    summary.first_user_text = GetJsonString(json, "first_user_text");
    summary.cwd = GetJsonString(json, "cwd");
    summary.model = GetJsonString(json, "model");
    // run_kind:旧索引行缺键回落 main_session(单发轨迹断档单;旧场没有
    // one_shot,回落无害)。
    summary.run_kind = GetJsonString(json, "run_kind");
    if (summary.run_kind.empty()) {
        summary.run_kind = "main_session";
    }
    summary.created_at_ms = GetJsonInt(json, "created_at_ms");
    summary.updated_at_ms = GetJsonInt(json, "updated_at_ms");
    if (json.contains("message_count") && json["message_count"].is_number_unsigned()) {
        summary.message_count = json["message_count"].get<std::uint64_t>();
    }
    if (json.contains("event_count") && json["event_count"].is_number_unsigned()) {
        summary.event_count = json["event_count"].get<std::uint64_t>();
    }
    summary.damaged = json.contains("damaged") && json["damaged"].is_boolean() &&
                      json["damaged"].get<bool>();
    summary.misplaced_v1 = json.contains("misplaced_v1") && json["misplaced_v1"].is_boolean() &&
                           json["misplaced_v1"].get<bool>();
    summary.session_dir = GetJsonString(json, "session_dir");
    return summary;
}

nlohmann::json PromptToJson(const PromptHistoryLine& prompt) {
    return nlohmann::json{{"session_id", prompt.session_id},
                          {"text", prompt.text},
                          {"ts_ms", prompt.ts_ms},
                          {"seq", prompt.seq}};
}

PromptHistoryLine PromptFromJson(const nlohmann::json& json, const std::string& workspace_key,
                                 const std::string& title) {
    PromptHistoryLine prompt;
    prompt.workspace_key = workspace_key;
    prompt.session_id = GetJsonString(json, "session_id");
    prompt.title = title;
    prompt.text = GetJsonString(json, "text");
    prompt.ts_ms = GetJsonInt(json, "ts_ms");
    if (json.contains("seq") && json["seq"].is_number_unsigned()) {
        prompt.seq = json["seq"].get<std::uint64_t>();
    }
    return prompt;
}

// 一份 workspace 索引(读盘或重建后的内存态)。
struct WorkspaceIndex {
    std::vector<WorkspaceSessionSummary> sessions;
    std::vector<PromptHistoryLine> prompts;  // 场内时序;未截尾
};

// 核心:读 <ws>/indexes/sessions.json,对指纹,动了的重扫,变了就写回。
WorkspaceIndex LoadOrRebuildIndex(const std::filesystem::path& workspace_dir,
                                  const std::string& workspace_key) {
    WorkspaceIndex index;
    const std::filesystem::path sessions_dir = workspace_dir / "sessions";
    const std::filesystem::path index_path = workspace_dir / "indexes" / "sessions.json";
    std::error_code ec;
    if (!std::filesystem::exists(sessions_dir, ec)) {
        return index;
    }

    // 旧账:行按 session_id 索引,提问行按 session 分桶。
    std::map<std::string, WorkspaceSessionSummary> old_rows;
    std::map<std::string, SessionFingerprint> old_fps;
    std::map<std::string, std::vector<PromptHistoryLine>> old_prompts;
    std::string old_title_lookup_session;  // no-op 占位,标题在行里查
    (void)old_title_lookup_session;
    {
        std::ifstream file(index_path, std::ios::binary);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            const nlohmann::json json =
                nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
            if (json.is_object() && GetJsonString(json, "schema") == kIndexSchema &&
                json.contains("version") && json["version"].is_number_integer() &&
                json["version"].get<int>() <= kIndexVersion && json.contains("sessions") &&
                json["sessions"].is_array()) {
                std::vector<WorkspaceSessionSummary> rows;
                for (const auto& row : json["sessions"]) {
                    if (!row.is_object()) {
                        continue;
                    }
                    rows.push_back(SummaryFromJson(row));
                    if (row.contains("fp")) {
                        old_fps[GetJsonString(row, "session_id")] =
                            SessionFingerprint::FromJson(row["fp"]);
                    }
                }
                for (const auto& row : rows) {
                    old_rows[row.session_id] = row;
                }
                if (json.contains("prompts") && json["prompts"].is_array()) {
                    for (const auto& prompt : json["prompts"]) {
                        if (!prompt.is_object()) {
                            continue;
                        }
                        const std::string session_id = GetJsonString(prompt, "session_id");
                        old_prompts[session_id].push_back(
                            PromptFromJson(prompt, workspace_key, std::string()));
                    }
                }
            }
        }
    }

    bool changed = false;
    std::set<std::string> seen;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path session_dir = entry.path();
        const std::string session_id = platform::PathToUtf8(session_dir.filename());
        seen.insert(session_id);
        const SessionFingerprint fp = FingerprintOf(session_dir);
        const auto old_fp = old_fps.find(session_id);
        const auto old_row = old_rows.find(session_id);
        if (old_fp != old_fps.end() && old_row != old_rows.end() &&
            old_fp->second.session_json_bytes == fp.session_json_bytes &&
            old_fp->second.session_json_mtime_ms == fp.session_json_mtime_ms &&
            old_fp->second.main_bytes == fp.main_bytes) {
            // 指纹没动:旧摘要照用(标题随提问行一起回填)。
            index.sessions.push_back(old_row->second);
            const auto prompts = old_prompts.find(session_id);
            if (prompts != old_prompts.end()) {
                for (const auto& prompt : prompts->second) {
                    PromptHistoryLine filled = prompt;
                    filled.title = old_row->second.title;
                    index.prompts.push_back(std::move(filled));
                }
            }
            continue;
        }
        changed = true;
        SessionScan scan = ScanSession(session_dir, workspace_key);
        index.sessions.push_back(scan.summary);
        for (auto& prompt : scan.prompts) {
            prompt.title = scan.summary.title;
        }
        std::move(scan.prompts.begin(), scan.prompts.end(), std::back_inserter(index.prompts));
    }
    // 索引里有、盘上没了的场次(已删/已搬):整行整桶弃掉。
    for (const auto& [session_id, row] : old_rows) {
        if (seen.count(session_id) == 0) {
            changed = true;
        }
    }

    if (changed) {
        std::sort(index.sessions.begin(), index.sessions.end(),
                  [](const WorkspaceSessionSummary& a, const WorkspaceSessionSummary& b) {
                      if (a.updated_at_ms != b.updated_at_ms) {
                          return a.updated_at_ms > b.updated_at_ms;
                      }
                      return a.session_id > b.session_id;
                  });
        // 写回的提问行按时间升序(读侧再倒),截最近的 kPromptHistoryCap 条。
        std::sort(index.prompts.begin(), index.prompts.end(),
                  [](const PromptHistoryLine& a, const PromptHistoryLine& b) {
                      if (a.ts_ms != b.ts_ms) {
                          return a.ts_ms < b.ts_ms;
                      }
                      return a.seq < b.seq;
                  });
        if (index.prompts.size() > kPromptHistoryCap) {
            index.prompts.erase(index.prompts.begin(),
                                index.prompts.end() -
                                    static_cast<std::ptrdiff_t>(kPromptHistoryCap));
        }
        nlohmann::json rows = nlohmann::json::array();
        // 指纹随行写回(查询期算好的那份;没动的行也重写一次,保持文件自洽)。
        for (const auto& summary : index.sessions) {
            rows.push_back(SummaryToJson(summary, FingerprintOf(
                                                      workspace_dir / "sessions" /
                                                      platform::Utf8ToPath(summary.session_id))));
        }
        nlohmann::json prompts_json = nlohmann::json::array();
        for (const auto& prompt : index.prompts) {
            prompts_json.push_back(PromptToJson(prompt));
        }
        const nlohmann::json out = nlohmann::json{{"schema", kIndexSchema},
                                                  {"version", kIndexVersion},
                                                  {"updated_at_ms", NowMs()},
                                                  {"sessions", std::move(rows)},
                                                  {"prompts", std::move(prompts_json)}};
        std::error_code mkdir_ec;
        std::filesystem::create_directories(workspace_dir / "indexes", mkdir_ec);
        if (!mkdir_ec) {
            WriteTextFileAtomic(index_path, out.dump());
        }
        // 写不回不算失败:索引是派生物,下次查询再试。
    } else {
        std::sort(index.sessions.begin(), index.sessions.end(),
                  [](const WorkspaceSessionSummary& a, const WorkspaceSessionSummary& b) {
                      if (a.updated_at_ms != b.updated_at_ms) {
                          return a.updated_at_ms > b.updated_at_ms;
                      }
                      return a.session_id > b.session_id;
                  });
        std::sort(index.prompts.begin(), index.prompts.end(),
                  [](const PromptHistoryLine& a, const PromptHistoryLine& b) {
                      if (a.ts_ms != b.ts_ms) {
                          return a.ts_ms < b.ts_ms;
                      }
                      return a.seq < b.seq;
                  });
    }
    return index;
}

std::vector<std::string> ListWorkspaceKeys(const std::filesystem::path& workspaces_root) {
    // 账本制:目录名是门牌不是 key——逐房读 workspace.json 取真钥匙,
    // 门牌与钥匙从此两清。坏房/外来目录不进列表。
    std::vector<std::string> keys;
    for (const workspace::index::WorkspaceRoom& room : workspace::index::ScanRooms(workspaces_root)) {
        const std::string& key = room.manifest.workspace_key;
        if (!IsSafeSingleSegment(key)) {
            continue;  // 非法段名不进列表(§12.1)
        }
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

}  // namespace

SessionIndexPage QueryWorkspaceSessions(const std::filesystem::path& workspaces_root,
                                        const SessionIndexQuery& query) {
    SessionIndexPage page;
    std::vector<std::string> keys;
    if (query.all_workspaces) {
        keys = ListWorkspaceKeys(workspaces_root);
    } else {
        if (query.current_workspace_key.empty() || !IsSafeSingleSegment(query.current_workspace_key)) {
            return page;
        }
        keys.push_back(query.current_workspace_key);
    }
    std::vector<WorkspaceSessionSummary> all;
    for (const std::string& key : keys) {
        // 账本制:key → 房门走 manifest 反查(目录名是门牌)。反查不到的
        // key(房被手删)跳过,不冒充。
        const auto room = workspace::index::ResolveDirByWorkspaceKey(workspaces_root, key);
        if (!room.has_value()) {
            continue;
        }
        const WorkspaceIndex index = LoadOrRebuildIndex(*room, key);
        all.reserve(all.size() + index.sessions.size());
        for (const auto& summary : index.sessions) {
            if (query.archived_only) {
                if (!summary.archived) {
                    continue;
                }
            } else if (!query.include_archived && summary.archived) {
                continue;
            }
            if (query.exclude_one_shot && summary.run_kind == "one_shot") {
                continue;  // resume 选择器不列单发场(单发语义不续)
            }
            if (!query.search.empty()) {
                const std::string needle = ToLowerAscii(query.search);
                const std::string haystack =
                    ToLowerAscii(summary.title + "\n" + summary.first_user_text + "\n" +
                                 summary.session_id);
                if (haystack.find(needle) == std::string::npos) {
                    continue;
                }
            }
            all.push_back(summary);
        }
    }
    const bool by_created = query.sort_by_created;
    std::sort(all.begin(), all.end(),
              [by_created](const WorkspaceSessionSummary& a, const WorkspaceSessionSummary& b) {
                  const std::int64_t left = by_created ? a.created_at_ms : a.updated_at_ms;
                  const std::int64_t right = by_created ? b.created_at_ms : b.updated_at_ms;
                  if (left != right) {
                      return left > right;
                  }
                  return a.session_id > b.session_id;
              });
    page.total = all.size();
    const std::size_t skip = query.cursor < all.size() ? query.cursor : all.size();
    std::size_t count = all.size() - skip;
    if (query.limit > 0 && query.limit < count) {
        count = query.limit;
    }
    page.entries.assign(all.begin() + static_cast<std::ptrdiff_t>(skip),
                        all.begin() + static_cast<std::ptrdiff_t>(skip + count));
    return page;
}

std::vector<PromptHistoryLine> ReadWorkspacePromptHistory(const std::filesystem::path& workspaces_root,
                                                          const std::string& workspace_key,
                                                          std::size_t max_lines) {
    if (workspace_key.empty() || !IsSafeSingleSegment(workspace_key)) {
        return {};
    }
    const auto room = workspace::index::ResolveDirByWorkspaceKey(workspaces_root, workspace_key);
    if (!room.has_value()) {
        return {};
    }
    const WorkspaceIndex index = LoadOrRebuildIndex(*room, workspace_key);
    std::vector<PromptHistoryLine> lines = index.prompts;
    // 新→新在场在后;消费侧要"最新优先"就倒着走,这里保持时间升序返回,
    // 与旧 ExtractPromptHistory 的口径一致(旧→新)。
    if (max_lines > 0 && lines.size() > max_lines) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(max_lines));
    }
    return lines;
}

std::vector<std::string> MakeSessionTranscriptExcerpt(const std::filesystem::path& session_dir,
                                                      std::size_t max_half) {
    std::vector<std::string> lines;
    std::ifstream file(session_dir / "main.jsonl", std::ios::binary);
    if (!file.is_open()) {
        return lines;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const nlohmann::json event = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (event.is_discarded() || !event.is_object()) {
            continue;
        }
        const std::string kind = GetJsonString(event, "kind");
        std::string text;
        const char* who = nullptr;
        if (kind == "input.received") {
            who = "user";
            const nlohmann::json empty = nlohmann::json::object();
            const nlohmann::json payload = event.contains("payload") && event["payload"].is_object()
                                               ? event["payload"]
                                               : empty;
            text = FirstTextOfContent(payload.contains("content") ? payload["content"]
                                                                  : nlohmann::json::array());
        } else if (kind == "model.output.completed") {
            who = "assistant";
            const nlohmann::json empty = nlohmann::json::object();
            const nlohmann::json payload = event.contains("payload") && event["payload"].is_object()
                                               ? event["payload"]
                                               : empty;
            if (payload.contains("blocks") && payload["blocks"].is_array()) {
                text = FirstTextOfContent(payload["blocks"]);
            }
        } else {
            continue;  // 控制事件不进转录
        }
        if (text.empty()) {
            continue;
        }
        const std::size_t newline = text.find('\n');
        if (newline != std::string::npos) {
            text = text.substr(0, newline);
        }
        lines.push_back(std::string("  ") + who + " · " + text);
    }
    if (max_half > 0 && lines.size() > max_half * 2) {
        std::vector<std::string> trimmed;
        trimmed.reserve(max_half * 2 + 1);
        trimmed.insert(trimmed.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(max_half));
        trimmed.push_back(std::string("  …(") + std::to_string(lines.size() - max_half * 2) + " 行省略)…");
        trimmed.insert(trimmed.end(), lines.end() - static_cast<std::ptrdiff_t>(max_half), lines.end());
        return trimmed;
    }
    return lines;
}

SessionToolTraceFold FoldSessionToolExecutions(const std::filesystem::path& session_dir) {
    SessionToolTraceFold fold;
    // call_id -> 行(组装中;planned 先建行,terminal/result 补尾)。
    std::vector<nlohmann::json> rows;
    std::map<std::string, std::size_t> row_of;
    std::ifstream file(session_dir / "main.jsonl", std::ios::binary);
    if (!file.is_open()) {
        return fold;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const nlohmann::json event = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (event.is_discarded() || !event.is_object()) {
            continue;
        }
        const std::string kind = GetJsonString(event, "kind");
        const std::string call_id = GetJsonString(event, "call_id");
        const nlohmann::json empty = nlohmann::json::object();
        const nlohmann::json payload =
            event.contains("payload") && event["payload"].is_object() ? event["payload"] : empty;
        std::uint64_t seq = 0;
        if (event.contains("seq") && event["seq"].is_number_unsigned()) {
            seq = event["seq"].get<std::uint64_t>();
        }
        if (seq > fold.max_seq) {
            fold.max_seq = seq;
        }
        const auto row_index_of = [&row_of, &rows, seq](const std::string& id) -> std::size_t {
            const auto it = row_of.find(id);
            if (it != row_of.end()) {
                return it->second;
            }
            nlohmann::json row = nlohmann::json{{"executionId", id},
                                                {"toolUseId", id},
                                                {"seqScheduled", seq},
                                                {"outcome", ""},
                                                {"durationMs", std::uint64_t{0}}};
            rows.push_back(std::move(row));
            const std::size_t index = rows.size() - 1;
            row_of[id] = index;
            return index;
        };
        if (kind == "tool.execution.planned") {
            const std::size_t index = row_index_of(call_id);
            rows[index]["toolName"] = GetJsonString(payload, "tool_name");
        } else if (kind == "tool.input.effective") {
            const std::size_t index = row_index_of(call_id);
            if (!rows[index].contains("toolName")) {
                rows[index]["toolName"] = GetJsonString(payload, "tool_name");
            }
            rows[index]["source"] = GetJsonString(payload, "source_kind");
            const std::string instance = GetJsonString(payload, "source_instance");
            if (!instance.empty()) {
                rows[index]["sourceInstance"] = instance;
            }
        } else if (kind == "tool.execution.started") {
            const std::size_t index = row_index_of(call_id);
            const std::string batch = GetJsonString(payload, "batch_id");
            if (!batch.empty()) {
                rows[index]["batchId"] = batch;
                if (payload.contains("position_in_batch") &&
                    payload["position_in_batch"].is_number_unsigned()) {
                    rows[index]["sequenceInBatch"] = payload["position_in_batch"];
                }
            }
            if (event.contains("turn_id") && event["turn_id"].is_string()) {
                rows[index]["turnId"] = event["turn_id"].get<std::string>();
            }
        } else if (kind == "tool.execution.finished" || kind == "tool.execution.failed" ||
                   kind == "tool.execution.cancelled" || kind == "tool.execution.unknown") {
            const std::size_t index = row_index_of(call_id);
            rows[index]["outcome"] = kind == "tool.execution.finished"
                                         ? std::string("succeeded")
                                         : (kind == "tool.execution.cancelled"
                                                ? std::string("cancelled")
                                                : (kind == "tool.execution.unknown"
                                                       ? std::string("unknown_after_start")
                                                       : std::string("failed")));
            const std::string error_code = GetJsonString(payload, "error_code");
            if (!error_code.empty()) {
                rows[index]["errorCode"] = error_code;
            } else if (kind != "tool.execution.finished") {
                const std::string reason = GetJsonString(payload, "reason");
                if (!reason.empty()) {
                    rows[index]["errorCode"] = reason;
                }
            }
            if (payload.contains("duration_ms") && payload["duration_ms"].is_number_unsigned()) {
                rows[index]["durationMs"] = payload["duration_ms"];
            }
            if (payload.contains("result_ref") && payload["result_ref"].is_object()) {
                const nlohmann::json& ref = payload["result_ref"];
                if (ref.contains("bytes") && ref["bytes"].is_number_unsigned()) {
                    rows[index]["resultBytes"] = ref["bytes"];
                }
                const std::string sha = GetJsonString(ref, "sha256");
                if (!sha.empty()) {
                    rows[index]["resultSha256"] = sha;
                    rows[index]["resultArtifactId"] = "art-" + sha.substr(0, 8);
                }
                const std::string ref_kind = GetJsonString(ref, "kind");
                if (!ref_kind.empty()) {
                    rows[index]["resultRefKind"] = ref_kind;
                }
            }
        } else if (kind == "tool.result.committed") {
            const std::size_t index = row_index_of(call_id);
            // 遮敏默认:preview 只取首段文本块的前 160 字,不回 inline 原文。
            if (payload.contains("content") && payload["content"].is_array()) {
                const std::string text = FirstTextOfContent(payload["content"]);
                if (!text.empty()) {
                    std::string preview = text.substr(0, 160);
                    rows[index]["resultPreview"] = std::move(preview);
                }
            }
        }
    }
    fold.executions = std::move(rows);
    return fold;
}

std::string FormatMillisAsLocalTimestamp(std::int64_t ms) {
    if (ms <= 0) {
        return std::string();
    }
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm parts{};
#ifdef _WIN32
    if (localtime_s(&parts, &seconds) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&seconds, &parts) == nullptr) {
        return std::string();
    }
#endif
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return std::string(buffer);
}

}  // namespace lubancode::trajectory
