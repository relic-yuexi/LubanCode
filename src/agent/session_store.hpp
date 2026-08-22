// 会话存档与续聊(0.13.x):把交互模式的对话历史逐条追加写进
// <主目录>/.lubancode/sessions/<会话id>.jsonl,退出后能用 /resume 或
// --continue 接着聊。
//
// 分层:序列化/反序列化/成对修补/Markdown 导出全是纯函数,只吃 api::Message
// 这些中立类型,不碰 config/(sessions 目录的路径由调用方 main.cpp 算好传
// 进来,agent/ 保持不反向依赖 config/ 的老规矩)。真正碰磁盘的只有
// SessionStore(append+flush 的落盘句柄)、ListSessions/ReadSessionFile
// (列目录/读文件)这几个薄壳。
//
// 文件格式(.jsonl,一行一条 JSON):
//   首行 meta: {"version":1,"wire":...,"model":...,"cwd":...,"started_at":...}
//   之后每行一条消息: {"role":"user"|"assistant","content":[块数组],"ts":...}
//   内容块按中立类型序列化:
//     {"type":"text","text":...}
//     {"type":"tool_use","id":...,"name":...,"input":{...}}
//     {"type":"tool_result","tool_use_id":...,"content":...,"is_error":...}
// 0.16.x 起,消息行之外还有两种**事件行**(顶层带 "type" 字段,消息行没有,
// 靠这个区分;旧版本读到事件行会当坏行跳过,一个不坏):
//   压缩事件: {"type":"compact","archive":{"role":...,"content":[...]},
//              "kept_from":<int>,"ts":...}
//     压缩(手动 /compact 或自动)发生时追加。archive 是压缩后顶在有效历史
//     最前面的那条消息(存档正文已并入保留轮的 user 文本,跟内存里
//     BuildCompactedHistory 的产物一致);kept_from 是压缩前**有效历史**里
//     从第几条(0 起数)起原样保留。回放时把已积累的有效列表换成
//     [archive] + 有效列表[kept_from..],恢复出来的正是压缩后的活状态。
//   标题事件: {"type":"title","title":...,"ts":...}
//     /title 追加,append-only,最后一条胜。
// api_key 之类的敏感配置本来就不在历史消息里,meta 也只存 wire/model/cwd,
// 存档文件里不会出现任何凭据。

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// meta(首行)
// ---------------------------------------------------------------------------

struct SessionMeta {
    int version = 1;          // 存档格式版本,往后格式变了靠它识别
    std::string wire;         // "anthropic" / "responses"
    std::string model;
    std::string cwd;
    std::string started_at;   // "yyyy-mm-dd HH:MM:SS",本地时间
};

// meta -> 一行 JSON(不带换行符)。
std::string SerializeSessionMeta(const SessionMeta& meta);

// 一行 JSON -> meta。不是合法 JSON、或者缺 version 字段,给 nullopt。
std::optional<SessionMeta> ParseSessionMeta(const std::string& line);

// ---------------------------------------------------------------------------
// 消息(每行一条)
// ---------------------------------------------------------------------------

// 消息 -> 一行 JSON(不带换行符)。ts 是这条消息落盘时刻,调用方给。
std::string SerializeSessionMessage(const api::Message& message, const std::string& ts);

// 一行 JSON -> 消息。解析不动(坏 JSON、缺字段、认不得的块类型)给 nullopt,
// 调用方跳过这一行接着读下一行——存档坏一行不该废掉整场会话。
std::optional<api::Message> DeserializeSessionMessage(const std::string& line);

// ---------------------------------------------------------------------------
// 事件行(compact / title)
// ---------------------------------------------------------------------------

// 压缩事件:archive 是压缩后顶在有效历史最前面的那条消息(存档正文已并入
// 保留轮),kept_from 是压缩前有效历史里从第几条(0 起数)起原样保留。
struct CompactEvent {
    api::Message archive;
    std::size_t kept_from = 0;
};

