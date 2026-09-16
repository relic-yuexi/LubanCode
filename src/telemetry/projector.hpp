// 纯 TelemetryProjector(端云协同可观测架构与 Telemetry 插件设计单 §14/
// §11,实施分期 T0"写纯 TelemetryProjector,只吃 golden Journal")。
//
// 投影规矩(§10.1 三不/§14):
//   - 不嵌 Recorder 锁、不联网、不读墙钟、不随机发号——同一 Journal、
//     同一 projection_key、同一 projector version,重放两次输出逐字节
//     相同(T0 验收线)。
//   - 只读 canonical Journal 文件;坏链/坏行停整条 stream 并报
//     telemetry.source_corrupt,不跳过坏行接着猜(§22.5)。
//   - span 映射照 §11.2 表:run/turn/gen_ai.request/tool.execute/
//     approval.wait/compact/verification;model.request.prepared 只作
//     属性材料,不另开 span;usage 归 request span 属性与 metric。
//   - 投影器产 D1 metadata:属性只从 §11.4 的封闭键集挑值,不碰正文;
//     出厂前过 Redactor 二道门。
//   - cursor/spool/export ACK 是 T1 的账,本件不落任何文件。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/redactor.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancode::telemetry {

struct ProjectorOptions {
    // 本地投影密钥:trace/span id 派生的 HMAC key(§9.3)。测试用固定假
    // 钥匙;真机归 T1 TelemetryService 持有,不进日志与 spool。
    std::string projection_key;
    // resource attributes 的装配层输入(§10.1)。
    ResourceInputs resource;
    // 数据档:T0 只走 D1(Diagnostic/Content 属后续批次,给了也按 D1 裁)。
    DataClass data_class = DataClass::Metadata;
};

struct ProjectionReport {
    bool ok = false;
    std::string error_code;  // 空 = 成功;telemetry.source_corrupt /
                             // telemetry.contract.* / telemetry.io_error
    std::string message;
    std::uint64_t events_projected = 0;
    std::string trace_id;  // 本 stream 的 trace id(空 = 没投影出来)
    std::string workspace_key;
    std::string session_id;
    std::string run_id;
    std::vector<TraceSpan> spans;       // 按起事件 id 稳定排序
    std::vector<MetricSample> metrics;  // 按 (name, labels) 稳定排序
    nlohmann::json resource_attributes;  // 已过 Redactor
    RedactionManifest redaction;         // 各 span/resource 的合并账
    std::vector<std::string> warnings;   // 悬空收口/迟到 usage 等(稳定序)

    nlohmann::json ToJson() const;  // 诊断/fixture 用
};

// 投影一份 Journal 文件(main/subagent stream 同形)。文件不存在/读不了
// 报 telemetry.io_error;验账不过报 telemetry.source_corrupt。
ProjectionReport ProjectJournalFile(const std::filesystem::path& stream_path,
                                    const ProjectorOptions& options);

// ---------------------------------------------------------------------------
// v3 半场(T07 / V3-GAP-02,SessionV3 旧设计清理单):session 账投影。
//
// 输入是 ReadV3Ledger 已验卷的账(坏账在读取层已拒,本件不做 IO)。映射
// 合同(单内 T07"Session/turn/request/tool/compact/Hook 到 span 的映射"):
//   session  session.started → session.ended;closeQuality clean=Ok、
//            incomplete=Error。attr lubancode.run.kind 取 payload.runKind
//            (子账不带 runKind 就省略,不暗填)。
//   turn     首条携带该 turnId 的行(消息或事件)开 span。v3 无 turn 终态
//            事件——一律按 terminal=missing 收口(partial),不用下一回合
//            的记录时间猜完整时长。compact/goal/memory 内部回合各有
//            turnId,各开各的 span;parent 沿 parentTurnId 挂主回合。
//   request  model.request.sent → model.response.completed/failed/cancelled
//            (model.request.failed = 传输层失败终态)。v3 每请求唯一
//            requestId、无 attempt 维度,不伪造 attempt 属性。usage 唯一
//            可累计 owner 是 assistant message(§五,与 T06 ProjectV3Usage
//            同源同键);model.usage.appended 只作观察警告,不二次累计;
//            缺实报 coverage=unknown,不写 0。
//   tool     tool.execution.started → finished/failed/cancelled/rejected/
//            unknown(按 actionId;重试 attempt 先收旧 span 再开新)。缺
//            started 的终态:span 锚在终事件上、时长 0、terminal=
//            missing_start,不拿 pending 时间猜时长。
//   compact  compact.requested → applied/failed/cancelled/rejected(按
//            compactId;requested 缺席时从 started 起锚)。
//   hook     hook.dispatch.requested → 本 dispatch 最后一枚 invocation 终态
//            (completed/failed/cancelled/unknown,按 hookDispatchId;洋葱
//            串行 invocation 的最后一枚终事件为 dispatch 终点)。hook.
//            skipped 只计数不开 span。
// title/verification/approval(T11 域)无 v3 span 材料——不伪造 span,
// 待各域发行后补映射并升投影版本。
//
// 确定性与红线同 v2 半场:id 由 identity 层 HMAC 派生、时间全取行内
// timestamp(单调钟 v3 无,记 0 不猜)、输出按锚行 seq 稳定排序;属性只
// 从封闭键集取值,不碰 message/event 正文;出厂前过 Redactor 二道门。
// ---------------------------------------------------------------------------
ProjectionReport ProjectV3LedgerFile(const trajectory::v3::V3Ledger& ledger,
                                     const ProjectorOptions& options);

}  // namespace lubancode::telemetry
