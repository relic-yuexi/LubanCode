// workspace/session 可重建索引(P0-2:Trajectory 升为唯一 Session)。
//
// /sessions、/resume 选择器、app-server thread/list、Ctrl+R 提问历史共同
// 的读侧投影:不重放整本 Journal,只吃 <workspace>/indexes/sessions.json
// 这份可重建账(单子 §5.2"优先读可重建 workspace/session 索引,不为每次
// 列表重放所有 Journal")。
//
// 索引是派生物,不是真本:每场记一枚指纹(session.json 与 main.jsonl 的
// 字节数),查询时对一遍目录——指纹没动的场直接用旧摘要,动了/新增的场
// 才单遍重扫该场 main.jsonl,末后原子写回。索引丢了/坏了就地重建,一个
// 字节不亏(Journal 才是真账)。
//
// 悬空的运行中 session(本进程或他进程 active)照列——列表是"有哪些场"
// 的事实,不是"哪些场可 resume"的裁定;可恢复性由 SessionManager 的
// resume 七步验账。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// 一场 session 的摘要(索引行)。
struct WorkspaceSessionSummary {
    std::string workspace_key;
    std::string session_id;
    std::string status;              // session.json 的 status(running/closed/archived/…)
    bool archived = false;           // status == archived
    std::string title;               // 最后一条 control.title.changed(可空)
    std::string first_user_text;     // 首条 input.received 的首段文本(可空)
    std::string cwd;                 // 最后一条 control.cwd.changed,空回落 launch_cwd
    std::string model;               // 首个 model.request.prepared 的 model(可空)
    std::int64_t created_at_ms = 0;  // session.json created_at_ms
    std::int64_t updated_at_ms = 0;  // 末事件 wall_time_ms(无事件回落 created)
    std::uint64_t message_count = 0;  // input.received + model.output.completed 计数
    std::uint64_t event_count = 0;    // main.jsonl 总行数(完整事件)
    bool damaged = false;            // 尾行解析不开/manifest 读不动
    bool misplaced_v1 = false;       // 新根下 schema_version=1:旧档搬错家
    std::string session_dir;         // UTF-8 绝对路径(消费方按需读账用)
};

// 查询形状(终端 /sessions、picker、app-server thread/list 共用)。
struct SessionIndexQuery {
    bool all_workspaces = false;          // false = 只列 current_workspace_key
    std::string current_workspace_key;    // scope 本仓时的 key
    bool include_archived = false;        // 归档场(archived 只读入口)
    bool archived_only = false;           // /sessions archived
    bool sort_by_created = false;         // 缺省按 updated 新→旧
    std::string search;                   // 标题/首句/会话 id 子串
    std::size_t limit = 0;                // 0 = 不截
    std::size_t cursor = 0;               // 跳过头 N 条(分页)
};

struct SessionIndexPage {
    std::vector<WorkspaceSessionSummary> entries;  // 新→旧
    std::size_t total = 0;                         // 过滤后总条数(截断前)
};

// 查询口(只读 + 惰性重建索引)。workspaces_root 不存在给空页,不冒充。
SessionIndexPage QueryWorkspaceSessions(const std::filesystem::path& workspaces_root,
                                        const SessionIndexQuery& query);

// 提问历史行(Ctrl+R;新→旧由本函数排好,max_lines 截条)。
struct PromptHistoryLine {
    std::string workspace_key;
    std::string session_id;
    std::string title;  // 该场最后标题(可空)
    std::string text;   // 提问正文(首段文本块)
    std::int64_t ts_ms = 0;
    std::uint64_t seq = 0;  // 场内提问次序(1 起;身份去重用)
};

// 当前 workspace 的提问历史(索引重建时顺带抽;读同一份派生账)。
std::vector<PromptHistoryLine> ReadWorkspacePromptHistory(const std::filesystem::path& workspaces_root,
                                                          const std::string& workspace_key,
                                                          std::size_t max_lines);

// 转录节选行(/resume 选择器的 Ctrl+T 浮层):按场读 main.jsonl 的
// input/model.output 首行,头尾各 max_half 行。纯读,坏了给空。
std::vector<std::string> MakeSessionTranscriptExcerpt(const std::filesystem::path& session_dir,
                                                      std::size_t max_half);

// epoch 毫秒 -> 本地时间 "yyyy-mm-dd HH:MM:SS"(与旧存档时间串同口径,
// 展示层用;坏值折空串)。
std::string FormatMillisAsLocalTimestamp(std::int64_t ms);

// trace/query 的工具执行折叠(P0-2:app-server 断线补账/冷回放,从
// main.jsonl 的 tool.* 事件折协议形状的执行账;与旧 tool_trace_v1 的
// 回放同语义,数据源换成唯一真账)。preview 只给首段文本的摘要(遮敏
// 默认,不回 inline 原文)。
struct SessionToolTraceFold {
    std::vector<nlohmann::json> executions;  // 行序 = planned 序(seq 升序)
    std::uint64_t max_seq = 0;               // 折叠到的最大事件 seq
};
SessionToolTraceFold FoldSessionToolExecutions(const std::filesystem::path& session_dir);

}  // namespace lubancode::trajectory
