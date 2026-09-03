// HookDispatcher:hooks 框架的调度中枢。业务层只发事件
//   dispatcher.Emit(HookEvent::PreToolUse, payload)
// 解析、匹配、信任、并发、超时、输出解析与决策归并全收在这里;业务层
// 不得再亲自遍历配置数组、起进程、猜退出码。
//
// 并发:同一事件下命中的同步 handler 各起一线程并发跑,全部收齐后按
// 定义序(来源 → 声明次序)归并——决策与日志顺序稳定,不受完成先后
// 影响。一只 hook 慢/挂,不阻止别的同时启动,但 Emit 整体等最慢的一只
// (同步事件的语义就是"收齐再走")。
//
// 退出码(v2):0 成功(stdout 有 JSON 照事件 schema 解)、2 阻断(stderr
// 作理由)、其它失败按 failure_policy。legacy adapter:任意非零仍拦
// (pre_tool)/只警告(post/session)、LUBAN_TOOL_* 照导、固定 30 秒。
//
// async handler 本期解析、展示、记录,但不执行(skipped_async)——安全点
// 投递是第六步;不执行就不可能拿 async 做权限决定,也不假装支持。
//
// 线程模型:Emit 由宿主主线程同步调用(与确认回调、工具执行同线程),
// 不与自身并发;成员无锁。
#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hooks/loader.hpp"
#include "hooks/protocol.hpp"
#include "hooks/trust.hpp"
#include "hooks/types.hpp"

namespace lubancode::hooks {

class HookDispatcher {
public:
    HookDispatcher() = default;

    // 拷贝/移动:定义表、信任账、上下文与账本照搬;锁与后台投递队列不跟
    // (std::mutex 不可移,新实例自备;投递队列属原实例的交接通道)。生产
    // 路径 dispatcher 由 unique_ptr 单持,这份语义只服务测试的按值装配。
    HookDispatcher(const HookDispatcher& other);
    HookDispatcher& operator=(const HookDispatcher& other);
    HookDispatcher(HookDispatcher&& other);
    HookDispatcher& operator=(HookDispatcher&& other);

    // 装载(会话启动一次;/hooks reload 重装)。base_context 是会话级公共
    // 字段(session_id/cwd/…),turn/agent 级字段之后 UpdateContext 覆写。
    // 返回装载期的提示(未信任项目 hook 数等),调用方决定怎么打给用户。
    struct ConfigureResult {
        bool has_untrusted_project = false;
        bool has_disabled = false;
        int definition_count = 0;
    };
    ConfigureResult Configure(LoadedHooks loaded, HookTrustStore trust, HookContext base_context);

    // 没有任何定义时为 true——调用方(回调装配层)据此完全不设 hook 回调,
    // 行为与"没有 hooks 系统"逐字节一致。
    bool Empty() const { return definitions_.empty(); }

    // 某事件有没有可能命中(含未信任/禁用的——它们命中后记 skipped,不是
    // 不存在)。装配层用来决定要不要挂回调。
    bool HasHandlersFor(HookEvent event) const;

    // 发射一个事件。无命中(或全被跳过)也返回完整记录,决策字段保持
    // 缺省——调用方照常往下走,只把记录入账。
    HookEventResult Emit(HookEvent event, const HookPayload& payload);

    // 带上下文覆写的发射:子代理(SubagentStart/Stop 与子代理内的工具
    // 事件)用——stdin JSON 里的 agent_id/agent_type/parent_agent_id 得是
    // 这只子代理的,不是主会话那份。ctx 从 context() 拷贝后改 agent 字段
    // 再传进来。只在宿主主线程同步调用,不加锁。
    HookEventResult EmitWith(HookEvent event, const HookPayload& payload, HookContext ctx);

    // 会话上下文更新(turn_id 每轮换;子代理触发时带 agent_id/type)。
    void UpdateContext(HookContext context) { context_ = std::move(context); }
    const HookContext& context() const { return context_; }

    // ---- /hooks 管理面 --------------------------------------------------
    const std::vector<HookDefinition>& definitions() const { return definitions_; }
    const HookDefinition* FindDefinition(int id) const;
    // 每只定义最近一次运行(没跑过 = nullopt)。
    const HookRunRecord* LastRecordFor(int definition_id) const;
    // 最近运行记录(新在前),cap 截断。
    std::vector<HookRunRecord> RecentRecords(std::size_t cap) const;

