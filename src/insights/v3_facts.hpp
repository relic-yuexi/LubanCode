// Insights 的 v3 领域读模型(T14/细化单 §一"建立领域读模型,由 v3 已
// 验证事实投出")。
//
// IntegrityGate 验过的 v3 树(主账 + 递归子账)在这里折成一份纯事实:
// 每笔模型请求的 prepared 引用(systemMessageRef/inputMessageRefs/
// contextRevision)、实际发送视图指纹(V3-REAL-06 的 inputView)、终态与
// usage owner(schema §五:assistant message.usage 唯一累计)、工具操作
// 折叠快照(FoldToolActions)与失败明细。prompt 审计、摩擦分类与
// session 分析只吃这份,不再各扫 JSONL,也不从最终聊天显示倒推请求。
//
// 口径铁律(细化单 §三):
//   - 三种视图不混:单次请求按当次 prepared 引用,不拿当前 contextChain
//     替代历史请求;统计全部历史走账面事实;
//   - 缺件如实:nullopt/空/unknown,不补 0、不猜、不借当前环境;
//   - prepared 引用 ≠ 供应商最终 wire:wire_message_count/divergent 只是
//     发送视图指纹账,正文不在手,报告措辞限定为"持久请求视图";
//   - 跨账身份带 session_id;evidence 锚 event_id + seq(§3.1 五键材料
//     由 ReadV3Ledger 验卷保证);
//   - 纯读:不开写柄、不调模型、不重跑工具。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/reader.hpp"

namespace lubancode::insights {

// 一笔模型请求的 v3 事实(model.request.prepared 及其终态/owner 折出)。
struct V3RequestFacts {
    std::string session_id;  // 所属账(主账/子账;跨账身份)
    std::string run_id;
    std::string request_id;
    std::string turn_id;
    std::string step_id;
    std::string purpose;  // 线上名;prepared 缺 purpose 键 = "unknown"
    std::string event_id;  // prepared 事件 id(evidenceRef 锚)
    std::uint64_t seq = 0;  // prepared 行 seq(报告跳转锚)
    std::uint64_t context_revision = 0;  // prepared 落点 revision
    std::string system_message_ref;      // 当次请求的 system 引用
    std::vector<std::string> input_message_refs;  // 单次请求输入(§4.1 行 3)
    std::optional<std::string> provider;
    std::optional<std::string> wire;
    std::optional<std::string> model;
    // inputView(V3-REAL-06 实际发送视图指纹):缺席 = 该请求无此账,如实缺。
    std::optional<std::uint64_t> wire_message_count;
    std::optional<bool> wire_view_divergent;  // 链引用数 ≠ 发送消息数
    std::optional<std::string> system_fingerprint;
    // 工具名表(prepared 快照):tool_names_recorded=false = 该请求没记
    // 工具面(不是"零工具"),消费方不得据此判"工具集没变"。
    bool tool_names_recorded = false;
    std::vector<std::string> tool_names;
    // token 估算引用(§4.36):prepared 只引用不内联;缺席如实,不造数。
    bool has_token_estimate = false;
    // 发送与终态(按事件族;无 = 未发生,不是成功)。
    bool sent = false;             // model.request.sent 在账
    std::string outcome;           // ""|"completed"|"failed"|"cancelled"
    std::string outcome_event_id;  // 定下 outcome 的那行事件(证据锚)
    std::uint64_t outcome_seq = 0;
    // usage owner(schema §五):assistant message.usage 唯一可累计;
    // model.usage.appended 只是迟到/失败观察,不进 owner(消费方另栏)。
    bool usage_reported = false;
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;  // 已含在 output,拆账用
    std::optional<std::string> usage_owner_message_id;  // reported 时给 owner id
};

// 工具失败明细(摩擦取材;reason 只留前 80 字,不搬全文)。
struct V3ToolFailureFacts {
    std::string session_id;
    std::string action_id;
    std::string turn_id;
    std::string tool_name;  // 声明块名;取不到留空
    std::uint64_t attempt = 1;
    std::string event_id;
    std::uint64_t seq = 0;
    std::string phase;  // "execution"|"persist"(落盘失败)
    std::string reason;
};

// 一次工具操作的折叠快照 + 账归属(FoldToolActions 是 v3 reader 现成投影,
// 不另写一套)。
struct V3ToolActionFacts {
    std::string session_id;
    trajectory::v3::ToolActionSnapshot snapshot;
};

// 一场账(主或子)的读模型。
struct V3SessionFacts {
    std::string session_id;
    std::string run_id;
    std::filesystem::path jsonl_path;
    bool is_subagent = false;
    // WalkSessionTree 的关联判定(root/linked/spawned/cycle/child_missing/
    // unreadable/…)。
    std::string link_status;
    std::uint64_t terminal_seq = 0;
    std::string terminal_hash;  // 末行 lineHash(派生 stale 指纹)
    // v3 封口唯一事实:session.ended 在账(无 session.json 可翻)。
    bool sealed = false;
    std::vector<V3RequestFacts> requests;  // prepared 序(账面序)
    std::vector<V3ToolActionFacts> tool_actions;
    std::vector<V3ToolFailureFacts> tool_failures;
    // 该账缺口点名(missing_blob/link_status 异常/迟到 usage 观察…)。
    std::vector<std::string> notes;
};

struct V3FactsRead {
    bool ok = false;
    std::string error_code;  // facts.* 稳定码
    std::string message;
    std::string session_id;
    std::vector<V3SessionFacts> sessions;  // 主账在前,子账按树序
    std::vector<std::string> warnings;     // partial 点名(不致命,不弃整场)
};

// 认领一场 v3 session 目录(调用方先 ProbeV3SessionStream 判明 V3Stream)
// 并投出读模型。主账验卷不过 → ok=false(WalkSessionTree/ReadV3Ledger 的
// 错误码透传);子账坏/缺/环 → 该子账缺席 + warnings 点名,整场不弃
//(T14:partial 不跳整场)。artifact 缺 blob 按存在性实探记 notes(不做
// hash 全量校验,深查归 ExpandResultPreview)。纯读。
V3FactsRead CollectV3SessionFacts(const std::filesystem::path& session_dir,
                                  const std::filesystem::path& v3_stream);

}  // namespace lubancode::insights
