// TurnRuntime(显示系统剥离单第三步):一轮问答的纯逻辑核。
//
// 从 app/turn_runner.cpp 的 RunTurn/BuildCallbacks/ConfirmToolUse 里抽出
// "不碰画面"的那半:权限裁定、hooks 决策、usage 记账、取消旗、prompt 预
// 处理。footer、监听线程、ToolDisplay、分界线、统计行这些终端活留在
// turn_runner(第五步的 TerminalTurnView 现居所),只消费这里的决定。
//
// 依赖方向:合同头(event.hpp 一族)零依赖;本头是合同之上的编排层,认
// hooks/、config/、tools/、api/ 这些内核件,不 include cli/*、app/*、
// frontend/*——单子"Runtime 不碰界面"的编译边界从这里起步。
//
// 行为约定:本头里的每一枚函数都是从 turn_runner 原文逐句搬来的裁定,
// 语义一个字不改;单测(test_turn_runtime.cpp)钉三景——权限、取消、
// usage——全程不碰终端。

#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"      // ToolHookDecision:hooks 决策的中立表态
#include "api/types.hpp"       // UsageReport/Message:usage 与 prompt 的领域形状
#include "config/command_permission.hpp"  // ClassifyCommandByPermissions:permissions 叠加(问题 7 拆出,不再借 config.hpp)
#include "hooks/dispatcher.hpp"
#include "tools/tool.hpp"      // Tool::Result:PostToolUse 的载荷

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 权限档位:cli::ConfirmMode 的中立镜像(Confirm/Auto/Yolo,Shift+Tab 循环)。
// 不 include cli/*,字段同名同义;映射由终端装配层做,漂移由单测钉住。
// ---------------------------------------------------------------------------
enum class PermissionMode { Confirm, Auto, Yolo };

// ---------------------------------------------------------------------------
// 权限裁定(纯函数,原文自 ConfirmToolUse 搬来)
//
// 裁定次序(一条不许倒):
//   1. settings.local.json 的 permissions 叠加:run_command 先过
//      ClassifyCommandByPermissions——deny 命中即黑名单,压过 allow、压过
//      会话"总是允许";yolo/--yes 是显式全放,deny 不拦。
//   2. auto 档里 run_command 过 ClassifyCommand(tools/command_safety)自动
//      分析,Safe 或 allow 命中即放;PowerShell 脚本块({ } 体内是任意
//      代码)两条路都不放,拉回确认。
//   3. PreToolUse 钩子的表态参与裁决:deny_hit 最高;钩子 allow 只跳"问
//      用户"这一步;钩子 ask 把"本来自动放行"拉回确认。
//   4. auto_confirm(--yes)/yolo/auto 档文件工具/选过 a 的工具,放行。
//
// 输出 Action::Ask 不等于"弹终端菜单"——真要问用户之前还有一道
// PermissionRequest 钩子(见 EmitPermissionRequest),问话本身由前端实现。
// ---------------------------------------------------------------------------

struct PermissionContext {
    bool auto_confirm = false;  // --yes:显式全放,deny 也不拦
    PermissionMode mode = PermissionMode::Confirm;
    const std::set<std::string>* always_allowed = nullptr;  // 选过 a 的工具(会话级)
    const std::vector<std::string>* allow_commands = nullptr;  // settings.local.json 前缀白名单
    const std::vector<std::string>* deny_commands = nullptr;   // settings.local.json 前缀黑名单
};

struct PermissionVerdict {
    enum class Action { Allow, Ask };
    Action action = Action::Ask;
    bool deny_hit = false;  // 黑名单命中(诊断/文案用;allow 时恒 false)
};

// 纯函数,可单测:档位 + 钩子表态 + 工具名/入参 -> 放行还是问。
PermissionVerdict EvaluatePermission(const PermissionContext& context, const runtime::ToolHookDecision& pre,
                                     const std::string& name, const nlohmann::json& input);

// ---------------------------------------------------------------------------
// hooks 决策(发射 + 归并映射)
//
// payload 组装、HookEventResult -> runtime::ToolHookDecision 的映射、
// PostToolUse 反馈的提取,原先在 BuildCallbacks 的闭包与 agent_tool 的后台
// 路径里各写一份,现在归一处。发射会真跑钩子进程(副作用),映射本身纯。
// ---------------------------------------------------------------------------

// PreToolUse:deny -> 拦;ask -> 强制问;allow -> 跳用户确认(deny 规则照
// 走);updatedInput/additionalContext 随行。dispatcher 为空 = 没配 hooks,
// 返回全默认决策(与"没有 hooks 系统"逐字节一致)。
runtime::ToolHookDecision EmitPreToolUse(hooks::HookDispatcher* dispatcher, const std::string& name,
                                       const nlohmann::json& input,
                                       const std::string& tool_execution_id = std::string());

