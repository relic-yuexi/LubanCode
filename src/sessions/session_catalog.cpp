// session_catalog.hpp 的实现:摘要/纯函数/带指纹缓存的台账薄壳。

#include "sessions/session_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "sessions/session_store.hpp"

namespace lubancode::sessions {

namespace {

// UTF-8 字符串路径 -> std::filesystem::path(Windows 下走 u8string 那条路,
// 与 session_store.cpp 同款;这里不引它是因为那枚是匿名 namespace 私货)。
std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// ASCII 折小写(中文等多字节按字节比,UTF-8 后续字节不带大小写语义)。
std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 一行账里抽顶层字符串字段("ts"/"title"/"cwd" 这类),字段不是字符串给
// 空串。行已验过是合法 JSON 对象才进来。
std::string TopLevelStringField(const nlohmann::json& j, const char* field) {
    const auto it = j.find(field);
    if (it == j.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

std::uint64_t SessionTimeSortKey(const std::string& ts) {
    // "yyyy-mm-dd HH:MM:SS" -> yyyymmddHHMMSS。逐字符挑数字,位不够折 0
    // ——格式对不上(空串、坏串)自然折 0,坏账垫底不炸表。
    std::uint64_t key = 0;
    int digits = 0;
    for (const char c : ts) {
        if (c < '0' || c > '9') {
            continue;
        }
        if (digits >= 14) {
            break;
        }
        key = key * 10 + static_cast<std::uint64_t>(c - '0');
        ++digits;
    }
    return digits == 14 ? key : 0;
}

std::uint64_t SessionIdTimeSortKey(const std::string& id) {
    // id = "yyyymmdd-HHMMSS-slug",前 15 个字符(去掉那个 '-')即数字。
    if (id.size() < 15) {
        return 0;
    }
    std::uint64_t key = 0;
    for (std::size_t i = 0; i < 15; ++i) {
        const char c = id[i];
        if (c < '0' || c > '9') {
            if (i == 8 && c == '-') {
                continue;  // 日期与时刻之间那一杠
            }
            return 0;
        }
        key = key * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return key;
}

std::string FileMtimeTimestamp(const std::string& file_path) {
    std::error_code ec;
    const std::filesystem::path path = Utf8Path(file_path);
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::string();
    }
    // file_clock 与 system_clock 的换算没有三家共用的成员出口:to_sys 是
    // libstdc++ 私货,Apple libc++ 连 C++20 的 clock_cast 都没有(CI 实锤
    // 过两轮)。用"两枚 now 的差"这个前 C++20 的老法子,三家都认;两枚
    // now 之间隔了微秒级,对秒级展示时间戳无感。
    const auto sys_now = std::chrono::system_clock::now();
    const auto file_now = std::chrono::file_clock::now();
    // file_clock 的差在 libc++ 上是 __int128 纳秒,隐式缩回 system_clock 的
    // 微秒 duration 不许(又一轮 CI 实锤)——显式 duration_cast 落锤,截掉
    // 的亚微秒对秒级时间戳无感。
    const std::time_t tt = std::chrono::system_clock::to_time_t(
        sys_now - std::chrono::duration_cast<std::chrono::system_clock::duration>(file_now - mtime));
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

SessionSummary SummarizeSessionContent(const std::string& id, const std::string& file_path,
                                       const std::string& content) {
    SessionSummary summary;
    summary.id = id;
    summary.file_path = file_path;
    summary.created_at = SessionIdTimeSortKey(id) > 0
                             ? id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2) + " " +
                                   id.substr(9, 2) + ":" + id.substr(11, 2) + ":" + id.substr(13, 2)
                             : std::string();

    // 逐行过账:meta 行定底子,消息行计数/抽首句,事件行取 title/cwd 与
    // 最后 ts(合法消息/事件行的 ts 都算账,queue 排队快照也是最近的账)。
    std::istringstream iss(content);
    std::string line;
    std::uint64_t last_ts_key = 0;
    std::string last_ts_text;
    bool first_line = true;
    bool meta_ok = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (first_line) {
            first_line = false;
            if (const auto meta = ParseSessionMeta(line); meta.has_value()) {
                meta_ok = true;
                summary.model = meta->model;
                summary.cwd = meta->cwd;
                if (!meta->started_at.empty()) {
                    summary.created_at = meta->started_at;
                }
                continue;
            }
            // 首行不是 meta:整场标 damaged,后面的行照读(能捞多少捞多少,
            // 展示层仍有首句可看)。
        }
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) {
            continue;  // 坏行:不计数、不当账,跳过接着读
        }
        const bool is_event = j.contains("type") && j["type"].is_string();
        if (is_event) {
            const std::string type = j["type"].get<std::string>();
            if (type == "title") {
                const std::string title = TopLevelStringField(j, "title");
                if (!title.empty()) {
                    summary.title = title;  // append-only,最后一条胜
                }
            } else if (type == "cwd") {
                const std::string cwd = TopLevelStringField(j, "cwd");
                if (!cwd.empty()) {
                    summary.cwd = cwd;  // append-only,最后一条胜
                }
            }
            // compact/compact_v2/queue 及认不得的事件:不算消息;ts 若合法
            // 照样记账(queue 快照就是最近的账,别当坏行)。
        } else {
            const auto message = DeserializeSessionMessage(line);
            if (message.has_value()) {
                summary.message_count += 1;
                if (summary.first_user_text.empty() && message->role == api::Role::User) {
                    for (const auto& block : message->content) {
                        if (const auto* tb = std::get_if<api::TextBlock>(&block);
                            tb != nullptr && !tb->text.empty()) {
                            summary.first_user_text = tb->text.substr(0, tb->text.find('\n'));
                            break;
                        }
                        if (const auto* image = std::get_if<api::ImageBlock>(&block);
                            image != nullptr) {
                            summary.first_user_text = "[图片] " + image->filename;
                            break;
                        }
                    }
                }
            } else {
                continue;  // 认不得的行不是账
            }
        }
        const std::string ts = TopLevelStringField(j, "ts");
        if (!ts.empty()) {
            const std::uint64_t key = SessionTimeSortKey(ts);
            if (key > 0 && key >= last_ts_key) {
                last_ts_key = key;
                last_ts_text = ts;
            }
        }
    }
    if (!meta_ok) {
        summary.health = SessionHealth::Damaged;
    }
    if (!last_ts_text.empty()) {
        summary.updated_at = last_ts_text;
        summary.updated_at_key = last_ts_key;
    }
    return summary;
}

bool SessionMatchesQuery(const SessionSummary& summary, const SessionQuery& query) {
    if (query.state != summary.state) {
        return false;
    }
    if (query.scope == SessionScope::Cwd) {
        // 两边都过归一化再比(斜杠方向/大小写/尾斜杠在 NormalizePathForCompare
        // 里归一)。cwd 认不出的场子(空串)在 Cwd 视图里不列。
        if (summary.cwd.empty() || NormalizePathForCompare(summary.cwd) != NormalizePathForCompare(query.cwd)) {
            return false;
        }
    }
    if (query.search.empty()) {
        return true;
    }
    const std::string needle = ToLowerAscii(query.search);
    return ToLowerAscii(summary.title).find(needle) != std::string::npos ||
           ToLowerAscii(summary.first_user_text).find(needle) != std::string::npos ||
           ToLowerAscii(summary.id).find(needle) != std::string::npos ||
           ToLowerAscii(summary.cwd).find(needle) != std::string::npos;
}

std::vector<std::size_t> SortSessionSummaries(const std::vector<SessionSummary>& summaries,
                                              const SessionQuery& query) {
    std::vector<std::size_t> order;
    order.reserve(summaries.size());
    for (std::size_t i = 0; i < summaries.size(); ++i) {
        order.push_back(i);
    }
    const bool by_updated = query.sort == SessionSort::Updated;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const std::uint64_t key_a =
            by_updated ? summaries[a].updated_at_key : SessionTimeSortKey(summaries[a].created_at);
        const std::uint64_t key_b =
            by_updated ? summaries[b].updated_at_key : SessionTimeSortKey(summaries[b].created_at);
        if (key_a != key_b) {
            return key_a > key_b;  // 新→旧
        }
        return summaries[a].id > summaries[b].id;  // 同键退 id 字典倒序,不抖
    });
    return order;
}

