// /prompt audit(Token 账本单 A3):把 PromptManifest 的段级账与真实运行
// 信号摆给用户——prompt 哪里费劲、哪些段常驻哪些可省的事实账。只摆事实
// 不说教,主人裁决。
//
// 形态(§10.2):
//   /prompt audit static               审眼前配置(不碰 Journal)
//   /prompt audit runtime [session-id] 审真实请求(manifest/prefix/cache)
//   /prompt audit outcome [--since 30d] 审结果信号(多 session)
//   /prompt audit all                  三层一起
//   /prompt audit explain <finding-id> 一条 finding 的全账(证据/反证/规则)
//   /prompt audit --json               JSON 输出(lubancode.prompt.audit v1)
// 现有 /prompt 裸敲与 reset 行为不动(prompt_commands 只加分派壳)。
// --model-review 属 A6,敲了明说后续批次,本地检查照跑。
//
// 口径戒律:
//   - 报告不含 prompt 正文;证据只落 hash/token/计数/段名/事件引用;
//   - Journal 不可得时:static 照常(不碰 Journal),runtime/outcome
//     明说"账不可得",不猜;
//   - 语义类检查只给 suspected(confidence=low),复核才作数。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/commands/command_flow.hpp"
#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"
#include "insights/feature_signal.hpp"
#include "insights/prompt_auditor.hpp"
#include "insights/session_analyzer.hpp"

namespace lubancode::agent {
struct PromptOptions;
class Tool;
}  // namespace lubancode::agent

namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::cli {
class ContextTracker;
}

namespace lubancode::app {

// ---------------- 纯函数(单测钉住) ----------------

// /prompt audit 的二级参数拆词(纯解析)。
struct ParsedPromptAuditCommand {
    enum class Mode { Invalid, Static, Runtime, Outcome, All, Explain };
    Mode mode = Mode::Invalid;
    bool invalid = false;     // 认不得的词/缺的参(与 bad_word 配对)
    bool json = false;
    std::string session_id;   // runtime 的可选目标场
    int since_days = 30;      // outcome/all 的时间窗
    std::string finding_id;   // explain 的目标
    std::string bad_word;     // 认不得的词/缺的参
    bool later_model_review = false;  // --model-review(A6 后续批次,明说)
};
ParsedPromptAuditCommand ParsePromptAuditCommand(const std::string& args);

// 报告模型(渲染的唯一入参;handler 装好,渲染零 IO)。
struct PromptAuditReportModel {
    std::string mode;  // static/runtime/outcome/all/explain(再跑哪层)
    // static 面的事实账。
    insights::PromptAuditFacts facts;
    bool has_static = false;
    // 三层 finding 合账(static → runtime → outcome,次序稳定)。
    std::vector<insights::Finding> findings;
    // runtime 面。
    std::string session_id;
    bool provisional = false;
    std::vector<insights::RuntimeRequestView> requests;
    // outcome 面(coverage 单列:active/incomplete/corrupt 不混)。
    bool has_outcome = false;
    std::vector<insights::WorkspaceScanEntry> scan;
    std::map<std::string, std::int64_t> status_counts;
    std::int64_t sessions_found = 0;
    std::vector<insights::FeatureSignal> signals;
    std::vector<std::string> warnings;
};

// 终端人话。行不带换行符。
std::vector<std::string> FormatPromptAuditReport(const PromptAuditReportModel& model);

// JSON 输出(schema: lubancode.prompt.audit v1)。content_policy 钉
// metadata_only;prompt_text_included 恒 false(隐私合同自描述)。
nlohmann::json BuildPromptAuditJson(const PromptAuditReportModel& model);

// ---------------- 执行(IO) ----------------

// 会话侧材料包(handler 从 SlashDispatchContext 装好)。
struct PromptAuditContext {
    const lubancode::cli::Theme& theme;
    // static 的拼装现场(nullptr = 没接,static 明说没有)。
    const lubancode::agent::PromptOptions* prompt_options = nullptr;
    std::string prompts_dir;
    std::string model_instructions;
    std::string soul_text;        // 已剥注释
    std::string soul_name;
    std::int64_t context_budget_tokens = 0;  // 0 = 未知
    // 工具面(nullptr = 没接,工具类检查明说没有)。
    const lubancode::tools::ToolRegistry* registry = nullptr;
    // runtime/outcome 的账(nullptr = 账未开)。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
    std::filesystem::path sessions_root;
    // 时钟注入(outcome 的 --since 窗;空 = now)。
    std::string now_yyyymmdd;
};

// /prompt audit 主入口(IO 侧):解析 -> 装报告模型 -> 渲染/JSON。
void HandlePromptAuditCommand(const std::string& args, const PromptAuditContext& context);

// 命令分派注册制:/prompt audit 的分派位(prompt_commands 的壳剥掉
// "audit" 前缀后把余参递到这;audit_args 不含 "audit" 一词)。
struct SlashDispatchContext;
CommandFlow HandleSlashPromptAudit(SlashDispatchContext& ctx, const std::string& audit_args);

}  // namespace lubancode::app
