// 自进化闭环阶段 1:EvolutionObservation——一条脱敏、可追根的经验。
//
// 契约(docs/features/evolution/README.md"采集边界")钉死的边界:
//   - 只产观察。不生成 Package,不决定晋升,不写候选区。
//   - 首版只收:/record 的 events.jsonl(目标口述、变量名、验收、最后验证)、
//     Workflow run(definition 元信息、events、终态)、/goal(objective、
//     iteration、evidence、终点判词)、ToolTrace(脱敏工具名、outcome、
//     error_code、短摘要)、项目 Memory 已接受条目。
//   - 首版不收:模型长篇思考原文;未脱敏的工具输入与输出;只有 HTTP 2xx
//     没有产物证据的"成功";模型自称"我学会了"的话;被拒 fingerprint
//     未变的同类建议。
//   - 模型思考原文压根不进观察:适配器(adapters.hpp)只读各家账本的白名单
//     字段,会话消息正文(assistant thinking)一处都不读,单测钉死。
//
// 脱敏不另起炉灶:文本过 skills::RedactSecrets(录制件的打码器,journal 侧
// RedactJournalText 与它同规矩),JSON 过 skills::SanitizeToolInput——本模块
// 只做截长与拼装,密钥这道门仍是既有设施把守。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::evolution {

// ---------------------------------------------------------------------------
// 来源类型(线上是字符串,不是数字)
// ---------------------------------------------------------------------------

// 六路来源。user_feedback 首版没有独立账本(用户当场纠正散在会话消息里,
// Memory 的 feedback 条目只覆盖已接受的一部分)——枚举与序列化先留位,
// adapter 不硬造,见 adapters.hpp 的 TODO。
enum class ObservationSource { Run, Goal, Recording, ToolTrace, Memory, UserFeedback };
std::string ToString(ObservationSource source);
bool ParseObservationSource(const std::string& text, ObservationSource& out);

// outcome 只记事实分档,不替评测、批准下结论。
//   success  有可核验的正面证据(验证通过/run succeeded/goal achieved);
//   failure  明确失败(验证不过/run failed/工具 error_code/goal blocked);
//   partial  未跑完或中性(continue/needs_user/cancelled);
//   unknown  账本没有成败证据(memory 条目天然没有;goal 从未评估)。
enum class ObservationOutcome { Success, Failure, Partial, Unknown };
std::string ToString(ObservationOutcome outcome);

// ---------------------------------------------------------------------------
// 观察本体(观察账一行 = 一条)
// ---------------------------------------------------------------------------

// 证据引用:指回原始账,不抄正文。ref 是文件(UTF-8 路径)或"文件#定位",
// note 说这一引用在看什么(如"verification" manifest 终态")。
struct EvidenceRef {
    std::string ref;
    std::string note;
};

// 行 schema 1。字段:
//   id          obs-<16hex>,由 source+source_id 决定(确定性——重采同一条
//               账本 id 不变,账去重靠它);
//   source      六路来源之一;
//   source_id   稳定来源 ID:run-id / goal-id / 录制件目录名 / execution_id /
//               memory id。指回原始账的钥匙;
//   source_ref  原始账的文件(UTF-8),show 指着它追根;
//   summary     脱敏一句话摘要(人看);
//   outcome     四档事实;
//   fingerprint 同类经验指纹(口径见 ComputeFingerprint);
//   details     来源专属的结构账(全部脱敏,只装白名单字段);
//   evidence    证据引用列表;
//   created_at  来源账本的时间(不是采集时刻;没有就空串)。
struct EvolutionObservation {
    int schema = 1;
    std::string id;
    ObservationSource source = ObservationSource::Run;
    std::string source_id;
    std::string source_ref;
    std::string summary;
    ObservationOutcome outcome = ObservationOutcome::Unknown;
    std::string fingerprint;
    nlohmann::json details = nlohmann::json::object();
    std::vector<EvidenceRef> evidence;
    std::string created_at;
};

// 一条观察 -> 一行 JSON(不带换行符;落盘由 store 追加换行)。
std::string SerializeObservation(const EvolutionObservation& observation);

// 一行 JSON -> 观察。不是合法 JSON、schema 不是 1、缺 id/source/source_id,
// 给 nullopt——坏行/半截行调用方跳过,不废整账(观察账与各事件账同约定)。
std::optional<EvolutionObservation> ParseObservation(const std::string& line);

// ---------------------------------------------------------------------------
// 指纹:同类经验怎么算"同类"
// ---------------------------------------------------------------------------

// 指纹口径(哪条进、哪条不进——改口径就是换账本,慎动):
//
// 进指纹(经 NormalizeShapeText 归一后拼接):
//   recording:目标口述 + 验收口述 + 工具名序列(连续同名折叠,保序);
//   run:workflow_id + 终态 + 节点序列(node_started 的 node_id,连续同名折叠);
//   goal:objective(最新 revision)+ 最终判词 decision;
//   tooltrace:工具名 + error_code(空则用 outcome);
//   memory:kind + 标题。
//
// 不进指纹(它们是"哪一次",不是"哪一类";塞进去同形任务就对不上号):
//   一切时间戳、路径、cwd、session/run/execution/goal 的 id、变量值、入参
//   原文与输入形状、effective_input_sha256、duration、字节数、token、模型名、
//   provider、账号、机器名。日期/URL/绝对路径在归一化里抽象成占位符。
//
// fingerprint = "fp-" + sha256("v1|<source>|<shape>")[0..16]。
// v1 是口径版本:口径变了换 v2,老账的指纹自然失配,不混算。
std::string ComputeFingerprint(ObservationSource source, const std::string& shape_text);

// 观察条目形态归一(指纹的原料,纯函数):
//   先过 skills::RedactSecrets(纵深:指纹是哈希,本不含明文,这一道防的是
//   归一化日志/调试输出侧漏),再 ASCII 小写、按空白切词,逐词认:
//     http(s)://…            -> <url>
//     盘符:\…、\\…、/… 开头    -> <path>(绝对路径)
//     yyyy[-/]m[-/]d、8 位纯数字 -> <date>
//   其余词原样(数字保留:重试次数这类语义量算"同类"的一部分)。
//   词间单空格,截 2000 字符。
std::string NormalizeShapeText(const std::string& text);

// ---------------------------------------------------------------------------
// 身份与脱敏
// ---------------------------------------------------------------------------

// 稳定观察 id:obs- + sha256("<source>|<source_id>")[0..16]。
std::string MakeObservationId(ObservationSource source, const std::string& source_id);

// 文本脱敏进观察:skills::RedactSecrets 之后截 cap 字符。摘要、口述、判词
// 一律过这一口,不直接搬账本原文。
std::string SanitizeObservationText(const std::string& text, std::size_t cap = 400);

// JSON 脱敏进观察:直接委托 skills::SanitizeToolInput(密钥键打码 + 字符串
// 值再过 RedactSecrets)。本函数只是 evolution 侧的窄口,规矩不另立。
nlohmann::json SanitizeObservationJson(const nlohmann::json& value);

// "yyyy-mm-dd HH:MM:SS"(本地时区);ms<=0 给空串。goal/trace 的 epoch 毫秒
// 落观察账时用。
std::string FormatEpochMsLocal(std::int64_t epoch_ms);

}  // namespace lubancode::evolution