// ---------------------------------------------------------------------------
// 台账薄壳
// ---------------------------------------------------------------------------

namespace {

// "size:mtime_count:path"。mtime 用原始计数(file_clock 的 rep),秒级
// 截断会把同秒的两笔写漏认成没变。count() 在 libc++ 上是 __int128,
// 直接进流是歧义重载(macOS CI 实锤)——按 int64 落账:纳秒计数在
// file_clock 纪元(2174 年)的量程内远够不到 int64 上限,截断无损。
std::string MakeFingerprint(const std::string& file_path, std::uintmax_t size,
                            std::filesystem::file_time_type mtime) {
    std::ostringstream oss;
    oss << size << ':'
        << static_cast<std::int64_t>(mtime.time_since_epoch().count()) << ':' << file_path;
    return oss.str();
}

}  // namespace

SessionCatalog::SessionCatalog(std::string sessions_dir) : sessions_dir_(std::move(sessions_dir)) {}

void SessionCatalog::Scan() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::map<std::string, SessionSummary> fresh;
    // 两处扫:根(活动)与 archive/(归档,0.26.25 起 SessionLifecycle 立)。
    // ListSessions/--continue/默认 /resume 只扫根——那是它们自己的口径;
    // 这里两处都收进缓存,查询按 state 筛,Archived 视图才看得到。
    const auto scan_dir = [&](const fs::path& dir, SessionState state) {
        fs::directory_iterator it(dir, ec);
        if (ec) {
            return;  // 目录不存在:还没存过任何会话(archive 常年不立)
        }
        for (const auto& dir_entry : it) {
            if (!dir_entry.is_regular_file(ec) || dir_entry.path().extension() != ".jsonl") {
                continue;
            }
            SessionSummary entry;
            entry.id = PathToUtf8(dir_entry.path().stem());
            entry.file_path = PathToUtf8(dir_entry.path());
            entry.state = state;
            RefreshEntry(entry);
            fresh[entry.id] = std::move(entry);
        }
    };
    scan_dir(Utf8Path(sessions_dir_), SessionState::Active);
    scan_dir(Utf8Path(sessions_dir_) / "archive", SessionState::Archived);
    entries_ = std::move(fresh);
}

