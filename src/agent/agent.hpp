// Agent(骨架拆解批四:从 loop.hpp 自立门户)。一台引擎加一张皮:皮 =
// AgentProfile,身份与请求策略全在皮上;环境接线(inbox、压力钩、发号器)
// 收进 AgentWiring;上下文管理(双历史/前缀账/sticky/压缩台账/artifact)
// 在 ContextManager。loop.hpp 只剩轮次推进器的家当(Callbacks、RunOutcome、
// AgentLoop、RunOneTool)。
//
// 门(病十二的收口):构造只留 profile 正门一只;兼容门(运行档案+提示、
// 裸参)已封——单测与既有调用方一律折成 AgentProfile 进来。构造后的活门
// 分三类,各有其名:
//   - 皮上的活字段:/model、/think 走 SetRequestProfile;/soul 走 SetSoul;
//     模型目录指令走 SetModelInstructions;/worktree 换目录重拼提示走
//     SetSystemPrompt;/context、/model 改窗口走 SetContextWindowTokens。
//   - 接线:SetWiring 一只(inbox/压力钩/发号器整份换)。
//   - 上下文策略:经 context() 直改 ContextManager(压缩开关、artifact 仓)。

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1)
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "agent/context_manager.hpp"
#include "agent/loop.hpp"
#include "agent/resolved_prompt_builder.hpp"  // ResolvedPromptBase(Token 账本单 A1)
#include "agent/runtime_profile.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

// 跨会话传话(0.25.x)的安全收件点:Run() 的工具循环每次"下一次请求尚未
// 发出"的边界(循环顶)会调一次 inbox;有信就注进 history,再发请求——
// 工具跑着不打断,正文收口后才收。注入规则(纯函数,单测钉):
//   - history 末条是 user(比如刚攒完的 tool_result 消息):把来信的文本块
//     追加到那条消息的末尾(保持 user/assistant 交替,三种 wire 都安全);
//   - 否则(末条是 assistant 等罕见边界):新起一条 user 消息。
// 来信的"来历"由调用方在文本里带清来源标识(不装成用户手敲),这里只管
// 结构;来信绝不会被当成确认、权限或命令——这条路由里根本没有那些口子。
using InboxPoll = std::function<std::optional<api::Message>()>;

// mid-turn 上下文安全点的压力通报回调(形状与语义见 agent/context.hpp 的
// ContextPressure)。
using OnContextPressure = std::function<void(const ContextPressure&)>;

// 系统提示的段落开关(骨架拆解批三·病十裁决):mcp/web/lsp/platforms 四段
// 从前只在主循环的 AssembleSystemPrompt 里按配置拼,子代理走 BuildSystemPrompt
// 薄壳不传——子代理天生缺这四段,无文档说是设计还是疏漏。裁决:补,写明补
// ——四段开关写进皮(显式声明),子代理默认与主代理同段;真要少的皮显式
// 关。皮上写不出的差别,就是还没想清的差别。
struct PromptSectionSwitches {
    bool mcp = true;   // features/mcp.md 段
    bool web = true;   // features/web.md 段
    bool lsp = true;   // features/lsp.md 段
    std::string wire;  // platforms/<wire>.md 段;空 = 不注平台段
};

// Agent 的完整装配档案(皮)。宿主只需换这份数据,便能得到 main、Explore、
// general-purpose 或 workflow agent;不靠继承分叉实现。
struct AgentProfile {
    std::string provider;
    // 请求档案:model/effort/reasoning 只此一份(批四·病十一其一:运行档案
    // 不再另存 model,请求整形归 RequestProfile,运行档案只管预算)。
    api::RequestProfile request;
    // 运行策略:输出/上下文预算、窗口、步数、length 续跑次数。
    AgentRuntimeProfile runtime;
    std::string system_prompt;
    // 病十:四段开关随皮走。main 的皮按实际配置填(config.mcp_servers/
    // search/lsp_servers/wire);子代理的皮从 main 派生时同段拷贝;旧调用
    // 方不填就是缺省全开(裁决:补)。
    PromptSectionSwitches prompt_sections;

    // ---- 批四·病十一其三:五层请求改写后端退役,会话级请求策略归位皮上。
    // 从前 Model/Think/ModelInstructions/SoulOverlay/DeferredIndex 五只
    // Backend 包装层在传输侧现拼请求(model 换字、effort 覆盖、system 追
    // 段);现在全部在 Agent 拼请求的那一步就地生效,指纹与 cache epoch 的
    // 账也从此看得见这些改动(从前在指纹算完之后才改,账是瞎的)。----
    // 模型目录(models.json)base_instructions 的原文,随 /model 换;空 =
    // 不注。注入位置:延迟索引段之后、魂之前。
    std::string model_instructions;
    // 魂(SOUL.md / souls/)的原文,随 /soul 换;空 = 不注,注入时剥注释。
    // 永远压轴(system 的最后一段)。
    std::string soul;
    // tool_search 的延迟工具索引段(活口):逐请求现查——tool_search 命中
    // 会在一次 Run() 中途改变 loaded 集合,下一份请求的索引段就得跟着变。
    // 空 = 不注。
    std::function<std::string()> deferred_index_provider;

