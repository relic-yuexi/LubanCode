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
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::agent {
class ContextArtifactStore;
}

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
// 无损结构压缩(只改工作视图)——"首次定形,epoch 内不追改"
// ---------------------------------------------------------------------------

struct StructuralCompressionOptions {
    // 超过这个字节数的工具结果,首次进请求视图就换成 artifact 引用(头尾
    // 预览)——要么首次就预览,要么这个 epoch 一直全文,不能半路变脸
    // (半路变脸就是追改已发前缀,前缀缓存守恒单第六期)。
    std::size_t long_result_bytes = 8192;
    // 换 artifact 引用时保留的头/尾预览字节数。
    std::size_t preview_bytes = 256;
    // 短于这个字节数的结果不做去重/外置——换引用的标注本身就不止这几个
    // 字节,压了反而更长。
    std::size_t min_compressible_bytes = 512;
    // 旧字段,已由"首次定形"规则取代(不再有热区豁免:热时全文、冷后换
    // 预览正是追改来源)。保留字段只为配置兼容,解析照收、行为不再看它。
    bool protect_hot_zone = true;
};

struct StructuralCompressionStats {
    std::size_t duplicate_groups = 0;          // 精确重复组数(同键同 hash)
    std::size_t duplicate_saved_bytes = 0;     // 去重省下的字节数
    std::size_t superseded_observations = 0;   // 同键新版出现(旧版不追改,只记数)
    std::size_t superseded_saved_bytes = 0;
    std::size_t offloaded_results = 0;         // 长结果换成 artifact 引用数
    std::size_t offloaded_saved_bytes = 0;
    std::size_t events_total = 0;              // 事件账总事件数

    std::size_t reclaimable_bytes() const {
        return duplicate_saved_bytes + superseded_saved_bytes + offloaded_saved_bytes;
    }
};

// 一枚 tool result 的"首次定形"决策。决策在一枚结果第一次进入请求视图时
// 做出,此后本 epoch 内不再改——追改已发前缀就是断缓存,老账递给模型后
// 不得涂改(agent/prefix.hpp 的追加律)。
enum class ResultViewKind {
    Full,          // 原文全文(短结果的默认归宿)
    Artifact,      // 超长,首次即换成 artifact 引用(头尾预览)
    DuplicateRef,  // 同键同 hash 的后来者,自述"与事件 eN 相同"
    NewVersion,    // 同键不同 hash 的新版本,自述"替代事件 eN",原文照发
    // L2 microcompact(第三期):冷区 artifact 换成 cheap 生成的局部语义
    // 摘要(带 source refs;原文仍在仓里,可 context_read 追回)。决策由
    // ApplyMicrocompactSummaries 显式改写——这是比"首次定形"更高一层的
    // 收拾动作,视图变化记 epoch 断因(microcompact),不冒充无事发生。
    MicrocompactSummary,
};

struct ResultViewDecision {
    ResultViewKind kind = ResultViewKind::Full;
    std::string ref_event_id;   // DuplicateRef/NewVersion 指到的那枚事件
    std::size_t seen_count = 1; // DuplicateRef:同键同 hash 累计出现次数
    // Artifact(第二期):落盘成功后记稳定 artifact_id("a0007")与 sha256
    // 短指纹(12 hex)——视图渲染与 memo 重放都用它。空 = 没落盘(无仓或
    // 落盘失败,后者决策已退回 Full)。
    std::string artifact_id;
    std::string artifact_sha;
    // L2(第三期):摘要正文与产出模型(kind=MicrocompactSummary 时有效)。
    std::string summary_text;
    std::string summary_model;
};

// 决策台账:tool_use_id -> 首次定形的决策。AgentLoop 每个 epoch 持一份
// (ReplaceHistory/compact 开新 epoch 时清空);不带记忆的调用(单测、
// /compact --dry-run 的"若现在从头压"估算)现场建一份新的即可。
struct ResultViewMemo {
    std::map<std::string, ResultViewDecision> decisions;
};

// 结构压缩:返回发给模型的工作视图。原 history 不动、消息条数不变、块序不
// 变;每枚 tool_result 按首次定形的决策渲染:
//   - 精确重复:"[已收敛:与事件 e12 的结果完全相同(read_file),累计出现 3 次;全文在会话存档]"
//   - 新版本:  "[此读取替代事件 e7 的旧版本]\n" + 原文(超长则 artifact)
//   - 超长:    "[artifact e123 · sha=... · 12345 字节 · 头部预览… 尾部预览…]"
// 硬规矩(前缀缓存守恒单第六期):后来者只自述,绝不回头改早先事件的
// 表示——e7 不补 superseded,重复不拆第一份。stats 只记本次新做的决策
// (memo 命中的旧决策不再计)。
std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats, ResultViewMemo& memo);

// 便捷重载:现场建一份新 memo,等于"若现在从头定形"的一次性视图
// (/compact --dry-run 的估算、单测用)。
std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats);

// 第二期(可追回 artifact):带仓的定形。新事件判成 Artifact 时先走仓的
// 原子落盘(blob -> chunks -> index),成功把 artifact_id 记进决策、视图
// 渲染带稳定 id 与检索指引;失败(仓没开/磁盘错/hash 不合)决策退回
// Full——内存全文照旧发送,绝不换成空引用(规格"原文不丢")。卸载对
// 同 tool_use_id 幂等,compact/重开 epoch 后重放不重复落盘。
std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats, ResultViewMemo& memo,
                                              ContextArtifactStore* store);

// 内容指纹:FNV-1a 64,十六进制 16 位。不引加密库——指纹只用来判"完全相同"
// 与做引用锚点,不做安全用途。
std::string Fingerprint64(const std::string& content);

// 热区起点:最后一条"用户文本输入"消息的下标;没有这样的消息给 0。
std::size_t HotZoneStartIndex(const std::vector<api::Message>& history);

}  // namespace lubancode::agent
