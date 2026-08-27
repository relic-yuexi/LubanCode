// Workflow 定义(自然语言编排单第 1 批):强类型 AST。
//
// workflow.yaml 解析后立刻归一成这份结构,后续校验、渲染、运行、journal
// 只认它,不再回头摸 YAML 节点——"YAML 交 yaml-cpp 解析,再转成内部强
// 类型;不得手搓缩进与 ${...}"的单子原文落在这里。
//
// 字段语义对齐单子"Workflow 定义草案"一节,但不强钉 YAML 拼法:YAML 里
// 可以写得更省,parser 负责填默认;这里的每个字段都是执行合同的一部分,
// 缺了就是缺,不猜。
//
// 依赖铁律:本头只 include 标准库与 nlohmann/json,不 include cli/app/
// frontend/tools——workflow 层经 resolver/executor 的抽象口子接现成设施,
// 定义本身谁都不认。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::workflow {

// schema_version:解析器不认的版本直接拒跑,不猜意思(单子"文件与安装
// 范围")。首版只有 1。
inline constexpr int kCurrentSchemaVersion = 1;

// ---------------------------------------------------------------------------
// 预算与限制(单子 limits 一段)
// ---------------------------------------------------------------------------

// 时限的解析产物:YAML 写 "10m"/"90s" 一类,parser 折成秒。显式秒数,
// 不包 chrono 类型——定义层要跨平台序列化稳定(单子"归一化输出与 hash
// 在三平台一致")。
struct WorkflowLimits {
    int max_concurrency = 4;        // 并行分支同时放行的上限(全局帽)
    int max_nodes = 64;             // 展开后的节点数帽(map 展开用)
    int max_steps = 128;            // 全局步数帽(含循环迭代)
    std::int64_t timeout_secs = 600;  // 总时限
    int tool_calls = 100;           // 工具调用次数帽(重试也计数,不开免单账)
    std::int64_t tokens = 120000;   // token 帽(输入+输出累计)

    bool operator==(const WorkflowLimits&) const = default;
};

// ---------------------------------------------------------------------------
// 重试与回落(单子"重试、回落与幂等"一节)
// ---------------------------------------------------------------------------

enum class BackoffKind { Fixed, Exponential };

// 可重试错误类别。稳定 code 由运行时错误载荷给出,这里只存定义声明的
// 白名单;空表 = 只按默认档(超时/限流/瞬时网络)。
struct RetryPolicy {
    int attempts = 1;  // 总尝试次数(1 = 不重试)
    BackoffKind backoff = BackoffKind::Exponential;
    std::int64_t initial_ms = 1000;
    std::int64_t max_ms = 30000;
    bool jitter = true;
    std::vector<std::string> when;  // 错误 code 白名单;空 = 默认可重试档

    bool operator==(const RetryPolicy&) const = default;
};

// 工具缺失/不可用时的处置(单子"Workflow 与 Tool/Plugin/MCP"):
// fail = 报错终止;skip = 跳过并在结果里记缺失;fallback = 走 fallback_to
// 指定的节点(必须是一条明边,不许运行时暗换);ask = 挂起问用户。
enum class OnUnavailable { Fail, Skip, Fallback, Ask };

std::string ToString(OnUnavailable value);

// ---------------------------------------------------------------------------
// 节点
// ---------------------------------------------------------------------------

// 首版节点种类(单子"首版节点种类"两节全表)。transform 不塞任意脚本,
// 只认宿主注册的纯数据变换名,validator 查能力表。
enum class NodeKind {
    Tool,       // 调 ToolRegistry 已注册工具
    Agent,      // 派一只现有子代理(AgentTool)
    Llm,        // 单次结构化模型调用,不开完整 agent loop
    Skill,      // 把 Skill 装进一次执行上下文
    Template,   // 安全模板渲染,不为排版再费一轮模型
    Transform,  // 宿主注册的纯数据变换
    Approval,   // 经 InteractionBroker 悬起审批
    AskUser,    // 经 InteractionBroker 悬起补问
    Subflow,    // 调用另一份有版本的 Workflow
    Async,      // 另开工作线程跑一只 I/O 节点,主调度守取消与总时限
    Parallel,   // 同时放行若干独立分支(分支形状写定)
    Join,       // 收束分支(all/all_settled/any/quorum/race)
    Map,        // 数组拆项,逐项跑一只节点或子图(运行时按数据扩开)
    Reduce,     // 按稳定次序汇总 map/parallel 结果
    Switch,     // 按结构化条件选路
    Foreach,    // 顺次迭代(有依赖或易撞 rate limit 的活)
    Loop,       // 有硬帽的条件循环:顺次跑 body,until 命中即停
    Checkpoint, // 显式落断点
    End,        // 写终态与 Workflow 输出
};

