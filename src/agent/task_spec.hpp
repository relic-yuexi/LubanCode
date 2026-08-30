// AgentTaskSpec(子代理递归派工与结构化任务交接单·P0-1):任务究竟要做什么
// 的 typed 真账。wire 上 `title + task` 是新路,`title + prompt` 是 legacy 路,
// 两条都归一成这一份 spec;给模型的渲染、Agent Dock 的分栏、trajectory 的
// task_ref 全从它投影,不许从渲染后的 prompt 反向猜 spec(单子 §4.1)。
//
// 分层规矩:本件只认 std 与 nlohmann,不摸盘、不发请求、不知道 tools 层的
// 任何类型。执行控制参数(agent_type/execution_mode/isolation/预算)不进
// spec——那是 AgentDispatchOptions 的地盘(单子 §4.2)。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

// 任务边界(不是权限边界):仓库任务才填,非文件任务可空。真权限仍由工作
// 区包装与工具政策执法,spec 里的 scope 只是给子代理的任务说明。
struct AgentTaskScope {
    std::vector<std::string> include_paths;
    std::vector<std::string> exclude_paths;

    bool empty() const { return include_paths.empty() && exclude_paths.empty(); }
};

// canonical 任务合同。title 在 wire 上位于 task 对象外层,解析时收进这里;
// 从此只认 spec 里这份 canonical 值(单子 §4.1 末段)。
struct AgentTaskSpec {
    int schema_version = 1;
    std::string title;                       // 人看,显示列硬帽由派工口执法
    std::string goal;                        // 必填:子代理要达成什么,一件事
    std::optional<std::string> source_request;  // 可选,只放用户原话摘录
    std::vector<std::string> context;        // 派工者补充的事实与背景
    AgentTaskScope scope;
    std::vector<std::string> constraints;    // 不改哪里、只读等限制
    std::vector<std::string> acceptance;     // 一条一个可查条件
    std::string deliverable;                 // 必填:要回什么
    // legacy 过渡账(单子 §5.2):title+prompt 归一成 goal=prompt、
    // deliverable=占位句。legacy_prompt 置真;渲染仍走同一条 RenderDelegatedTask。
    bool legacy_prompt = false;

    bool operator==(const AgentTaskSpec&) const = default;
};

// 解析结果:ok() 为假时 error 是模型可见的稳定错误(带精确 JSON path)。
struct AgentTaskSpecParseResult {
    std::optional<AgentTaskSpec> spec;
    std::string error;  // 非空 = 拒绝原因(含 JSON path)

    bool ok() const { return spec.has_value() && error.empty(); }
};

// 长度与安全门(单子 §5.3)的公开常量:宿主输入硬帽,不是 token 预算。
inline constexpr std::size_t kMaxTaskSpecBytes = 32 * 1024;  // 整份 spec 的 UTF-8 上限
inline constexpr std::size_t kMaxTaskSpecItems = 16;         // 每组(context 等)条数上限
inline constexpr std::size_t kMaxTaskSpecItemBytes = 4 * 1024;  // 单条上限

// wire `task` 对象 -> typed spec。title 单独递进(wire 上在 task 外层)。
// 规矩:goal/deliverable 必填非空;数组逐条非空字符串;类型错/超限按稳定
// JSON path 报错;不许 NUL。task 为 null/缺省不算错(调用方走 legacy 路)。
AgentTaskSpecParseResult ParseAgentTaskSpec(const nlohmann::json& task, const std::string& title);

// legacy `title + prompt` 归一(单子 §5.2):goal=prompt、deliverable=占位句、
// legacy_prompt=true。prompt 空串由调用方先拒(错误文案在派工口)。
AgentTaskSpec CanonicalizeLegacyPrompt(const std::string& title, const std::string& prompt);

// 宿主侧强校验(schema_check 对嵌套 schema 校验不完整,这里是真闸)。
// 返回空串 = 过;非空 = 带 JSON path 的稳定错误。
std::string ValidateAgentTaskSpec(const AgentTaskSpec& spec);

// canonical JSON:字段序固定、空段省略、UTF-8 原样。trajectory 的 task_ref
// 与 task_spec_hash 都以它为准;渲染文本是派生品,可重建。
nlohmann::json CanonicalSpecJson(const AgentTaskSpec& spec);

// 稳定渲染(单子 §5.4):顺序固定、空段不渲染、多行逐行加 "- " 缩进,不让
// 内容伪造新栏。首轮 api::Message 的 TextBlock 放这份渲染,不再放裸 prompt。
std::string RenderDelegatedTask(const AgentTaskSpec& spec);

// spec 指纹:canonical JSON 的 SHA-256 前 16 位十六进制。hooks 的
// SubagentStart(task_spec_hash)与台账详情用它对账。
std::string TaskSpecHash(const AgentTaskSpec& spec);

}  // namespace lubancode::agent
