// 统一模型路由(渐进式上下文装载与 cheap/normal/lao 模型分工,第一期)。
//
// 三根梁:
//   1. 任务分三档角色——cheap(后台小活)/ normal(普通对话与实现)/
//      lao(计划与架构)。角色跟着 TaskKind 走,不跟 main/subagent 身份走:
//      子代理做计划也走 lao,main 触发后台摘要也走 cheap(规格"子代理仍
//      与 main 同级")。
//   2. 回退链钉死:effective_normal = normal ?? 会话 model;
//      effective_cheap = cheap ?? normal;effective_lao = lao ?? normal。
//      "空"包括字段缺失、空字符串、显式 null——读入后一律归一成"未配置",
//      不报错、不禁功能。
//   3. 路由看得见:每份 ModelRoute 带 source(人话来源)与 fell_back标记,
//      /model roles、/context 与 usage 台账都从这里翻账。
//
// 依赖方向:agent 层只认纯数据(api/types.hpp 的 Usage)与标准库,不牵扯
// config/——config 层解析三角色字段,app 层拿 Config 算出这份表。跨
// provider 的 backend 重建也归 app 层(ModelRouterService),这里只管
// "该用谁"的决策。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

// 三档逻辑角色。注意这是"任务阶段"的分工,不是 agent 身份的等级。
enum class ModelRole { Cheap, Normal, Lao };

// 任务种类。调用点必须登记明确的一种,不许凭 prompt 长短猜大模型。
//   NormalTurn        普通对话与实现、工具选择、编辑、运行、排错(main 与
//                     子代理的默认档——同级同路)。
//   Plan              Plan Mode 的需求澄清、架构权衡与完整计划(产品尚无
//                     Plan Mode,本枚举先钉口径,接线点到位即走 lao)。
//   Compact           /compact 与 auto compact 的 map/reduce 与默认终稿。
//   CompactRepair     compact 校验失败后的修补或回退(normal)。
//   Microcompact      冷区 tool result 的局部语义压缩(cheap)。
//   MemoryExtract     回合记忆候选抽取(cheap)。
//   RetrievalExpansion 检索扩展词(cheap;与抽取同轮产出时同路)。
//   Classification    低风险分类(cheap)。
//   SessionTitle      会话标题生成(cheap)。
//   ResumeSummary     resume 列表摘要(cheap)。
enum class TaskKind {
    NormalTurn,
    Plan,
    Compact,
    CompactRepair,
    Microcompact,
    MemoryExtract,
    RetrievalExpansion,
    Classification,
    SessionTitle,
    ResumeSummary,
    // 持久目标单:goal 终点判定的独立 evaluator 调用(无工具,另起 request)。
    GoalEvaluate,
};

std::string ToString(ModelRole role);
std::string ToString(TaskKind kind);

// 默认角色映射表(纯函数):哪种任务走哪档角色。调用方只报 TaskKind,
// 不自行拼 model 字符串(规格"调用点收拢")。
ModelRole DefaultRoleForTask(TaskKind kind);

// 一份解析后的完整路由。provider 留空 = 继承当前活跃 provider;跨
// provider 路由须重建对应 backend,不能只在当前 backend 上换一串 model 名。
struct ModelRoute {
    std::string provider;
    std::string model;
    std::string effort;  // 空 = 请求不带 reasoning_effort,维持现状
    std::optional<std::size_t> context_window;      // 空 = 未声明
    std::optional<std::size_t> max_output_tokens;   // 空 = 未声明
    // 这份值从哪一级来("/model roles" 的"来源"列):如"项目配置""当前会话"
    // "model_roles 段(项目级)"。诊断页显示原始来源,不猜。
    std::string source;
    // cheap/lao 未配置、回落到 normal 那份时为真——展示层必须写
    // "回落到 normal",不能把同名再印一遍让用户猜(规格"界面"节)。
    bool fell_back_to_normal = false;
};

// 解析前的一格角色配置:model 为空 = 该角色未配置(缺失/空串/null 归一)。
// shorthand(只填模型名的字符串字段)与 model_roles 高级段都折成这份。
struct ModelRoleSpec {
    std::string provider;
    std::string model;
    std::string effort;
    std::optional<std::size_t> context_window;
    std::optional<std::size_t> max_output_tokens;
    std::string source;  // 未配置时留空