// 事件 -> 一行 JSON(不带换行符)。
std::string SerializeCompactEvent(const CompactEvent& event, const std::string& ts);

// 一行 JSON -> 压缩事件。不是合法 JSON、type 不是 "compact"、缺 archive 或
// kept_from(或类型不对),给 nullopt——坏事件行调用方跳过,不废整场。
std::optional<CompactEvent> ParseCompactEvent(const std::string& line);

// 由"压缩前有效历史长度 + BuildCompactedHistory 拼出的新历史"算出事件:
// archive = new_history.front(),kept_from = old_size - (new_history.size()-1)。
// 新历史为空(不该发生,纯防御)给 kept_from = old_size + 空 archive。
CompactEvent MakeCompactEvent(std::size_t old_history_size, const std::vector<api::Message>& new_history);

// 回放一次压缩事件:effective 换成 [archive] + effective[kept_from..]。
// kept_from 越界按"全不保留"处理(夹到 effective.size()),不越界访问。
std::vector<api::Message> ApplyCompactEvent(std::vector<api::Message> effective, const CompactEvent& event);

// ---------------------------------------------------------------------------
// compact_v2 事件(0.27.x 第三期):分层压缩的完整记账。
//
// 回放语义与 v1 完全同型(archive + kept_from),/resume 读到 v2 与读到 v1
// 走同一条重建路;多的字段(manifest/epoch/metrics)供审计、/export 与
// "从原始事件 rebase"用,不影响老版本读档(老版本当坏行跳过,消息账无损)。
// ---------------------------------------------------------------------------

struct CompactV2Event {
    api::Message archive;      // 同 v1:压缩后顶在最前的消息(存档已并入保留轮)
    std::size_t kept_from = 0; // 同 v1:压缩前有效历史从第几条起原样保留
    int epoch = 0;             // 本场第几次压缩(v1 也计数,从 1 起)
    nlohmann::json manifest;   // 终稿 manifest(goal/constraints/open_items/...)
    nlohmann::json metrics;    // chunks/reduce_passes/pre_post_tokens/trigger 等
};

// v2 事件 -> 一行 JSON(不带换行符)。
std::string SerializeCompactV2Event(const CompactV2Event& event, const std::string& ts);

// 一行 JSON -> v2 事件。type 不是 "compact_v2"、缺 archive/kept_from 给
// nullopt;manifest/metrics/epoch 缺失按默认空值收(老写档兼容)。
std::optional<CompactV2Event> ParseCompactV2Event(const std::string& line);

// v1 事件升级成 v2(epoch/manifest/metrics 由调用方补)。
CompactV2Event UpgradeToV2(const CompactEvent& event, int epoch, nlohmann::json manifest, nlohmann::json metrics);

// v2 事件折回 v1 形状(回放共用一条路)。
CompactEvent AsCompactEvent(const CompactV2Event& event);

// 标题事件 -> 一行 JSON(不带换行符)。
std::string SerializeTitleEvent(const std::string& title, const std::string& ts);

// 一行 JSON -> 标题。type 不是 "title" 或缺 title 字段给 nullopt。
std::optional<std::string> ParseTitleEvent(const std::string& line);

// cwd 事件(0.27.x):meta.cwd 是首行写死的,会话中途 /worktree 进房出房、
// 换目录都追加一行 cwd 事件,append-only 最后一条胜。回放时覆盖 meta.cwd,
// /resume 拿它把会话送回原房(验明正身那条路)。
std::string SerializeCwdEvent(const std::string& cwd, const std::string& ts);

// 一行 JSON -> cwd。type 不是 "cwd" 或缺 cwd 字段给 nullopt。
std::optional<std::string> ParseCwdEvent(const std::string& line);