// PostToolUse:钩子反馈逐条交回,追加进模型所见的 tool_result。副作用已发
// 生,不能撤销,只许追加反馈。
std::vector<std::string> EmitPostToolUse(hooks::HookDispatcher* dispatcher, const std::string& name,
                                         const nlohmann::json& input, const tools::Tool::Result& result,
                                         const std::string& tool_execution_id = std::string());

// PermissionRequest:真要问用户前的那一票。deny -> 拒;allow -> 不弹;不表
// 态 -> 正常问。发射本体;文案上屏由前端负责。
enum class PermissionHookReply { None, Allow, Deny };
struct PermissionHookResult {
    PermissionHookReply reply = PermissionHookReply::None;
    std::string reason;  // deny 的理由,给用户与模型看
};
PermissionHookResult EmitPermissionRequest(hooks::HookDispatcher* dispatcher, const std::string& name,
                                           const nlohmann::json& input);

// 装配层用来决定挂不挂 hook 回调(空 dispatcher = 没配,行为与从前一致)。
bool HasToolHooks(const hooks::HookDispatcher* dispatcher);
bool HasPermissionHooks(const hooks::HookDispatcher* dispatcher);

// 纯映射:HookEventResult.permission -> ToolHookDecision(后台子代理的
// DetachedHookSession 路径与主路径共用同一颗映射脑袋,deny 理由拼法一处定)。
runtime::ToolHookDecision MapPreToolDecision(const hooks::HookEventResult& merged);

// ---------------------------------------------------------------------------
// usage 记账(原文自 app::UsageStats 搬来,语义一个字不改)
// ---------------------------------------------------------------------------

// 一次独立模型请求的 usage 流水账(前缀缓存守恒单第一期):on_usage 每响
// 一次落一笔,append-only,不拿整轮平均数盖过去。
struct StepUsageRecord {
    int step_index = 0;
    std::string request_id;
    std::string model;
    int cache_epoch = 1;            // 请求落在哪个缓存 epoch(1 起)
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;  // output 里的 reasoning 拆账(含在 output_tokens)
    bool reported = false;             // provider 是否回报了 usage
    std::string epoch_break_reason;    // 空 = 本步没断 epoch

    std::int64_t total_input_tokens() const {
        return input_tokens + cache_read_tokens + cache_creation_tokens;
    }

    // 本步命中率(百分比);没实测(reported=false 或总输入 0)返回 -1,
    // 显示层写"服务端未回报",不许拿 0 冒充真未命中。
    int cache_hit_percent() const {
        if (!reported || total_input_tokens() <= 0) {
            return -1;
        }
        const double ratio =
            static_cast<double>(cache_read_tokens) / static_cast<double>(total_input_tokens()) * 100.0;
        return static_cast<int>(ratio + 0.5);
    }
};

// 一次 Run() 内(可能因为工具调用来回好几趟)的 token 用量台账:每笔
// usage 落成一条 StepUsageRecord,汇总从记录求和——不是只存整场平均数。
// 统计行的"输入"一律走 total_input_tokens(input+cache_read+cache_creation),
// 命中率分母只取输入,按 token 总和重算,不取各步百分比的算术平均。
struct TurnUsageStats {
    std::vector<StepUsageRecord> steps;

    void Add(const api::UsageReport& report) {
        StepUsageRecord record;
        record.step_index = report.step_index;
        record.request_id = report.request_id;
        record.model = report.model;
        record.cache_epoch = report.cache_epoch;
        record.input_tokens = report.usage.input_tokens;
        record.cache_read_tokens = report.usage.cache_read_tokens;
        record.cache_creation_tokens = report.usage.cache_creation_tokens;
        record.output_tokens = report.usage.output_tokens;
        record.reasoning_tokens = report.usage.output_reasoning_tokens;
        record.reported = report.reported();
        record.epoch_break_reason = report.epoch_break_reason;
        steps.push_back(std::move(record));
    }

    int request_count() const { return static_cast<int>(steps.size()); }

    std::int64_t input_tokens() const {
        std::int64_t total = 0;
        for (const auto& step : steps) total += step.input_tokens;
        return total;
    }

    std::int64_t cache_read_tokens() const {
        std::int64_t total = 0;
        for (const auto& step : steps) total += step.cache_read_tokens;
        return total;
    }

    std::int64_t cache_creation_tokens() const {
        std::int64_t total = 0;
        for (const auto& step : steps) total += step.cache_creation_tokens;
        return total;
    }

    std::int64_t output_tokens() const {
        std::int64_t total = 0;
        for (const auto& step : steps) total += step.output_tokens;
        return total;
    }

    // 输出里 reasoning 的拆账合计(含在 output_tokens 里,不是另加的一笔;
    // provider 没拆账就是 0——与"reasoning 真为零"分不清,显示层措辞按
    // "未拆账"处理,不猜)。
    std::int64_t reasoning_tokens() const {
        std::int64_t total = 0;
        for (const auto& step : steps) total += step.reasoning_tokens;
        return total;
    }

