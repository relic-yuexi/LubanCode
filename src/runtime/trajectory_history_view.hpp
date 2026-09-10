// v3 历史时间线 → 显示 DTO(P3 显示侧第一棒;session_switch.hpp 接线点
// 3/5)。
//
// 分层规矩(同 trajectory_session.hpp:481 注释):runtime 适配层做翻译,
// trajectory 纯库不认 api 层类型;cli(终端 resume 重放)与(未来的)
// app_server/browser 都从这一层拿——显示层不许直接碰 reader.hpp 的
// C++ 结构。DTO 保留逐消息的上下文状态标志(§4.10:多次压缩不能靠永久
// 布尔)与压缩标记的持久 token 字段(§4.11:读 applied 持久字段,resume
// 不拿今天的 tokenizer 重算)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::runtime {

// v3 会话目录识别(session_switch 接线点 2 的显示层转发):session_dir
// 没有 main.jsonl、却有 <id>.jsonl 且首行 schemaVersion==3 时回该流路径。
// cli/app_server 从这层拿,不直接碰 trajectory::v3;识别不出给 nullopt,
// 调用方按 v2 老路走,不猜。
std::optional<std::filesystem::path> FindV3HistoryStream(const std::filesystem::path& session_dir);

// 一条旧消息的显示投影:api 四角色正文 + 上下文状态三标志。hidden 是
// display.hidden(§4.28"默认时间线不单独显示正文,不等于删除"):显示
// 层默认不渲染,详情/展开档另算;system 消息是上下文根,不进显示 DTO。
struct RestoredMessageView {
    api::Message message;  // role/content(text/tool_use/tool_result 块)
    std::string message_id;
    std::string timestamp;
    bool in_current_context = false;       // 在当前链上
    bool replaced_by_derivation = false;   // 降档退链的原版(§4.38)
    std::vector<std::string> removed_by_compacts;  // 历次移出上下文的 compactId
    bool hidden = false;                   // display.hidden:默认不渲染
};

// 压缩分界线的显示投影:token 数字读 compact.applied 持久字段,不许重算;
// removed/retained refs 供"可展开查看摘要、保留范围"的详情档(§4.10)。
struct RestoredCompactView {
    std::string compact_id;
    std::string event_id;
    std::string timestamp;
    std::uint64_t context_tokens_before = 0;
    std::uint64_t context_tokens_after = 0;
    std::vector<std::string> removed_message_refs;  // 只退出模型上下文,原档不删
    std::vector<std::string> retained_message_refs;
};

// 时间线一格:消息或压缩标记(seq 升序合流,与 v3 投影同序;压缩标记插
// 在 applied 的发生位置,不挪到别处伪造顺序)。
struct RestoredHistoryItem {
    enum class Kind { Message, Compact };
    Kind kind = Kind::Message;
    std::uint64_t seq = 0;
    std::string timestamp;
    RestoredMessageView message;  // kind=Message
    RestoredCompactView compact;  // kind=Compact
};

struct RestoredHistoryView {
    std::string session_id;
    std::string source_jsonl;  // v3 账路径(诊断/详情档按需再读)
    std::vector<RestoredHistoryItem> items;
};

// v3 账(sessions/<id>/<id>.jsonl)→ 显示 DTO:ReadV3Ledger 验卷 +
// ProjectHistoryTimeline 状态标志 + FoldToolActions 的调用配对(assistant
// 调用块的 provider 号换成 actionId,与 tool 消息同键)。工具配对键与
// 会话内 live 渲染无关——旧史一次性铺进滚动缓冲,不进 live 条目账。
//
// 读不动(验卷不过/非 v3 文件)给空 items:调用方按"没有可显示旧史"
// 处理,不冒充、不抛错(§4.10"源缺失时报告缺口,不假称齐全")。
RestoredHistoryView ProjectRestoredHistory(const std::filesystem::path& v3_jsonl);

// ---------------------------------------------------------------------------
// P3 第二棒:转录摘要行 + seq 游标分页(Ctrl+T 浮层与 app-server
// thread/read 共用的切片口径)。行是一次性渲染的派生物;翻页只切行,
// 不反复全量重读账本(§4.10"可缓存时间线……缓存须绑定源 hash")。
// ---------------------------------------------------------------------------

// 一行转录摘要:seq 是行的时间线身份(压缩标记也有 seq),游标翻页用。
struct RestoredTranscriptLine {
    std::uint64_t seq = 0;
    std::string text;
};

// 一页转录行(旧→新,时间线原序)。
struct RestoredTranscriptPage {
    std::vector<std::string> lines;
    bool has_older = false;  // 本页之外还有更旧行可取
    bool has_newer = false;  // 本页之外还有更新行可取
    // 本页边界行的 seq(继续翻页的游标);空页给空。
    std::optional<std::uint64_t> oldest_seq;
    std::optional<std::uint64_t> newest_seq;
};

// 时间线 → 摘要行(seq 升序)。行规(§4.10/§4.28):
//   - 消息行 "  <role> · <首行>":role 是四角色投影后的 user/assistant/
//     tool;被压缩的行注"已压缩",降档退链的原版注"已降档"——"哪段
//     已压缩、当前模型还能看哪段,界面要分得清"(§1.3);
//   - 压缩标记处插一行 "  ◆ 上下文已压缩:前 ~N → 后 ~M tokens":数字
//     读 compact.applied 持久字段,不重算(§4.11);没有数字给简版;
//   - hidden 默认不渲染、不报错(§4.28"隐藏不等于删除"):详情/开关
//     另算,行表里就是没有这行。
std::vector<RestoredTranscriptLine> RenderRestoredTranscriptLines(const RestoredHistoryView& view);

// seq 游标切页(lines 须 seq 升序;max_lines=0 不限):
//   - 两游标皆空 = 首开:取时间线尾页(最新 max_lines 行);
//   - before_seq:取 seq < before_seq 的最近 max_lines 行(向旧翻);
//   - after_seq:取 seq > after_seq 的最早 max_lines 行(向新翻)。
// has_older/has_newer 按剩余行如实算;边界游标取本页首末行。
RestoredTranscriptPage SliceRestoredTranscript(const std::vector<RestoredTranscriptLine>& lines,
                                                const std::optional<std::uint64_t>& before_seq,
                                                const std::optional<std::uint64_t>& after_seq,
                                                std::size_t max_lines);

}  // namespace lubancode::runtime