    // ---- 病十三的方向:工具可见性策略写进皮(实例仓与策略分家的第一步)。
    // 谓词不放行的工具不进请求的 tools 数组;模型调了"注册表里查得到、
    // 谓词不放行"的工具时,回 filter_denial 的文案(空 = "尚未挂载"的
    // 默认说法)。谓词由装配层注入(按 loaded 集合/角色过滤),AgentLoop
    // 自己不懂什么叫"延迟"。----
    std::function<bool(const tools::Tool&)> tool_filter;
    std::string tool_filter_denial;

    // ---- Token 账本单 A1(事实接线) ----
    // 这份 Agent 实例对应的请求用途:main_turn/subagent_turn/workflow_node/
    // …(§6.2 十二值枚举)。装配层按调用场合显式设(主会话/AgentTool 子
    // 代理/workflow agent 节点各自的构造点);默认 MainTurn 是主会话的
    // 真实值,不是"漏填的占位"。
    accounting::RequestPurpose purpose = accounting::RequestPurpose::MainTurn;
    // ResolvedPromptBuilder 的底账(§6.4):装配层用 BuildResolvedPromptBase
    // 替代裸调 AssembleSystemPrompt 时才有值,连同 system_prompt 一并从
    // 同一次拼装产出。空 = 这份皮的系统提示未接 ResolvedPromptBuilder
    //(旧调用方/未迁移路径),AgentLoop 落 model.request.prepared 时不带
    // manifest,文本组装退回原 With* 三连,行为与接线前逐字节一致。
    std::optional<ResolvedPromptBase> resolved_prompt_base;
};

// 环境接线(批四·病十二:接线类的门收成一只)。这些都是"宿主把外面的
// 世界接进引擎"的钩子,不是皮上的策略:换皮不该动它们,所以单独一份、
// 单独一道门。
struct AgentWiring {
    // 跨会话收件点(见 InboxPoll 注释):空 = 没有来信要收,行为跟从前一致。
    InboxPoll inbox;
    // 上下文压力通报(PreRequest 评估与 hard trim 之后各来一次):回调里
    // 可以安全地做一次语义压缩并 ReplaceHistory。空 = 不通报,安全网照旧
    // 只是没人听见。
    OnContextPressure on_context_pressure;
    // execution_id 发号口:装配层(接了 Runtime 的会话)把它指到
    // IdAuthority::NextItemId 上——execution_id 与 Runtime item id 同源,
    // 不另开计数器。空 = 旧路兜底 "exec-N"(仅单测/未接 Runtime 的会话)。
    std::function<std::string()> execution_id_issuer;
};

class Agent {
public:
    // 正门(唯一构造):吃一份 AgentProfile——身份(provider)、请求档案
    //(model/effort/reasoning)、运行策略(输出/上下文预算、窗口、步数、
    // length 续跑)、系统提示、段落开关、请求策略叠层与工具可见性全从这
    // 一份来。main、general-purpose 子代理、后台子代理、单发模式各自声明
    // 覆盖什么,其余继承,不再一串易漏的裸参数。
    Agent(api::Backend& backend, tools::ToolRegistry& registry, AgentProfile profile);

    const std::string& provider() const { return profile_.provider; }
    const api::RequestProfile& request_profile() const { return profile_.request; }
    // /model、/think 的会话级同步:整份请求档案换(model 归这一份,批四·
    // 病十一其一——不再有"运行档案里的 model 手工 if 同步"那条暗道)。
    void SetRequestProfile(api::RequestProfile request) { profile_.request = std::move(request); }
    // 模型目录 base_instructions 的会话级同步(/model 切目录内模型时随换)。
    void SetModelInstructions(std::string instructions) { profile_.model_instructions = std::move(instructions); }
    // 魂的会话级同步(/soul、/soul off)。
    void SetSoul(std::string soul) { profile_.soul = std::move(soul); }

    // 只读访问:运行期诊断(/context、agent 查看态)要展示"这份 loop 实际
    // 吃到的预算与来源",不再让各处自己猜。
    const AgentRuntimeProfile& runtime_profile() const { return profile_.runtime; }

    // 窗口是运行档案里唯一的活字段(/context、/model 的目录窗口生效口)。
    void SetContextWindowTokens(std::size_t window_tokens) { profile_.runtime.context_window_tokens = window_tokens; }

    // /worktree 切换目录后只换运行环境段,已有聊天史要照留。主循环在下一
    // 次请求前换掉系统提示,文件工具则由进程 CWD 即刻接管。
    void SetSystemPrompt(std::string system_prompt) { system_prompt_ = std::move(system_prompt); }