void SessionCatalog::RefreshEntry(SessionSummary& entry) const {
    std::error_code ec;
    namespace fs = std::filesystem;
    const SessionState state_before = entry.state;  // Scan 按目录写好;别让重读盖掉
    const fs::path path = Utf8Path(entry.file_path);
    const auto size = fs::file_size(path, ec);
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) {
        // 读不动(被挪走/权限):标 damaged,保留旧摘要里能看的部分。
        entry.health = SessionHealth::Damaged;
        return;
    }
    const std::string fingerprint = MakeFingerprint(entry.file_path, size, mtime);
    const auto cached = entries_.find(entry.id);
    if (cached != entries_.end() && cached->second.file_fingerprint == fingerprint &&
        cached->second.health == SessionHealth::Ok) {
        SessionSummary keep = cached->second;  // 指纹没变:一口盘都不重读
        keep.file_path = entry.file_path;
        entry = std::move(keep);
        return;
    }
    const auto content = ReadSessionFileBytes(entry.file_path);
    if (!content.has_value()) {
        entry.health = SessionHealth::Damaged;
        entry.file_fingerprint = fingerprint;
        return;
    }
    entry = SummarizeSessionContent(entry.id, entry.file_path, *content);
    if (entry.created_at.empty()) {
        // meta 没有 started_at 且 id 不带时间:created 保持空,排序垫底。
    }
    if (entry.updated_at.empty()) {
        entry.updated_at = FileMtimeTimestamp(entry.file_path);  // 账里没 ts,退 mtime
        entry.updated_at_key = SessionTimeSortKey(entry.updated_at);
    }
    // SummarizeSessionContent 返回的 state 是默认 Active,会盖掉 Scan 按
    // 目录写好的值——按指纹前的旧账还原(调用方给的才是真状态)。
    entry.state = state_before;
    entry.file_fingerprint = fingerprint;
}

SessionQueryPage SessionCatalog::Query(const SessionQuery& query) const {
    // 全量重扫只挑指纹变了的场子重读;坏档(health == Damaged 且指纹没变)
    // 不重试——除非文件又动了(指纹变,走重读路)。
    std::vector<SessionSummary> summaries;
    summaries.reserve(entries_.size());
    for (auto& [id, entry] : entries_) {
        SessionSummary copy = entry;
        RefreshEntry(copy);
        summaries.push_back(std::move(copy));
    }

    SessionQueryPage page;
    const auto order = SortSessionSummaries(summaries, query);
    std::vector<std::size_t> hits;
    hits.reserve(order.size());
    for (const std::size_t index : order) {
        if (SessionMatchesQuery(summaries[index], query)) {
            hits.push_back(index);
        }
    }
    page.total = hits.size();
    const std::size_t begin = (std::min)(query.cursor, hits.size());
    const std::size_t end =
        query.limit == 0 ? hits.size() : (std::min)(hits.size(), begin + query.limit);
    for (std::size_t i = begin; i < end; ++i) {
        page.entries.push_back(summaries[hits[i]]);
    }
    return page;
}

const SessionSummary* SessionCatalog::Find(const std::string& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace lubancode::sessions