// ---------------------------------------------------------------------------
// queue 事件(取走即消费单路径二:排队消息落会话存档)
//
// 排队消息只活内存,排了没送走就 /exit 或崩掉,resume 载回来的是空队列
// ——用户视角"消息不见了"。这里给存档添一种事件行,与 compact/title 同列:
//   {"type":"queue","items":[{"id":1,"target":"main"|"#3","text":...,
//                             "attempts":0},...],"ts":...}
// 追加时机(append-only 快照式):排队账一变(进队/送达出队/失败回还)就
// 追加一份全量快照,回放取**最后一条**。已送达的条目不在快照里,天然出档。
// 目标用 short_label 的写法("main" / "#3"):子代理任务号本就带 #,解析时
// 按前缀分型;存档里不引 cli 层类型,agent/ 不反向依赖的老规矩不破。
// 老版本读到 queue 行当坏行跳过,消息账无损(事件行的通用约定)。
// ---------------------------------------------------------------------------

// 存档侧的排队条目(中立形状,不带 cli 层的编辑事务/状态机)。
struct ArchivedQueueItem {
    std::uint64_t id = 0;
    bool subagent = false;   // false = main 会话,true = 子代理
    int task_id = 0;         // subagent 时有效
    std::string text;
    int attempts = 0;        // 自动发送失败回还过的次数(0 = 没试过)
};

// 一份快照 -> 一行 JSON(不带换行符)。items 为空也给 nullopt 之外的空串?
// 不:空账不落行(退出前队列已空就什么也不追加,回放侧最后一条胜的语义
// 之下,"空"由"没有 queue 行"或"最后一条 queue 行 items 为空"两种写法都
// 表达得了;写侧只在非空时追加)。
std::string SerializeQueueEvent(const std::vector<ArchivedQueueItem>& items, const std::string& ts);

// 一行 JSON -> 排队快照。type 不是 "queue"、缺 items 或不是数组,给
// nullopt;单条里缺 text 或 id 非法,跳过那一条不废整份。
std::optional<std::vector<ArchivedQueueItem>> ParseQueueEvent(const std::string& line);

// ---------------------------------------------------------------------------
// 会话 id
// ---------------------------------------------------------------------------

// 首条用户消息 -> 文件名安全的 slug:按 UTF-8 码点截前 max_chars 个字
// (绝不从多字节字符中间掐断),ASCII 字母数字原样留,中文等多字节字符
// 原样留,空白和文件名危险字符(\ / : * ? " < > | 及控制符)换成 '-',
// 连续 '-' 并成一个,首尾 '-' 剥掉。全剥没了给 "untitled"。
std::string MakeSessionSlug(const std::string& first_user_text, std::size_t max_chars = 20);

// 会话 id = 启动时间戳(yyyymmdd-HHMMSS)+ "-" + slug。
std::string MakeSessionId(const std::string& timestamp, const std::string& first_user_text);

// 按 UTF-8 码点截前 max_chars 个字(绝不从多字节字符中间掐断),截了补 "…"。
// /sessions 列表的首句摘要用。
std::string TruncateUtf8Chars(const std::string& text, std::size_t max_chars);

// 超过 max_chars 个码点时保留头尾、中间换 "…"(总码点数不超过 max_chars)。
// /sessions all 缩略显示过长目录路径用。max_chars < 2 时退化成 TruncateUtf8Chars。
std::string AbbreviateUtf8Middle(const std::string& text, std::size_t max_chars);

// cwd 归一化比较键:weakly_canonical 归一(失败退 lexically_normal),
// 反斜杠统一成正斜杠,ASCII 大小写按 Windows 习惯折成小写,尾斜杠剥掉。
// 两个路径指没指同一个目录,比这个函数的返回值。
std::string NormalizePathForCompare(const std::string& utf8_path);

// ---------------------------------------------------------------------------
// tool_use / tool_result 成对修补
// ---------------------------------------------------------------------------

// 恢复历史前过一遍:assistant 消息里每个 tool_use,下一条 user 消息里必须有
// 对应 tool_use_id 的 tool_result(API 硬约束,缺了直接 400)。孤儿 tool_use
// 就地补一条 is_error 的 tool_result "[会话恢复:结果缺失]"——紧跟在该
// assistant 消息之后(下一条 user 消息里补进去,没有就插一条新 user 消息)。
// 反向的孤儿 tool_result(对不上任何 tool_use)直接删掉。返回修补的块数。
int RepairToolPairs(std::vector<api::Message>& history);

