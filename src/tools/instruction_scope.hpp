// 写前作用域闸(AGENTS.md 作用域单 P0,单子 §六/§七):路径可知的写
// 工具在真正落盘前,先问一句"这些目标的 instruction chain 本 Agent 见过
// 没有"。第一次拦住不是错误,是协议握手——该 scope 的完整规则随
// tool_result 注入下一份请求,模型读后重试同一目标即放行。
//
// 分工(单子 §六的 owner 表):
//   ProjectInstructionResolver(config 层)——某目标受哪些文档管,唯一真账;
//   InstructionScopeState(这里)——当前 Agent 已见过哪些链指纹,各 Agent
//     自持一份,不共享;
//   ScopedInstructionGate(这里)——写前能不能落盘的判定;
//   RunOneTool(agent 层)——闸的挂点,在 PreToolUse Hook 与用户确认之前、
//     任何文件副作用之前,全仓只有这一条正门,不开旁路。
//
// 线程规矩:Resolver 全程 const 可并跑;State 里的集合有锁护着(主代理与
// workflow 工具节点可能在不同线程碰同一份会话账),锁只罩 set 读写。

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/project_instructions.hpp"  // ProjectInstructionResolver/InstructionChain

namespace lubancode::tools {

// Agent 自持的已见链指纹账。指纹由文档内容摘要合成(见 Resolver 的
// ChainFingerprint):AGENTS.md 一变,指纹即变,旧确认自然作废——账按
// 内容寻址,无须主动失效,也不会拿旧指纹错放行新规则。
class InstructionScopeState {
public:
    bool Seen(const std::string& fingerprint) const;
    void MarkSeen(const std::string& fingerprint);
    void Reset();

private:
    mutable std::mutex mutex_;
    std::set<std::string> seen_;
};

// 从写工具入参里抽出全部目标路径(UTF-8 原样,不归一——归一是
// Resolver 的事)。P0 认得 write_file / edit_file 的 "path";其余工具
// (含未知名、只读工具)返回空,本闸不管。将来接入多路径 patch 工具时
// 在这里扩名单。
std::vector<std::string> CollectWriteTargets(const std::string& tool_name, const nlohmann::json& input);

// 拦截回执:message 是给模型看的完整文案(instructions_required + 注入
// 的规则正文);fingerprints 是本次已出示并登记的链,重试同目标即命中。
struct ScopeGateDenial {
    std::vector<std::string> presented_fingerprints;
    std::string message;
};

// 写前作用域闸。构造只持引用,不拷贝 Resolver(共享同一份);CheckTargets
// 无副作用之外的隐藏动作——拦下时才把指纹登记进 State(放行路不记新账)。
class ScopedInstructionGate {
public:
    ScopedInstructionGate(const lubancode::config::ProjectInstructionResolver& resolver,
                          InstructionScopeState& state)
        : resolver_(&resolver), state_(&state) {}

    // 多目标判定(单子 §7.4):每目标各自 Resolve 链,按链指纹分组;
    // 任一作用域未确认 → 整笔拦(零副作用,一个文件都不动);全部已确认
    // 或目标不受任何指令文档管(空链)→ nullopt 放行。
    std::optional<ScopeGateDenial> CheckTargets(const std::vector<std::string>& utf8_targets);

private:
    const lubancode::config::ProjectInstructionResolver* resolver_;
    InstructionScopeState* state_;
};

// 一条链的注入正文(单测与拦截文案共用同一份):按"最近文档优先"往
// budget 里整份装(近处规则永不被根文件挤没,单子 §5.4 的病根),输出
// 仍按 root -> nearest 排——机械优先级一眼可读。装不下的远端文档点名
// 列出,不冒充全部已装。
std::string BuildChainInjection(const lubancode::config::InstructionChain& chain, std::size_t budget);

// ---- 装配层共用件 ----------------------------------------------------------

// 把 resolver + state 绑成 RunOneTool 的 on_scope_gate 形状(shared_ptr 按值
// 捕获,闭包自持寿命)。认不得的写工具名直接放行(CollectWriteTargets
// 名单为准)。交互会话、单发、子代理、Workflow 三路都从这一只出,语义
// 不许各拼各的。
std::function<std::optional<std::string>(const std::string&, const nlohmann::json&)>
BuildScopeGateCallback(std::shared_ptr<const lubancode::config::ProjectInstructionResolver> resolver,
                       std::shared_ptr<InstructionScopeState> state);

// 基线预确认(单子 §7.1):root->cwd 链已拼进这个 Agent 系统提示的,写
// 同 scope 不重复拦。口径是"内容逐字节对上才算已见":prompt_instructions
// 是装配时拼进系统提示的那截串,与当下 Resolve 出的 content 全等且未截断
// 才登记——会话搬过房、指令被外部改过、根文件太长被帽掐过的,一律不
// 算已见,首次写重新注入。宁多拦一次,不静默放行。
void MarkBaselineSeen(const lubancode::config::ProjectInstructionResolver& resolver,
                      InstructionScopeState& state, const std::filesystem::path& cwd,
                      const std::string& prompt_instructions);

}  // namespace lubancode::tools
