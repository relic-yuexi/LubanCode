// Shared synchronous Memory extraction; candidates only, no host scheduling.
#pragma once
#include <atomic>
#include <cstddef>
#include <expected>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/model_router.hpp"
#include "agent/sample_model.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
namespace lubancode::agent::memory_extraction {
inline constexpr int kMemoryExtractMaxTokens = 4096;

// 抽取请求的本地超时预算(秒):SampleModel 的看门狗到点归因
// local_deadline(DeadlineTimeout),不是网络错误。原先住在
// commands/memory_commands.cpp 的匿名段,回合总结异步化单起公开——后台
// 执行器与收账点的文案同用一把尺。
inline constexpr int kMemoryExtractTimeoutSecs = 45;

// 抽取结果(回合总结 + 候选 + 检索扩展词)。
struct ProposedCandidate {
    std::string kind;         // fact | preference | feedback
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string confidence;   // user-stated | verified | inferred
    std::string occurred_at;  // 事件发生时间:材料里明确给出才填,否则空(不造假)
};

struct MemoryExtraction {
    std::string task_type;    // code | research | config | docs | other
    std::string summary;
    std::vector<std::string> retrieval_terms;
    std::vector<ProposedCandidate> candidates;
};

// ---------------------------------------------------------------------------
// 抽取失败的结构化错误(P0-A/P0-B):模型文本按不可信输入处理,语法/编码/
// 字段错都从抽取接口稳定返回,不许越过这层抛出。
//
// 旧账兼容:StableExtractErrorCode 的旧文案路把 syntax_invalid/utf8_invalid/
// schema_invalid 一律记 parse_failed——离线重放旧账时,parse_failed ≈ 这三
// 类的统称;新账各记各名,route_miss/empty_output 口径不变。
// ---------------------------------------------------------------------------
enum class ExtractionErrorCode {
    SyntaxInvalid,    // JSON 语法坏:未转义引号、漏逗号、多对象歧义、半截对象
    Utf8Invalid,      // 响应正文不是合法 UTF-8(先验整段,再谈语法)
    SchemaInvalid,    // 语法过了,字段合同不过:缺必填/null/数字/数组/顶层数组
    OutputTruncated,  // provider 结束原因报长度截断(max_tokens/length 一族)
    EmptyOutput,      // 采样"成功"但正文为空
    TransportFailed,  // 发送失败/流内错/看门狗取消
    // 本地超时预算到点(取消误报 ESC 单 Bug 1):采样层的 local_deadline
    // 稳定码在这里立名——与 transport_failed 分开数,离线才知道"慢死"与
    // "网死"各占多少;终端提示带预算,不冤枉用户按键。
    DeadlineTimeout,
    RouteMiss,        // cheap 路由找不到 provider(旧稳定码 route_miss)
    WorkerStartFailed,  // Host accepted extraction, but its worker was not created.
};
const char* ExtractionErrorCodeName(ExtractionErrorCode code);

// 一次抽取失败的完整账。message 是终端可直出的短文案(自证合法 UTF-8,
// 不含库异常的 last read 片段);诊断字段只进日志与轨迹,查原文复用受控
// 轨迹,不在错误里转储正文。
struct ExtractionError {
    ExtractionErrorCode code = ExtractionErrorCode::SyntaxInvalid;
    std::string message;
    // ---- 诊断(P0-A) ----
    std::string request_id;               // provider 外部号(空 = 没回)
    std::size_t body_bytes = 0;           // 响应正文总字节
    std::size_t error_offset = static_cast<std::size_t>(-1);  // 原文字节偏移;-1 = 不适用
    bool utf8_valid = true;               // 整段 UTF-8 预检结果
    std::string stop_reason;              // provider 结束原因(空 = 未报告,单列诊断)
    std::string field_path;               // schema_invalid 时的字段路径(如 candidates[0].kind)
    std::string schema_check_error;       // SampleModel output_schema 复检账(空 = 没设或过了)
};
inline constexpr std::size_t kExtractionNoOffset = static_cast<std::size_t>(-1);

// 抽取输出预算(P1-A):候选正文与写路同款上限(kMaxTopicBytes,8 KiB)对齐,
// 超长候选整条跳过——先减冗长输出,不动请求的 max_tokens。
inline constexpr std::size_t kMaxCandidateContentBytes = 8 * 1024;

// 任务类型判定(用户基调 1:先推测目的再选总结提示词)。纯词法启发,不
// 打请求;user_text 是本轮用户消息,tool_names 是本轮调用过的工具名。
std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names);