    // 动作:信任当前 hash / 撤信 / 禁用-启用(managed 不可禁)。成功后
    // definitions_ 里的镜像同步更新。id 不存在返回 false。
    bool TrustDefinition(int id);
    bool UntrustDefinition(int id);
    bool SetDefinitionDisabled(int id, bool disabled);

    const HookTrustStore& trust_store() const { return trust_; }

    // 生成一次发射的 hook_run_id(单调递增 + 随机分量太重,用"进程内计数
    // + 时间戳"就够对账)。只标单次 Hook 发射;不得拿来给 turn_id 代班——
    // canonical turn id 由回合装配层在 UserPromptSubmit 之前从 IdAuthority
    // 定一枚,四本账(hooks/事件流/trace/轨迹)共用。
    static std::string NextHookRunId();

    // ---- 后台(外挂)执行面 ----------------------------------------------
    // 契约:后台线程不碰 dispatcher 的账本(recent_/last_record_)与定义表。
    // 它拿一份只读策略快照在后台执行器里真跑钩子(EmitDetached),跑完的
    // 记录只投递(PostExternalRecords);主会话在安全点(轮起/轮收//hooks)
    // 调 AdoptExternalRecords 归并落账、刷 UI。同步决策(PreToolUse/
    // PermissionRequest)由快照执行给出,不静默绕过。

    // 主线程调用:拷一份当前定义表(含信任/禁用账)。后台执行器存着这份
    // 快照跑——会话中途 trust/disable 只影响之后新起的快照,不在跑的
    // 那份不追改(只读语义)。
    std::vector<HookDefinition> PolicySnapshot() const;

    // 对给定定义表跑一次事件:匹配、并发执行、按定义序归并,与 Emit 同一
    // 个核,但不落账、不读任何成员——任意线程可调(后台执行器用)。
    static HookEventResult EmitDetached(const std::vector<HookDefinition>& definitions, HookEvent event,
                                        const HookPayload& payload, const HookContext& ctx);

    // 后台线程调用:投递跑完的记录与告警(只入队,不碰账本)。warning 是
    // 明示降级这类要给用户看的话,随记录一起在安全点被收走。
    void PostExternalRecords(std::vector<HookRunRecord> records, const std::string& warning = {});

    // 主会话安全点调用:把积压的投递归并进账本(recent_/last_record_),
    // 返回本次收编的记录与告警(调用方打一行报信)。
    struct ExternalAdoption {
        std::vector<HookRunRecord> records;
        std::vector<std::string> warnings;
    };
    ExternalAdoption AdoptExternalRecords();

    // 有没有积压未归并的后台投递(安全点判断用)。
    bool HasExternalRecords() const;

private:
    // 匹配:matcher(缺省/*/精确/竖线集合/显式 regex)对 match_value。
    // 事件没有匹配字段时(match_value 空),只有 */缺省能命中。纯函数。
    static bool MatcherHits(const HookDefinition& def, const std::string& match_value);

    // Emit/EmitDetached 共用的执行核:定义表由参数带入,不碰成员。
    static HookEventResult RunEventCore(const std::vector<HookDefinition>& definitions, HookEvent event,
                                        const HookPayload& payload, const HookContext& ctx);

    HookEventResult EmitImpl(HookEvent event, const HookPayload& payload, const HookContext& ctx,
                             bool context_override);

    std::vector<HookDefinition> definitions_;
    HookTrustStore trust_;
    HookContext context_;
    std::deque<HookRunRecord> recent_;         // 新在头,容量 kRecentCap
    std::map<int, HookRunRecord> last_record_;  // definition id -> 最近一次
    static constexpr std::size_t kRecentCap = 100;

    // 定义表的快照读与主线程改动(Configure/Trust*/SetDisabled)互斥。Emit
    // 走主线程不加锁(与改动同线程,调用约定互斥);PolicySnapshot 可能从
    // 起后台任务的路径调,拿锁读。
    mutable std::mutex definitions_mutex_;
    // 只护 external_pending_——后台线程投递与主会话归并之间的交接队列。
    mutable std::mutex external_mutex_;
    struct ExternalPending {
        std::vector<HookRunRecord> records;
        std::vector<std::string> warnings;
    };
    std::deque<ExternalPending> external_pending_;
};

}  // namespace lubancode::hooks