// ---------------------------------------------------------------------------
// 整文件解析 / 导出
// ---------------------------------------------------------------------------

struct LoadedSession {
    SessionMeta meta;
    // 有效态:按顺序回放 compact 事件之后的消息列表(已做过 RepairToolPairs),
    // /resume 恢复进内存的就是这份——压缩后的活状态,不背全量旧账。
    std::vector<api::Message> messages;
    // 全量流水:文件里全部消息行,原样按序(不回放事件、不修补)。/export
    // 走这份。旧存档(无事件行)两份内容一致(除修补差异)。
    std::vector<api::Message> all_messages;
    // 每次压缩发生在全量流水的第几条之前(即事件行之前已有的消息条数),
    // 升序;/export 按这个位置插标注行。
    std::vector<std::size_t> compact_positions;
    std::string title;                   // 最后一条 title 事件;没有就空
    int compact_count = 0;               // 回放掉的 compact 事件数(v1 + v2 都算)
    int compact_epoch = 0;               // 最后一次压缩的序号(第几次压缩)
    nlohmann::json last_compact_manifest;  // 最后一次压缩的 manifest(没有就 null)
    int repaired = 0;                    // 修补的孤儿 tool_use 块数
    int skipped_lines = 0;               // 解析不动、跳过的行数(坏事件行也算)
    // 最后一条 queue 事件快照(排队消息落档单):没有 queue 行就是空表。
    // /resume 拿它重建会话层 SteeringQueue(空表 = 档里没排队的账)。
    std::vector<ArchivedQueueItem> queued_messages;
};

// 纯函数:整个 .jsonl 文件内容 -> meta + 有效态消息列表(事件已回放、已修补
// 成对)+ 全量流水。首行不是合法 meta 给 nullopt(压根不是本工具的存档,
// 别硬解)。旧存档没有事件行,按现状全量恢复,一个不坏。
std::optional<LoadedSession> ParseSessionFile(const std::string& content);

// 纯函数:会话 -> Markdown。用户/助手分节(## 用户 / ## 助手),工具调用
// 折叠成 <details>(名字 + 入参 JSON + 结果前 max_result_lines 行,超了标注
// 省略);tool_result 就近配对到 tool_use 的 details 里,只装着 tool_result
// 的 user 消息不再单开"用户"一节。title 非空时用它当大标题(会话 id 降为
// 一行元信息);compact_positions 里的每个位置(第 N 条消息之前)插一行
// "> ⚡ 此处发生过一次上下文压缩" 标注。
std::string ExportSessionMarkdown(const SessionMeta& meta, const std::vector<api::Message>& messages,
                                   const std::string& session_id, int max_result_lines = 30,
                                   const std::string& title = std::string(),
                                   const std::vector<std::size_t>& compact_positions = {});

// ---------------------------------------------------------------------------
// 磁盘薄壳
// ---------------------------------------------------------------------------

// 一场会话的落盘句柄:Begin() 建目录建文件写 meta 行,AppendMessage() 一条
// 消息一行、写完立刻 flush(崩溃安全,不攒);ResumeAt() 接管已有存档文件
// 接着追加。整个生命周期文件以 append 模式持有。
class SessionStore {
public:
    explicit SessionStore(std::string sessions_dir);

    // 开新会话文件。失败(建目录/开文件不成)返回 false,调用方打个警告
    // 继续聊——落盘失败不该拦着人用。
    bool Begin(const SessionMeta& meta, const std::string& session_id);

    // /resume:接管已有文件,后续 AppendMessage 追加到它尾巴上。
    bool ResumeAt(const std::string& file_path, const std::string& session_id);

