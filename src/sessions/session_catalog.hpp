// SessionCatalog:会话台账的中立查询层(会话管理器单第一步)。
//
// 只查账:扫 sessions 目录、算摘要、按查询条件筛/排/分页。不 include
// Theme、ChoiceMenu,不碰 stdout——终端、app-server、Web 都能吃这碗饭。
// 搬与删(archive/unarchive/delete)归 SessionLifecycle(第 4、5 步),这里
// 连一行文件都不写。
//
// 时间口径(单子"产品定案二"定死):
//   created_at = meta.started_at;缺了退会话 id 时间(id 以 yyyymmdd-HHMMSS
//               起头);再缺给空串(排序时垫底)。
//   updated_at = 文件里最后一条带合法 ts 的账(消息行、事件行——queue
//               排队快照也是账,0.26.18 起算);都没有退文件 mtime。
//
// 缓存口径:打开时扫一回,按 path + size + mtime 算指纹;之后查询先比
// 指纹,变了才重读那一场。键盘搜索只筛内存,不因每敲一字重读盘。
//
// 坏档口径:坏 meta 只标那一场 damaged(health 字段),不拖垮整表。
// state(第四步起):根目录 = Active,archive/ 子目录 = Archived。查询按
// state 筛——默认(active)不掺归档,Archived 视图另查。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::sessions {

// 查询合同。scope/state/sort/search/cursor/limit 五件,与单子
// "代码边界 SessionCatalog"一节逐字对齐。
enum class SessionScope { Cwd, All };
enum class SessionState { Active, Archived };
enum class SessionSort { Updated, Created };

struct SessionQuery {
    SessionScope scope = SessionScope::Cwd;
    SessionState state = SessionState::Active;
    SessionSort sort = SessionSort::Updated;
    std::string search;      // 空 = 不筛;命中 title/首句/id/cwd,ASCII 不分大小写
    std::string cwd;         // scope == Cwd 时的本目录(归一化比较)
    std::size_t cursor = 0;  // 跳过前 cursor 条(分页)
    std::size_t limit = 20;  // 本页条数;0 = 不截
};

// 健康度:ok = 正常;damaged = 首行 meta 读不出/整个文件读不动,只这一场
// 标坏,别的场照列。
enum class SessionHealth { Ok, Damaged };

// 一场会话的摘要。时间字段用存档侧的稳定串("yyyy-mm-dd HH:MM:SS" /
// "yyyymmdd-HHMMSS"),相对时间(1m ago 那类)归终端渲染层算。
struct SessionSummary {
    std::string id;               // 文件名去掉 .jsonl
    std::string file_path;
    std::string title;            // 最后一条 title 事件;没有就空
    std::string first_user_text;  // 首条用户消息第一行
    std::string cwd;              // meta.cwd(含 cwd 事件回放,最后一条胜)
    std::string model;            // meta.model
    std::string created_at;       // meta.started_at;缺退 id 时间;再缺空串
    std::string updated_at;       // 最后合法账的 ts;缺退 mtime 串;再缺空串
    std::uint64_t updated_at_key = 0;  // updated_at 的排序键(yyyy-mm-dd HH:MM:SS
                                       // 字典序即时间序,坏串折 0 垫底)
    std::size_t message_count = 0;     // 全量消息行数(事件行不算)
    SessionState state = SessionState::Active;
    SessionHealth health = SessionHealth::Ok;
    std::string file_fingerprint;  // "size:mtime:path",缓存指纹
};

// 一页查询结果。total 是筛选/排序后的命中总数(不受分页影响),底栏
// "当前序号 / 命中总数 · 百分比"吃的就是它。
struct SessionQueryPage {
    std::vector<SessionSummary> entries;
    std::size_t total = 0;
};

// ---------------------------------------------------------------------------
// 纯函数(可单测)
// ---------------------------------------------------------------------------

// "yyyy-mm-dd HH:MM:SS" -> 排序键(纯数字,同格式字典序即时间序);
// 认不出的串折 0。updated_at 排序、百分比算式共吃这一口。
std::uint64_t SessionTimeSortKey(const std::string& ts);

// "yyyymmdd-HHMMSS" 的 id 前缀 -> 同款排序键;认不出折 0。
std::uint64_t SessionIdTimeSortKey(const std::string& id);

// 文件 mtime -> "yyyy-mm-dd HH:MM:SS" 本地时间串。拿不到给空串。
std::string FileMtimeTimestamp(const std::string& file_path);

// 一份 JSONL 全文 -> 摘要(不算指纹,调用方自己蘸)。首行 meta 读不出
// 给 damaged——只要 id/file_path/created(退 id 时间)/updated(退 mtime
// 由调用方蘸)。单子口径:坏一场不废整表,所以这里只标不抛。
SessionSummary SummarizeSessionContent(const std::string& id, const std::string& file_path,
                                       const std::string& content);

// id + cwd(scope == Cwd 时非空,两边都过 NormalizePathForCompare 再比)。
// 搜索命中 title/first_user_text/id/cwd;ASCII 折小写比,多字节原样比。
bool SessionMatchesQuery(const SessionSummary& summary, const SessionQuery& query);

// 摘要表 -> 排好序的命中下标(新→旧)。排序键相同退 id 字典序(倒序语义
// 下的稳定 tie-break,同秒开两场也不抖)。
std::vector<std::size_t> SortSessionSummaries(const std::vector<SessionSummary>& summaries,
                                              const SessionQuery& query);

// ---------------------------------------------------------------------------
// 磁盘薄壳:带指纹缓存的台账
// ---------------------------------------------------------------------------

// 打开时 Scan 一回,之后 Query 按指纹增量重读。不常驻线程、不建索引库
// ——JSONL 仍是唯一真账。
class SessionCatalog {
public:
    explicit SessionCatalog(std::string sessions_dir);

    // 扫一遍目录,建/补摘要缓存。目录不存在给空表(还没存过任何会话)。
    // 若干文件读不动:那几场标 damaged,其余照常。
    void Scan();

    // 按查询合同出一页。先对缓存里的每场比指纹,变了才重读那一场。
    SessionQueryPage Query(const SessionQuery& query) const;

    // 缓存里现有条数(测试/诊断用)。
    std::size_t size() const { return entries_.size(); }

    // 台账看的会话根目录(转录浮层按 id 拼路径用;只读)。
    const std::string& sessions_dir() const { return sessions_dir_; }

    // id -> 摘要(缓存里没有给 nullptr)。
    const SessionSummary* Find(const std::string& id) const;

private:
    // 单场摘要按指纹比对,变了(或头一回见)重读文件。
    void RefreshEntry(SessionSummary& entry) const;

    std::string sessions_dir_;
    // id -> 摘要。map 按 id 字典序存,查询时另按 sort 排。
    std::map<std::string, SessionSummary> entries_;
};

}  // namespace lubancode::sessions