std::string ToString(NodeKind kind);
bool ParseNodeKind(const std::string& s, NodeKind& out);

// join 策略(单子"并行与汇合规矩"五种)。
enum class JoinPolicy { All, AllSettled, Any, Quorum, Race };

std::string ToString(JoinPolicy policy);
bool ParseJoinPolicy(const std::string& s, JoinPolicy& out);

// 条件表达式的受限形状:switch 边与 fallback 只读结构化值,禁 eval、
// shell、内联脚本(单子 Edge 一节)。首版只钉三种原子判定,够论文检索
// 这类 SOP 用;后续要更复杂的,加节点种类,不开表达式口子。
enum class ConditionOp {
    Exists,        // ${path} 有值(非 null)
    NotExists,     // ${path} 无值
    Equals,        // ${path} == literal
    NotEquals,     // ${path} != literal
    GreaterThan,   // ${path} > literal(数值比较)
    LessThan,      // ${path} < literal
    Contains,      // ${path}(数组/字符串)含 literal
    StartsWith,    // ${path}(字符串)以 literal 起头
    NonEmpty,      // ${path} 是非空数组/字符串
};

std::string ToString(ConditionOp op);
bool ParseConditionOp(const std::string& s, ConditionOp& out);

// 一只节点。kind 之外的领域字段按需填,parser 负责保证"kind 与字段"的
// 自洽(如 Tool 节点必有 tool 名);单态结构 + kind 的路数与 ServerEvent
// 同理:加节点种类不惊动全体消费方。
struct WorkflowNode {
    std::string id;         // 图内唯一;同时是 Store 分区名
    std::string label;      // 人看的名(可空 = 用 id)
    NodeKind kind = NodeKind::Tool;

    // tool 节点:稳定 tool id(如 plugin__papers__arxiv_search)。
    std::string tool;
    // agent 节点:role/task/allowed_tools/step_limit/model_role。
    std::string role;
    std::string task;             // prompts/xx.md 一类包内引用
    std::vector<std::string> allowed_tools;
    int step_limit = 0;           // 0 = 用全局默认
    std::string model_role;       // 空 = 会话当前模型
    // llm 节点:prompt 包内引用 + output_schema(JSON Schema,可空)。
    std::string prompt;
    nlohmann::json output_schema = nlohmann::json::object();
    // skill 节点:skill 名(来自 SkillCatalog)。
    std::string skill;
    // transform 节点:宿主注册的变换名(validator 查能力表,不认魔法串)。
    std::string operation;
    // template 节点:模板文件包内引用。
    std::string template_path;
    // subflow 节点:目标 workflow id + 版本要求(空 = 最新)。
    std::string subflow_id;
    std::string subflow_version;
    // async:另开工作线程执行一只普通节点。它是 I/O 等待边界,不是 fan-out;
    // 产物按 async 节点 id 再落一份,下游不必越过边界去读 body。
    std::string async_body;
    // parallel/join:分支 id 表 + 汇合策略 + 分支并发帽。
    std::vector<std::string> branches;
    JoinPolicy join = JoinPolicy::AllSettled;
    int join_quorum = 0;          // JoinPolicy::Quorum 时 N,0 = 未设
    int max_concurrency = 0;      // 0 = 用全局;parallel/map 各自可压
    // map/foreach:要迭代的数组来源(${...} 引用)+ 每项跑的节点 id。
    std::string items_ref;
    std::string map_body;         // map/foreach 逐项执行的节点 id
    // reduce:汇总 map/parallel 结果的节点 id(稳定次序按定义顺序)。
    std::string reduce_body;
    std::string initial_ref;      // reduce 起始累加值(${...},可空)
    // switch:conditions 按声明顺序评,首中即走;都不中走 default_to。
    struct SwitchCase {
        ConditionOp op = ConditionOp::Exists;
        std::string path;    // ${...} 引用
        nlohmann::json literal = nlohmann::json::object();  // 比较字面量
        std::string to;      // 命中后去的节点 id