// 本地压缩本轮材料：用户正文 2 KiB、最终助手答复 3 KiB、最近六次工具
// 摘录共 2 KiB。工具参数只留定位字段，不发补丁/文件正文。忽略中间答复、
// 思考和图片；max_bytes 是整段转写的严格字节上限，不含系统提示词。
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes);

// 抽取提示词:基础契约(features/memory-summary-base.md)+ 分型侧重
// (features/memory-summary-<type>.md),用户目录可覆盖。task_type 认不出
// 时用 other。
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type);

// 解析模型输出。先验整段 UTF-8,再按明确规则收 JSON:纯 JSON、单层代码
// 围栏、无歧义的前后说明(首个 { 之前不含 {,配对 } 之后无剩余内容);多
// 对象、字符串外悬空花括号、半截对象一律拒绝,不选一个碰运气。字段合同
// (P0-B):顶层必须 object;task_type/summary 必填 string(summary 非空);
// 已声明字段类型错(null/数字/数组/对象)拒绝整次并在错误里带字段路径;
// 无效业务候选(kind 不在枚举、title/content 空、正文超预算)沿既有规则
// 跳过该条,禁止静默类型转换。候选最多 3 条。
std::expected<MemoryExtraction, ExtractionError> ParseExtractionJson(const std::string& text);

// 发一次抽取请求(同步,带看门狗取消)。失败只返回错误,调用方降级。
// reasoning_effort 非空时随请求带上(cheap 路由的档位);accounting 非空时
// 把这次调用的 usage/时长记进去(分角色记账,不混普通 turn 的账)。
// cancel/boundary_recorder 是回合总结异步化单添的口:外部取消链(会话
// 拆除/换代的 RequestCancel)与旁路桥(轨迹 Journal 的
// purpose=memory_extract 落账)原先只在同步前台路拼,后台执行器同一条
// 路也要走——默认空,旧行为不变。
// 采样走 agent::SampleModel 原语(批一·病四)。
std::expected<MemoryExtraction, ExtractionError> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort = std::string(),
                                                                 agent::BackgroundCallAccounting* accounting = nullptr,
                                                                 agent::LoopBoundaryRecorder* boundary_recorder = nullptr,
                                                                 const std::atomic<bool>* cancel = nullptr);

// 采样结果的抽取侧收口:RunMemoryExtraction 与走 ModelRouterService::Sample
// 一站的调用方共用——失败回 transport_failed、空文回 empty_output、已知
// 截断结束原因(max_tokens/length 一族)回 output_truncated,成功交解析。
// 结束原因未知/缺失不据此判死:照走解析,诊断里单列 stop_reason 原值。
std::expected<MemoryExtraction, ExtractionError> FinishMemoryExtraction(const agent::SampleResult& sampled);

// 抽取输出的字段合同(SampleModel.output_schema 本地复检用,与
// ParseExtractionJson 的判型同一份合同;候选内部字段的合同在解析函数里
// 显式判型——公共校验器不递归嵌套)。设这份不等于 provider 结构化输出
// 接通:api::Request 没有 output_schema 字段,wire 侧约束待批六再议。
const nlohmann::json& MemoryExtractionOutputSchema();

}  // namespace lubancode::agent::memory_extraction
