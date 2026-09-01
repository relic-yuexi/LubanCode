// AgentDefinition(自定义 Agent 与 Prompt Profile 单·阶段 1):一份可落盘
// 的 Agent YAML 的解析结果。只装数据,不装配——合并父上下文、模型、工具、
// 权限产出 AgentProfile 是阶段 3 的 AgentProfileResolver 的事,这里碰都不碰。
//
// 字段名与现有代码的对齐(阶段 0 契约;契约 docs/reference/agents.md §4.8
// 定的是 AgentRuntimeProfile 全套,runtime 五个预算字段与
// src/agent/runtime_profile.hpp 一字不差):
//   - runtime.max_output_tokens <-> AgentRuntimeProfile.max_output_tokens
//     (正整数;空 = 走三级解析:config > provider > 模型目录)。
//   - runtime.max_steps_per_turn <-> AgentRuntimeProfile.max_steps_per_turn
//     (src/agent/runtime_profile.hpp,同名同义:一个 turn 内的步数上限)。
//   - runtime.max_context_chars <-> AgentRuntimeProfile.max_context_chars
//     (正整数;空 = 继承,默认 600000)。
//   - runtime.context_window_tokens <-> AgentRuntimeProfile.context_window_tokens
//     (非负整数;0 = 未知,不做 mid-turn 评估)。
//   - runtime.length_continuations <-> AgentRuntimeProfile.length_continuations
//     (非负整数;0 = 不续跑,默认 1)。
//   - runtime.execution_mode <-> agent 工具的 execution_mode 三态
//     (auto/foreground/background,src/tools/agent_tool.cpp),不再用 bool。
//   - runtime.isolation <-> agent 工具的 isolation(worktree 隔离)。
//   - model.role <-> agent::ModelRole 三档(cheap/normal/lao,
//     src/agent/model_router.hpp);"inherit" 沿用父 Agent。
//   - model.effort <-> api::RequestProfile.reasoning_effort(档位字符串,
//     "inherit" 沿用父 Agent;是否越过 provider 能力由 doctor/resolver 查)。
//   - prompt.project_instructions <-> PromptOptions.project_instructions 的
//     继承开关(inherit/omit)。
//   - prompt.soul <-> AgentProfile.soul 的启停(inherit/off;首版不许在
//     Agent 文件里另塞 Soul 正文)。
//
// 解析走 yaml-cpp 严格模式(单子 4.2):未知字段、类型错、缺必填、认不得的
// 枚举值一律报错,错误指到字段与行列;绝不像旧 Workflow parser 那样静默跳过。
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::agent {

// Agent YAML 的当前 schema 版本。只收这个值;更高的版本拒绝加载并提示
// 升级(单子"兼容与发布"),更低的(不存在)同样拒。
inline constexpr int kAgentDefinitionSchemaVersion = 1;

// 一条解析诊断。error(warning = false)会让定义不可用;warning 不挡加载
// (如文件名与 name 不一致),但 doctor/inspect 都要摆出来。
struct AgentDefinitionIssue {
    std::string field;    // 出错字段路径,如 "tools.allow"、"runtime.execution_mode"
    std::string message;  // 人话,不带密钥、不带环境变量值
    int line = -1;        // 1 起的行号;拿不到时 -1
    int column = -1;      // 1 起的列号;拿不到时 -1
    bool warning = false;
    // 稳定错误码(阶段 3 起,Resolver 的合并期诊断用,契约 §9.2/§9.3 的码
    // 表:agent.permission_widening 一族)。空 = 解析层老账(码进文档表,
    // message 里是人话)——解析层不回头补码,两级各自安好。
    std::string code;

    // 诊断里打印的那一行:"file:line:col `field` [code]: message"(行列与
    // 码缺席就短写)。
    std::string Format(const std::string& file_label) const;
};

// prompt 段:Agent 怎样拼系统提示词。全是稀疏覆盖,省了就继承父 Agent。
struct AgentPromptSpec {
    std::optional<std::string> profile;  // 引用 Prompt Profile 名;空 = 继承父的或落回 default
    // project_instructions(AGENTS.md 那段)要不要跟着继承。
    enum class ProjectInstructions { Inherit, Omit } project_instructions = ProjectInstructions::Inherit;
    // 首版 Soul 只能跟着继承或关掉,不许在 Agent 文件里另塞正文。
    enum class Soul { Inherit, Off } soul = Soul::Inherit;
};

// model 段:引用现有模型角色与推理强度,不另发明模型名。
struct AgentModelSpec {
    std::string role;    // ""/inherit/cheap/normal/lao;空与 inherit 同义(沿用父 Agent)
    std::string effort;  // ""/inherit/档位;空与 inherit 同义(沿用父 Agent)
};

// tools 段:只收完整工具名,首版不做 glob。已按原次序去重。
struct AgentToolRules {
    std::vector<std::string> allow;  // 空 = 不裁,继承父 Agent 的有效工具
    std::vector<std::string> deny;   // 与 allow 交叠时 deny 胜出(单子测试账)
};

// 一份解析后的 Agent 定义。全部字段都是"声明",不是"解析结果"——有效性
// (Skill/MCP/工具/模型角色存不存在)由 doctor 静态预检与阶段 3 的 resolver 查。
struct AgentDefinition {
    int schema = kAgentDefinitionSchemaVersion;
    std::string name;         // 稳定 ID,小写 kebab-case
    std::string description;  // 何时派它出场,给主 Agent 派活用
    AgentPromptSpec prompt;
    AgentModelSpec model;
    std::vector<std::string> skills_preload;  // skills.preload,启动时装进上下文的 Skill 名
    AgentToolRules tools;
    std::vector<std::string> mcp_servers;     // 只引用已配置、已信任的服务名,不内联启动配置
    std::vector<std::string> requires_tools;  // requires.tools,启动前检查,缺项报错不放宽
    // ---- runtime:复用 AgentRuntimeProfile 概念(见文件头对齐账,契约 4.8) ----
    std::optional<int> max_output_tokens;      // 正整数;空 = 三级解析(config > provider > 目录)
    std::optional<int> max_steps_per_turn;     // 非负整数;空 = 继承父 Agent(0 = 不限步)
    // 任务总 turn 上限(turn 预算单 §4.1):非负整数;空 = 走宿主默认
    //(subagent.default_max_turns,再缺 = 0 不限)。含义是"从接到任务到交回
    // 终态,最多准入几次逻辑模型请求"——父代理补话、孩子回信、Stop 钩子
    // 续跑都吃同一本累计账。与 max_steps_per_turn(兼容窗内仍是"每个
    // input round"的旧义)分家。新旧同现的明拒在 P1-0 兼容批落,本批先
    // 双读不冲突(两根线各自执法,谁先到谁收)。
    std::optional<int> max_turns;
    std::optional<std::size_t> max_context_chars;      // 正整数;空 = 继承(默认 600000)
    std::optional<std::size_t> context_window_tokens;  // 非负整数;空 = 继承(0 = 未知)
    std::optional<int> length_continuations;   // 非负整数;空 = 继承(默认 1,0 = 不续)
    std::string execution_mode;                // auto/foreground/background;空 = auto
    std::string isolation;                     // none/worktree;空 = none
    // ---- permissions:只能比父 Agent 更窄 ----
    std::string permissions_mode;           // inherit/confirm/auto/yolo;空 = inherit
};

// 解析结果:definition 在有任何一个 error 时为 nullopt(不交半份定义出去);
// issues 把 error 与 warning 都攒全,doctor 逐条摆。
struct AgentDefinitionParseResult {
    std::optional<AgentDefinition> definition;
    std::vector<AgentDefinitionIssue> issues;
};

// Agent 名字规矩(单子 4.1:小写 kebab-case):与 Skill 名同一套词法——
// 1-64 个 ASCII 小写字母/数字/横线,横线不顶头、不收尾、不连写。
// 内置的 general-purpose/Explore 由码内注册,不过这道闸(Explore 带大写,
// 单子明令名称先保留)。
bool IsValidAgentName(const std::string& name);

// 字符串数组去重并保住原次序(单子 4.2:"数组去重,同时保住原次序")。
std::vector<std::string> DedupePreserveOrder(const std::vector<std::string>& items);

// 严格解析一份 Agent YAML。file_label 只进诊断文案(来源层与路径由 Catalog
// 记),不参与语义。文本为空、语法坏、未知字段、类型错、缺必填、枚举值
// 认不得,都折成 issues 里的 error。
AgentDefinitionParseResult ParseAgentDefinitionYaml(const std::string& yaml_text,
                                                    const std::string& file_label);

}  // namespace lubancode::agent
