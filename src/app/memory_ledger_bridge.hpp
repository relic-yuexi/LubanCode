// memory 落账口在 Trajectory 账上的实现(存储 v2 P0-3 起,v3 接入 T08/
// V3-GAP-03)。
//
// memory::MemoryAccounting 是 memory 域自持的纯接口;这里把它接到
// TrajectorySessionLedger,按场的格式分派:
//   - v3 场(v3_main_writer 在):走受管 writer/context 服务,不再拿
//     "ledger.main()==nullptr"当召回失败——那是旧 Recorder 时代的假设,
//     v3 场一犯就把 memory-on 场的召回整轮丢掉(候选非空却一个不注)。
//       * 召回注入(主会话):正文快照先落 display=hidden 的正式 user
//         消息(origin=context_runtime,不冒充人类输入,重放不再触发
//         人类召回),AdmitMessages 接纳进链(context.input.applied),
//         再落 memory.recall.injected 事实(memoryId/revision/hash/
//         messageRef)——恢复拿这枚快照解释旧请求,不用新 topic 正文
//         倒推;后续 model.request.prepared 的 inputMessageRefs 沿链自然
//         带上它。快照提交失败(消息/接纳落不稳)本轮不注入该条,
//         主任务按 optional 策略放行(§9.2"不得注了却无账")。
//       * 派工冻结(target_run_id 非空):正文内联或内容寻址 blob 落
//         memory.recall.injected(targetRunId),父账不接纳进链——父模型
//         没见过这段,链上冒领就是假账;子账侧的完整隐藏消息/采用链归
//         总设计 §4.71(T13-M),不在本桥。
//       * 写入因果边:memory.save.requested 只记"谁发起了一笔写";排队
//         成败是 memory.write.receipted(MemoryTurnLedger),落盘回执在
//         workspace lifecycle(memory.save.committed)——三态按真实回执
//         分账,worker 从不直接抢写 Session。
//   - v2 场(旧 Recorder 在):老路原样保留(EventEnvelope/context.
//     injected/memory.save.requested),旧档消费期不动,退役归 T02-B。
#pragma once

#include <expected>
#include <string>

#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"

namespace lubancode::app {

class MemoryLedgerBridge final : public memory::MemoryAccounting {
public:
    explicit MemoryLedgerBridge(runtime::TrajectorySessionLedger& ledger);
    ~MemoryLedgerBridge() override = default;

    MemoryLedgerBridge(const MemoryLedgerBridge&) = delete;
    MemoryLedgerBridge& operator=(const MemoryLedgerBridge&) = delete;

    std::expected<void, std::string> RecordRecallInjection(
        const memory::InjectedMemoryRecord& record) override;
    std::string RecordSaveRequested(const memory::SaveLedgerNote& note) override;
    std::string current_session_id() const override;

private:
    // v3 场的两个落点;v2 老路在 cpp 的 RecordXxxV2。
    std::expected<void, std::string> RecordRecallInjectionV3(
        trajectory::v3::V3Writer& writer, const memory::InjectedMemoryRecord& record);
    std::string RecordSaveRequestedV3(trajectory::v3::V3Writer& writer,
                                      const memory::SaveLedgerNote& note);

    runtime::TrajectorySessionLedger& ledger_;
};

}  // namespace lubancode::app