    bool configured() const { return !model.empty(); }
};

// 三角色解析后的完整表。notices 攒冲突/回退提示,启动横幅与 /model roles
// 打给用户看(规格"路由看得见")。
struct ModelRouteTable {
    ModelRoute cheap;
    ModelRoute normal;
    ModelRoute lao;
    // compact_model 旧字段单写(cheap_model 未配置)时:只顶替 Compact/
    // Microcompact 任务的模型,不接管记忆抽取、标题、resume 摘要(规格
    // "旧字段只影响 compact,不该突然接管记忆和标题")。
    std::optional<ModelRoute> compact_legacy_override;
    std::vector<std::string> notices;

    const ModelRoute& RoleRoute(ModelRole role) const;
    // 按任务种类取路由:先过默认角色映射;Compact/Microcompact 且旧字段
    // 在场时优先旧字段(未配 cheap 的兼容期行为)。
    ModelRoute RouteFor(TaskKind kind) const;
};

// 回退链解析(纯函数,单测钉):
//   normal:spec 配了用 spec;没配用 session_model(来源"当前会话")。
//   cheap/lao:spec 配了用 spec;没配整体回落 normal(标记 fell_back)。
// session_model 为空(尚无会话模型)时,未配置角色给空 model——调用方
// 拿到空 model 应视为"本任务暂不可发",不许静默换成别的名字。
ModelRouteTable ResolveModelRoutes(const ModelRoleSpec& normal_spec, const ModelRoleSpec& cheap_spec,
                                   const ModelRoleSpec& lao_spec, const std::string& session_model,
                                   const std::string& active_provider);

// ---------------------------------------------------------------------------
// usage 分角色记账(规格"路由看得见":每次后台模型调用都记任务种类、
// 角色、模型、token 与耗时;/context 与 /model roles 翻账)
// ---------------------------------------------------------------------------

// 一次后台模型调用(压缩/抽取/标题)的附带账:这类调用额外花的采样不
// 混进普通 turn 的账(规格"测试"节),函数调用方拿去记进 ModelUsageLedger。
// 多次子请求(map 各块、reduce 归并)时 usage 累加成一个总数。
struct BackgroundCallAccounting {
    api::Usage usage;  // 各次子请求的 usage 合计
    bool usage_reported = false;
    std::int64_t duration_ms = 0;
};

// 一档角色的累计账。
struct ModelUsageEntry {
    int calls = 0;
    std::int64_t input_tokens = 0;   // 含 cache 读写的完整输入
    std::int64_t output_tokens = 0;
    std::int64_t duration_ms = 0;
    std::string last_model;          // 最近一次实际用的模型名
    bool reported = false;           // 服务端是否回报过 usage(没回报不拿 0 冒充)

    bool empty() const { return calls == 0; }
};

class ModelUsageLedger {
public:
    // 记一笔调用。duration_ms <= 0 表示未计时(照记,不猜)。reported=false
    // 时 token 三项按 0 收账、reported 留假——展示层写"未报告"。
    void Record(ModelRole role, std::string_view model, const api::Usage& usage, std::int64_t duration_ms,
                bool reported);

    // 回退留痕:如"cheap 不可用,compact 已回落 normal:qwen3.8-27b"。
    // from==to 的重复记录直接吞掉(不是回退)。
    void RecordFallback(TaskKind kind, ModelRole from, ModelRole to, std::string reason);

    const std::map<ModelRole, ModelUsageEntry>& by_role() const { return by_role_; }
    const std::vector<std::string>& fallback_notes() const { return fallback_notes_; }

    // 一行一账,给 /context 与 /model roles 用:角色 · 模型 · N 次 ·
    // 输入 X · 输出 Y。没账的角色不列。
    std::vector<std::string> ReportLines() const;

    void Clear() {
        by_role_.clear();
        fallback_notes_.clear();
    }

private:
    std::map<ModelRole, ModelUsageEntry> by_role_;
    std::vector<std::string> fallback_notes_;
};

}  // namespace lubancode::agent
