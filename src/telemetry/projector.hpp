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

}  // namespace lubancode::telemetry