    // ---- 接线(病十二:inbox/压力钩/发号器一只门) ----
    const AgentWiring& wiring() const { return wiring_; }
    void SetWiring(AgentWiring wiring) { wiring_ = std::move(wiring); }

    // ---- 上下文策略与台账(病六:上下文管理的手都在 ContextManager) ----
    ContextManager& context() { return context_; }
    const ContextManager& context() const { return context_; }
    int cache_epoch() const { return context_.cache_epoch(); }
    const StructuralCompressionStats& structural_stats() const { return context_.structural_stats(); }
    const ResultViewMemo& result_view_memo() const { return context_.result_view_memo(); }

    // 请求级动态上下文(项目记忆召回、运行中子代理名册)。前缀缓存守恒单
    // 第五期起不再塞 system 尾巴——那会让分叉点落在全部旧历史之前,每条
    // 外层用户消息都断一次前缀。现在它随本轮 user 消息进 request_history_
    // 的尾部 TextBlock,发过即钉住,后续请求原样重放;持久 history_ 不收
    // 这块,session/export/compact/记忆抽取都只见用户真输入。空串 = 不追加。
    void SetTurnContext(std::string context) { turn_context_ = std::move(context); }

    // 发一轮用户输入。内部可能会跑好几个来回(工具调用),直到模型给出
    // end_turn(或者别的非 tool_use 的 stop_reason)才返回。历史跨多次
    // Run() 调用保留,下一句问话会带着之前的上下文。
    // cancel 非空且流式/工具执行期间被外部(cli 层的 ESC 监听线程)置位:
    // 半截 assistant 文本(如果已经流出来了)照常攒进历史,末尾附一段打断
    // 标注;工具循环发现自己被打断,已经在执行的那个工具的结果照常入历史、
    // 还没轮到的补一条"未执行"的合成 tool_result(保住 tool_use/tool_result
    // 成对约束,不然下一轮重放历史会被 API 拒绝);两种情况都从 Run() 正常
    // 返回(RunOutcome::cancelled = true),不是 std::unexpected——打断不是错误。
    std::expected<RunOutcome, std::string> Run(const std::string& user_input, const TurnWiring& wiring,
                                                const std::atomic<bool>* cancel = nullptr);

    // 跟字符串入口同义,只是调用方已经把本地图片装进 user_message 了。图片
    // 也须原样入 history,下一轮、重发、会话恢复才能带得上。
    std::expected<RunOutcome, std::string> Run(api::Message user_message, const TurnWiring& wiring,
                                                const std::atomic<bool>* cancel = nullptr);

    const std::vector<api::Message>& history() const { return context_.durable_history(); }
    // M6.6:/compact 用。跟 history() 是同一份数据,单独起个大写名字是给
    // 老调用方对账用(名字沿用,签名不动)。
    const std::vector<api::Message>& History() const { return context_.durable_history(); }

    // M6.6:/compact 压缩完之后,把内部存的完整历史换成压缩后的那份(archive
    // 消息 + 最近一轮完整对话)。agent/compact.cpp 里的 Compact() 本身不碰
    // Agent,只管算出新历史,真正替换由调用方拿到新历史后调这个方法完成。
    // 前缀记账:这是有意改前缀,不装无事发生——ContextManager 显式开新
    // cache epoch(history_compacted),清掉上一份请求的指纹;压缩后第一份
    // 请求就是新 epoch 的冷启动,后续再守追加律。
    void ReplaceHistory(std::vector<api::Message> new_history) {
        context_.ReplaceHistory(std::move(new_history));
        // mid-turn compact 在 Run() 的请求安全点同步换史。本轮动态上下文
        // 不该进 compact/session,却仍须给压缩后的下一次请求看;新 epoch
        // 已经开了,补在最新消息尾部即可,不追改旧请求。
        if (run_active_ && !active_turn_context_.empty()) {
            context_.AppendToLastRequest(api::TextBlock{active_turn_context_});
        }
    }

private:
    friend class AgentLoop;

    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    AgentProfile profile_;
    std::string system_prompt_;              // 皮上的活段(/worktree 重拼;构造时从 profile 落)
    std::string turn_context_;
    std::string active_turn_context_;        // 只在 Run() 活着时给 mid-turn compact 重注入
    bool run_active_ = false;
    ContextManager context_;
    AgentWiring wiring_;

    // 逐枚追踪:批次序号(execution_id 的兜底发号)。装配层接了 Runtime
    // 的会话在 wiring.execution_id_issuer 里换成 IdAuthority 的号——单子
    // 明言不可再造第二只计数器,这里只是没接 Runtime 的旧路(单测)兜底。
    int batch_counter_ = 0;
    std::uint64_t execution_counter_ = 0;
    std::string issue_execution_id() {
        if (wiring_.execution_id_issuer) {
            return wiring_.execution_id_issuer();
        }
        return "exec-" + std::to_string(++execution_counter_);
    }

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
