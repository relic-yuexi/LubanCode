// 规范化事件账与无损结构压缩(0.27.x 分层压缩第二期)。
//
// 现有 api::Message 适合上 wire,不适合判断哪些内容重复。这里从历史派生
// 一层中立账(不替换 JSONL、不落盘):每个 tool use/result 原子对是一枚
// ToolExchange 事件,带稳定 event_id("e<序号>"——账从同一份历史重算,前缀
// 不变则 id 不变)与内容指纹。结构压缩只改"发给模型的视图"(把重复/过期/
// 超长的工具结果正文换成引用与预览),不动 AgentLoop 的活历史,更不动
// session JSONL——全量真账一字不丢,需要核查时从原账取回。
//
// 硬规矩:
//   - 去重只走精确键 + 精确内容 hash,不走相似度;相似只能帮忙找候选,
//     不能判等(第一期不上 LSH)。
//   - 副作用工具(run_command、web_fetch/web_search 等)不参与去重——命令
//     文本一样不代表这次可以不跑,cwd/环境/文件树/时间都可能变。这里的
//     压缩本来就只改展示视图、绝不跳执行,不认它们的键是不给"看走眼"
//     留门。
//   - 文件读取按"同键同 hash"判重;文件改版(hash 变了)绝不与新读取合
//     并,旧观察标 superseded(保头部预览),依赖旧版作出的决定(assistant
//     正文)原样不动。
//   - tool_use/tool_result 是原子组:压缩只重写 tool_result 的 content 字符
//     串,消息条数与块序一概不碰,配对天然不破,三 wire 都不生非法形状。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// 规范化事件账
// ---------------------------------------------------------------------------

enum class NormalizedEventKind {
    UserText,        // 用户文本输入(一轮的开头)
    AssistantText,   // 助手正文/思考
    ToolExchange,    // tool use + tool result 原子对
    Other,           // 其余(纯工具结果消息的孤儿块、空消息等)
};

struct NormalizedEvent {
    std::string id;                     // "e0","e1",... 同一份历史重算稳定
    NormalizedEventKind kind = NormalizedEventKind::Other;
    std::size_t message_index = 0;      // 来源消息下标(history 内)
    // ToolExchange 专属:
    std::string tool_name;              // "read_file" / "search" / ...
    std::string tool_use_id;
    nlohmann::json tool_input;
    std::string result_content;         // 配对 result 的正文(没配上给空)
    bool result_is_error = false;
    std::size_t result_message_index = static_cast<std::size_t>(-1);  // result 所在消息;未配对 = -1
    std::string dedup_key;              // 规范键;空 = 不参与判重(副作用工具等)
    std::string content_hash;           // result 正文的 FNV-1a 64 指纹(十六进制)
};

// 从历史派生事件账。逐消息扫:assistant 消息里的每个 ToolUseBlock 与其后
// 第一条 user 消息里同 tool_use_id 的 ToolResultBlock 配成一枚 ToolExchange;
// 配不上(打断/恢复前的孤儿,理论上有 RepairToolPairs 兜底)按 Other 记,
// 不丢消息。
std::vector<NormalizedEvent> BuildEventLedger(const std::vector<api::Message>& history);

// ---------------------------------------------------------------------------
// 无损结构压缩(只改工作视图)
// ---------------------------------------------------------------------------

struct StructuralCompressionOptions {
    // 超过这个字节数的冷区工具结果,正文换成 artifact 引用(头尾预览)。
    std::size_t long_result_bytes = 8192;
    // 换 artifact 引用时保留的头/尾预览字节数。
    std::size_t preview_bytes = 256;
    // 短于这个字节数的结果不做去重/外置——换引用的标注本身就不止这几个
    // 字节,压了反而更长。
    std::size_t min_compressible_bytes = 512;
    // 热区保护:最后一条用户文本输入(含)之后的消息不碰——那是模型正
    // 盯着看的最近上下文。true = 按这个规矩保护(默认)。
    bool protect_hot_zone = true;
};

struct StructuralCompressionStats {
    std::size_t duplicate_groups = 0;          // 精确重复组数(同键同 hash)
    std::size_t duplicate_saved_bytes = 0;     // 去重省下的字节数
    std::size_t superseded_observations = 0;   // 被新版本覆盖的旧文件读取数
    std::size_t superseded_saved_bytes = 0;
    std::size_t offloaded_results = 0;         // 长结果换成 artifact 引用数
    std::size_t offloaded_saved_bytes = 0;
    std::size_t events_total = 0;              // 事件账总事件数

    std::size_t reclaimable_bytes() const {
        return duplicate_saved_bytes + superseded_saved_bytes + offloaded_saved_bytes;
    }
};

// 结构压缩:返回发给模型的工作视图。原 history 不动、消息条数不变、块序不
// 变,只把冷区里可收的 tool_result.content 换成:
//   - 精确重复:"[已收敛:与 e12 相同(read_file src/a.cpp),累计出现 3 次]"
//   - 被覆盖:"[已收敛:此版本其后已改版,最新读取见 e45;头部预览…]"
//   - 超长:  "[artifact e123 · sha=... · 12345 字节 · 头部预览… 尾部预览…]"
// stats 填实际回收量;/compact --dry-run 与观测直接读它。
std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats);

// 内容指纹:FNV-1a 64,十六进制 16 位。不引加密库——指纹只用来判"完全相同"
// 与做引用锚点,不做安全用途。
std::string Fingerprint64(const std::string& content);

// 热区起点:最后一条"用户文本输入"消息的下标;没有这样的消息给 0。
std::size_t HotZoneStartIndex(const std::vector<api::Message>& history);

}  // namespace lubancode::agent