    // 追加一条消息(自动带当前时刻的 ts),append+flush。
    bool AppendMessage(const api::Message& message);

    // 追加一条压缩事件行(自动带 ts),append+flush。
    bool AppendCompactEvent(const CompactEvent& event);

    // 追加一条 compact_v2 事件行(自动带 ts),append+flush。
    bool AppendCompactV2Event(const CompactV2Event& event);

    // 追加一条标题事件行(自动带 ts),append+flush。
    bool AppendTitleEvent(const std::string& title);

    // 追加一条 cwd 事件行(自动带 ts),append+flush。/worktree 进出房、
    // 会话目录搬迁时各追一条,/resume 回放取最后一条。
    bool AppendCwdEvent(const std::string& cwd);

    // 追加一条排队消息快照事件行(自动带 ts),append+flush。排队账一变
    // (进队/送达/回还)由会话层追一份全量快照;/resume 回放取最后一条。
    bool AppendQueueEvent(const std::vector<ArchivedQueueItem>& items);

    // /clear:关掉当前文件(留在磁盘上),回到"没有活动会话"状态,下一条
    // 用户消息再 Begin 一场新的。
    void Reset();

    bool active() const { return out_.is_open(); }
    const std::string& session_id() const { return session_id_; }
    const std::string& file_path() const { return file_path_; }
    const std::string& sessions_dir() const { return sessions_dir_; }

private:
    std::string sessions_dir_;
    std::string session_id_;
    std::string file_path_;
    std::ofstream out_;
};

// /sessions 列表用的一条。
struct SessionListEntry {
    std::string id;               // 文件名去掉 .jsonl
    std::string file_path;
    std::string started_at;       // meta 里的,读不出 meta 就空着
    std::string cwd;              // meta 里的,/sessions all 显示用
    std::string title;            // 最后一条 title 事件;没有就空,展示层回退首句摘要
    std::string first_user_text;  // 首条用户消息第一行(原样,展示层自己截)
    std::size_t message_count = 0;  // 全量消息行数(事件行不算)
};

// 扫 sessions_dir 下的 *.jsonl,按 id 倒序(id 以 yyyymmdd-HHMMSS 起头,
// 字典倒序即时间倒序)取最近 limit 场。目录不存在给空表。
// cwd_filter 非空时只留 meta.cwd 跟它指同一个目录的场子(两边都过
// NormalizePathForCompare 再比);空串 = 不过滤,全列。
std::vector<SessionListEntry> ListSessions(const std::string& sessions_dir, std::size_t limit = 20,
                                            const std::string& cwd_filter = std::string());

// 整个读进来(二进制,按 UTF-8 字节串用)。读不到给 nullopt。
std::optional<std::string> ReadSessionFileBytes(const std::string& file_path);

// ---------------------------------------------------------------------------
// 提问历史抽取(0.30.x Ctrl+R 反向搜索)
// ---------------------------------------------------------------------------

// 一条从存档里抽出的用户提问。只读事件账,不新增任何写路径。
struct PromptHistoryRecord {
    std::string text;  // 提问正文(多行拼 '\n')
    std::string ts;    // 存档消息行里的原始 ts
};

// 纯函数:一场存档的完整 JSONL -> 该场的用户提问记录。只收"role==user 且
// 首块是纯 text、无 tool_result 块"的行——工具结果、密钥、未发送草稿、
// 事件行(compact/title/cwd)一概不进;以 '/' 起头的 slash 命令不是提问,
// 也不进。坏行跳过,不废整份。时间序,文件里什么序就什么序。
std::vector<PromptHistoryRecord> ExtractPromptHistory(const std::string& jsonl_content);

// 当前本地时间,"yyyy-mm-dd HH:MM:SS"。meta.started_at / 消息 ts 用。
std::string NowTimestamp();

// 当前本地时间,"yyyymmddHHMMSS" 掐成 "yyyymmdd-HHMMSS"。会话 id 用。
std::string NowIdTimestamp();

}  // namespace lubancode::agent
