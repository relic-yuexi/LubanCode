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
// api_key 之类的敏感配置本来就不在历史消息里,meta 也只存 wire/model/cwd,
// 存档文件里不会出现任何凭据。

#pragma once

#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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
    std::vector<api::Message> messages;  // 已做过 RepairToolPairs
    int repaired = 0;                    // 修补的孤儿 tool_use 块数
    int skipped_lines = 0;               // 解析不动、跳过的行数
};

// 纯函数:整个 .jsonl 文件内容 -> meta + 消息列表(已修补成对)。首行不是
// 合法 meta 给 nullopt(压根不是本工具的存档,别硬解)。
std::optional<LoadedSession> ParseSessionFile(const std::string& content);

// 纯函数:会话 -> Markdown。用户/助手分节(## 用户 / ## 助手),工具调用
// 折叠成 <details>(名字 + 入参 JSON + 结果前 max_result_lines 行,超了标注
// 省略);tool_result 就近配对到 tool_use 的 details 里,只装着 tool_result
// 的 user 消息不再单开"用户"一节。
std::string ExportSessionMarkdown(const SessionMeta& meta, const std::vector<api::Message>& messages,
                                   const std::string& session_id, int max_result_lines = 30);

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
    std::string first_user_text;  // 首条用户消息第一行(原样,展示层自己截)
    std::size_t message_count = 0;
};

// 扫 sessions_dir 下的 *.jsonl,按 id 倒序(id 以 yyyymmdd-HHMMSS 起头,
// 字典倒序即时间倒序)取最近 limit 场。目录不存在给空表。
std::vector<SessionListEntry> ListSessions(const std::string& sessions_dir, std::size_t limit = 20);

// 整个读进来(二进制,按 UTF-8 字节串用)。读不到给 nullopt。
std::optional<std::string> ReadSessionFileBytes(const std::string& file_path);

// 当前本地时间,"yyyy-mm-dd HH:MM:SS"。meta.started_at / 消息 ts 用。
std::string NowTimestamp();

// 当前本地时间,"yyyymmddHHMMSS" 掐成 "yyyymmdd-HHMMSS"。会话 id 用。
std::string NowIdTimestamp();

}  // namespace lubancode::agent
