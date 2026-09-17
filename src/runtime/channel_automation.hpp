// ChannelAutomationBridge + 聊天侧任务工具(QQ 接入单 Q5 §11.2):渠道
// 会话的模型与 automation 域之间唯一的桥。
//
// 面上的规矩(todo §十一,冻结面 contracts.md §11/§13 不动):
//   - 工具只认本桥:模型经 create_reminder/list_reminders/cancel_reminder
//     落 automation 域命令(CreateJob/CancelJob——V2 的 store 合同与幂等键
//     原样复用,不旁路改账)。工具名进 QQ 模板的 tools.allow,走五层交集
//     (ApplyChannelToolPolicy)与逐轮闸——未配对 sender 进不了模型,自然
//     拿不到工具;本地 CLI/自动任务路没有渠道上下文,一律 fail closed。
//   - 归属:job 落创建者三元组(channel/account/sender 配对身份);查询与
//     取消只操作归属自己的任务,跨用户不泄露不越权(§11.2 "所有查询/修改
//     核 job 归属")。
//   - 幂等:创建键 = 渠道域 + 账号 + 来信消息 id(同信重发/同轮重调不双建,
//     同键异规格 conflict 如实回);取消键同式派生。
//   - 交付目标:job 落创建时会话三元组,到点结果投回该会话(投递政策在
//     ChannelWorkPump:主动消息/回复窗分型,不在工具面)。
//   - 时间解析归模型:模型把"今晚八点"折成 at_ms(UTC 毫秒)或受限 cron
//     + 显式时区;宿主只验证(过期明拒、坏 cron/坏时区明拒、回执带完整
//     日期与 jobId),不猜不改(§11.2 "不能静默改成明天")。
//
// 线程面:泵同步单线程(CompositeGatewayPump 串行 tick),工具执行在
// 渠道轮内、TurnScope 存续期间——桥不设锁。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/automation_store.hpp"
#include "channel/types.hpp"
#include "tools/registry.hpp"

namespace lubancode::runtime {

class ChannelAutomationBridge {
public:
    // 本轮渠道上下文(泵在执行渠道轮前冻结递进):工具的归属与幂等键
    // 只认这份账,模型不可伪造。
    struct TurnContext {
        std::string channel_id;
        std::string account_id;
        std::string conversation_id;
        channel::ConversationKind conversation_kind = channel::ConversationKind::Direct;
        std::string sender_id;  // 配对身份(路由信封的 sender)
        std::string message_id; // 触发来信(创建幂等键原料)
        std::int64_t received_at_ms = 0;
    };

    // store 为空 = automation 域不在(软档装配/测试):工具 fail closed。
    explicit ChannelAutomationBridge(
        gateway::AutomationStore* store = nullptr,
        std::function<std::int64_t()> now_ms = nullptr);
    void set_store(gateway::AutomationStore* store);

    // 渠道轮的上下文Scope:构造落账、析构清——泵用它在 ProcessWorkItem
    // 包住整轮执行。终端路/自动任务路不开 Scope,工具必拒。
    class TurnScope {
    public:
        TurnScope(ChannelAutomationBridge& bridge, const TurnContext& context);
        ~TurnScope();
        TurnScope(const TurnScope&) = delete;
        TurnScope& operator=(const TurnScope&) = delete;

    private:
        ChannelAutomationBridge& bridge_;
    };

    // ---- 工具操作(输入校验 + 归属闸 + 幂等;失败给模型人话) -------------
    struct ToolOutcome {
        bool ok = false;
        bool duplicate = false;   // 幂等命中(同键同载荷回原回执)
        std::string error_code;  // 稳定码(automation.* / channel_job.*)
        std::string error;       // 人话(给模型转述)
        nlohmann::json payload;  // ok 时的结构化回执
    };
    ToolOutcome CreateReminder(const nlohmann::json& input);
    ToolOutcome ListReminders();
    ToolOutcome CancelReminder(const nlohmann::json& input);
    ToolOutcome GetCurrentTime() const;

    const gateway::AutomationStore* store() const { return store_; }

private:
    const TurnContext* Current() const;

    gateway::AutomationStore* store_ = nullptr;
    std::function<std::int64_t()> now_ms_;
    std::optional<TurnContext> current_;
};

// 注册渠道任务工具与只读时钟(幂等:同名已在不重复注册)。
// 任务操作需要 TurnScope；读时钟不依赖任务账或渠道身份。
void RegisterChannelAutomationTools(tools::ToolRegistry& registry,
                                    std::shared_ptr<ChannelAutomationBridge> bridge);

// 工具注册名(模板/测试对账用)。
inline constexpr const char* kChannelCreateReminderTool = "create_reminder";
inline constexpr const char* kChannelListRemindersTool = "list_reminders";
inline constexpr const char* kChannelCancelReminderTool = "cancel_reminder";
inline constexpr const char* kChannelGetCurrentTimeTool = "get_current_time";

}  // namespace lubancode::runtime
