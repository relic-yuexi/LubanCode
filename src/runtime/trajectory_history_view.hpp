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
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::runtime {

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

}  // namespace lubancode::runtime