    // 完整输入(input + cache_read + cache_creation)。
    std::int64_t total_input_tokens() const {
        return input_tokens() + cache_read_tokens() + cache_creation_tokens();
    }

    // 整轮命中率(百分比,四舍五入);分母只取输入,按 token 总和重算。
    // 一笔实测都没有(全没回报)时返回 -1,显示层写"服务端未回报"。
    int cache_hit_percent() const {
        if (total_input_tokens() <= 0) {
            return -1;
        }
        const double ratio = static_cast<double>(cache_read_tokens()) / static_cast<double>(total_input_tokens()) * 100.0;
        return static_cast<int>(ratio + 0.5);
    }

    // 整轮里哪怕一笔 usage 是实测的就算 true——全没回报时统计行按
    // "usage 未报告"收场,不拿 0 命中糊(缓存诊断单四态里的 not_reported)。
    bool any_reported() const {
        for (const auto& step : steps) {
            if (step.reported) {
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// UserPromptSubmit(prompt 预处理的决策段)
//
// 用户 prompt 送模型前:可阻断(continue=false/exit 2,这一轮不发模型、
// 不算错误),可追加 developer context(原 prompt 不动,注入文本带来源标识
// 单独成块,不串成一坨)。原文自 RunTurn 搬来;背景回流通知的追加也在这。
// ---------------------------------------------------------------------------

struct PromptGate {
    bool blocked = false;              // UserPromptSubmit 拉了闸:本轮不发了
    std::string block_reason;          // 拉闸理由(给用户看)
    std::vector<std::string> additional_context;  // 钩子附加上下文(带标识注入)
};

// 纯编排:dispatcher 为空(没配)直接过,一个字段不动。
// background_notices:后台子代理回流结果(查看态回流单),非空时按
// "不可信参考资料"声明追加进消息尾部——声明原文与 RunTurn 一致。
PromptGate ApplyUserPromptSubmit(hooks::HookDispatcher* dispatcher, const std::string& user_input,
                                 const std::string& background_notices, api::Message& message);

// ---------------------------------------------------------------------------
// TurnRuntime:一轮的聚合核
//
// 每轮 RunTurn 建一份:cancel 旗是轮级(监听线程写、Run 线程读),usage
// 台账轮级(一问一答算一次统计),权限档按当轮快照。会话级状态(always_
// allowed_tools、确认档)由调用方持有,指针传入——轮对象不偷会话的账。
// ---------------------------------------------------------------------------
class TurnRuntime {
public:
    struct Options {
        bool auto_confirm = false;
        PermissionMode permission_mode = PermissionMode::Confirm;
        std::set<std::string>* always_allowed = nullptr;  // 会话级"总是允许"
        std::vector<std::string> allow_commands;
        std::vector<std::string> deny_commands;
        hooks::HookDispatcher* hook_dispatcher = nullptr;
    };

    explicit TurnRuntime(Options options);

    TurnRuntime(const TurnRuntime&) = delete;
    TurnRuntime& operator=(const TurnRuntime&) = delete;

    // ---- 权限 -------------------------------------------------------------
    // 档位裁定(纯);问话本身(菜单/[y/a/N]/悬起 future)在前端。
    PermissionVerdict EvaluatePermission(const runtime::ToolHookDecision& pre, const std::string& name,
                                         const nlohmann::json& input) const;

    // ---- hooks ------------------------------------------------------------
    runtime::ToolHookDecision EmitPreToolUse(const std::string& name, const nlohmann::json& input);
    std::vector<std::string> EmitPostToolUse(const std::string& name, const nlohmann::json& input,
                                             const tools::Tool::Result& result);
    PermissionHookResult EmitPermissionRequest(const std::string& name, const nlohmann::json& input);
    bool has_tool_hooks() const;
    bool has_permission_hooks() const;

    // ---- prompt -----------------------------------------------------------
    PromptGate ApplyUserPromptSubmit(const std::string& user_input, const std::string& background_notices,
                                     api::Message& message);

    // ---- usage ------------------------------------------------------------
    TurnUsageStats usage;

    // ---- 取消 -------------------------------------------------------------
    // ESC/Ctrl+C 的轮级旗:监听线程(另一线程)store,Run 所在线程 load。
    // 原子语义不可省——真机驱动器实测踩过普通 bool 在这条跨线程路径上的
    // 可见性问题(见 turn_runner.hpp RunTurn 注释),acquire/release 堵上。
    std::atomic<bool> cancel{false};
    bool interrupted() const { return cancel.load(std::memory_order_acquire); }
    void request_interrupt() { cancel.store(true, std::memory_order_release); }

private:
    bool auto_confirm_ = false;
    PermissionMode permission_mode_ = PermissionMode::Confirm;
    std::set<std::string>* always_allowed_ = nullptr;
    std::vector<std::string> allow_commands_;
    std::vector<std::string> deny_commands_;
    hooks::HookDispatcher* hook_dispatcher_ = nullptr;
};

}  // namespace lubancode::runtime