        bool operator==(const SwitchCase&) const = default;
    };
    std::vector<SwitchCase> conditions;
    std::string default_to;       // 可空 = 都不中时结束(outcome=skipped)

    // loop:body 每轮顺次执行;until 在一轮末尾读取 body 最新输出。
    // min/max 可写正整数,也可写 ${inputs.xxx};hard_limit 必须是定义里的
    // 正整数,运行期 max 不得越过它。
    std::vector<std::string> loop_body;
    std::optional<SwitchCase> loop_until;
    nlohmann::json loop_min_iterations = 1;
    nlohmann::json loop_max_iterations = nullptr;
    int loop_hard_limit = 32;

    // 入参:${...} 引用混字面量,ResolveInputs 阶段展开。object 形状;
    // 也可整体一个 "${...}" 字符串(此时 input 直接取引用值)。
    nlohmann::json input = nlohmann::json::object();

    // 出边之外的重试与缺失处置。
    std::optional<RetryPolicy> retry;
    OnUnavailable on_unavailable = OnUnavailable::Fail;
    std::string fallback_to;       // on_unavailable=fallback 时必填

    // checkpoint 自动落点开关(默认:节点终态后轻 checkpoint)。
    bool checkpoint = true;
    // 副作用声明:写文件/发消息一类。无 idempotency_key 的副作用节点
    // attempts 只能是 1(单子"重试、回落与幂等")。
    bool has_side_effects = false;
    std::string idempotency_key;   // 空 = 无幂等键

    bool operator==(const WorkflowNode&) const = default;
};

// 普通出边:from + outcome + to。一个节点同一 outcome 只许一条普通出边,
// fan-out 必须走 parallel/map 节点(单子 Edge 一节)。
struct WorkflowEdge {
    std::string from;
    std::string outcome = "success";  // success / error / empty / skipped / joined ...
    std::string to;

    bool operator==(const WorkflowEdge&) const = default;
};

// ---------------------------------------------------------------------------
// 定义本体
// ---------------------------------------------------------------------------

struct WorkflowDefinition {
    int schema_version = kCurrentSchemaVersion;
    std::string id;
    std::string version;      // 业务版本,编辑时至少补 patch
    std::string name;
    std::string description;
    std::string alias;        // /<alias> 直呼入口;空 = 只走 /workflow run
    bool enabled = true;      // disable 后 catalog 仍列,直呼 alias 不响应

    // 输入 schema(JSON Schema 子集:type/required/properties/默认值)。
    nlohmann::json inputs = nlohmann::json::object();
    // 输出 schema:声明 result 各字段的形状。
    nlohmann::json outputs = nlohmann::json::object();

    std::string entry;
    WorkflowLimits limits;

    std::vector<WorkflowNode> nodes;  // 定义顺序即汇合顺序(单子 Store 一节)
    std::map<std::string, WorkflowNode> node_map;  // parser 填,id -> 节点

    std::vector<WorkflowEdge> edges;

    // result 映射:${nodes.<id>.output...} -> 输出字段。
    nlohmann::json result = nlohmann::json::object();

    // 归一化 JSON(解析后的稳定序列化,内容 hash 的底)。parser 填;
    // 手工构造的定义调 BuildNormalizedJson 补。
    nlohmann::json normalized;

    nlohmann::json ToJson() const;
    static WorkflowDefinition FromJson(const nlohmann::json& j);
};

// 归一化序列化:字段顺序固定(nlohmann::json 用 ordered_json 保序),
// 三平台逐字节一致——hash 与"resume 必须找到同一 hash"都靠它。
nlohmann::json BuildNormalizedJson(const WorkflowDefinition& def);

// 内容 hash:归一化 JSON 的 SHA-256(64 位十六进制小写)。审批、缓存、
// 恢复都认 hash,不只认可改名的 id(单子"文件与安装范围")。
std::string ContentHash(const WorkflowDefinition& def);

}  // namespace lubancode::workflow
