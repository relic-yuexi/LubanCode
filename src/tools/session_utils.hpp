// 会话侧仍被生产消费的纯工具(P0-6 从 sessions/session_store.* 与
// sessions/session_lifecycle.* 迁来的活口;旧 JSONL 读写件已删,这里
// 不碰任何旧格式解析——那活在迁移器 tools/legacy-storage-migrator)。
//
//   - 时间戳:NowTimestamp / NowIdTimestamp(meta 与显示共用的本地钟串);
//   - UTF-8 截断:TruncateUtf8Chars / AbbreviateUtf8Middle(/sessions
//     列表与确认屏的显示截断);
//   - 会话 id 拼装:MakeSessionSlug / MakeSessionId(新场次仍用同款
//     yyyymmdd-HHMMSS-slug 形状);
//   - 路径归一比较:NormalizePathForCompare(mention 补全与目录对账);
//   - 引用消歧:ResolveSessionRef(archive/delete 命令的 id 前缀/标题
//     解析,纯函数,候选由 workspace 索引喂)。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"  // api::Message(Markdown 导出的载荷)

namespace lubancode::tools {

// 当前本地时间,"yyyy-mm-dd HH:MM:SS"(显示与账面用)。
std::string NowTimestamp();

// 当前本地时间,"yyyymmddHHMMSS" 掐成 "yyyymmdd-HHMMSS"(会话 id 底子)。
std::string NowIdTimestamp();

// 按 UTF-8 码点截前 max_chars 个字(绝不从多字节字符中间掐断),截了
// 补 "…"。/sessions 列表的首句摘要用。
std::string TruncateUtf8Chars(const std::string& text, std::size_t max_chars);

// 超过 max_chars 个码点时保留头尾、中间换 "…"(总码点数不超过 max_chars)。
// 过长目录路径的缩略显示用。max_chars < 2 时退化成 TruncateUtf8Chars。
std::string AbbreviateUtf8Middle(const std::string& text, std::size_t max_chars);

// 首条用户消息 -> 文件名安全的 slug:按 UTF-8 码点截前 max_chars 个字,
// ASCII 字母数字与 . _ - 原样留,中文等多字节字符原样留,空白与危险字符
// 换 '-',连续 '-' 并成一个,首尾 '-'/'.' 剥掉,全剥没了给 "untitled"。
std::string MakeSessionSlug(const std::string& first_user_text, std::size_t max_chars = 20);

// 会话 id = 启动时间戳(yyyymmdd-HHMMSS)+ "-" + slug。
std::string MakeSessionId(const std::string& timestamp, const std::string& first_user_text);

// cwd 归一化比较键:weakly_canonical 归一(失败退 lexically_normal),
// 反斜杠统一成正斜杠,ASCII 大小写按 Windows 习惯折成小写,尾斜杠剥掉。
// 两个路径指没指同一个目录,比这个函数的返回值。
std::string NormalizePathForCompare(const std::string& utf8_path);

// ---------------------------------------------------------------------------
// 会话引用消歧(archive/delete 等管理命令的入参解析)
// ---------------------------------------------------------------------------

// 引用解析的候选账(ambiguous 时给调用方列给人看)。候选来自 workspace
// 会话索引(trajectory::QueryWorkspaceSessions),这里不读盘。
struct SessionRefCandidate {
    std::string id;
    std::string file_path;
    std::string title;   // 展示用,可为空
};

// 引用:id(完整或唯一前缀)或标题。解析顺序:先完整 id,再唯一前缀,
// 再标题唯一命中;重名/多义给 ambiguous=true 并返回全部命中(调用方列
// 短 id 叫用户点明);没有命中给 nullopt。纯函数可单测。
std::optional<std::vector<SessionRefCandidate>> ResolveSessionRef(
    const std::vector<SessionRefCandidate>& candidates, const std::string& ref, bool& ambiguous);

// ---------------------------------------------------------------------------
// Markdown 导出(P0-6 自 sessions/session_store.* 迁来;/export 新路吃)
// ---------------------------------------------------------------------------

// 导出头部四件(会话元信息;trajectory 路按 ReplayState 折叠补,缺省空)。
struct ExportSessionHeader {
    std::string started_at;
    std::string wire;
    std::string model;
    std::string cwd;
};

// 纯函数:会话 -> Markdown。用户/助手分节(## 用户 / ## 助手),工具调用
// 折叠成 <details>(名字 + 入参 JSON + 结果前 max_result_lines 行,超了标注
// 省略);tool_result 就近配对到 tool_use 的 details 里,只装着 tool_result
// 的 user 消息不再单开"用户"一节。title 非空时用它当大标题(会话 id 降为
// 一行元信息);compact_positions 里的每个位置(第 N 条消息之前)插一行
// "> ⚡ 此处发生过一次上下文压缩" 标注。
std::string ExportSessionMarkdown(const ExportSessionHeader& header,
                                  const std::vector<api::Message>& messages, const std::string& session_id,
                                  int max_result_lines = 30, const std::string& title = std::string(),
                                  const std::vector<std::size_t>& compact_positions = {});

}  // namespace lubancode::tools
