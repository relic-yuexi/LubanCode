// P0-3(存储 v2):memory 落账口在 Trajectory 账上的实现。
//
// memory::MemoryAccounting 是 memory 域自持的纯接口;这里把它接到
// TrajectorySessionLedger:
//   - 召回快照:正文先写 session artifacts 的内容寻址 blob(≤512B 按
//     合同 §四内联),再落 context.injected 事件;写不稳回错,调用方本轮
//     不注入该条(§9.2 memory.recall_snapshot_failed,不得"注了却无账")。
//   - 写入因果边:memory.save.requested 落 main stream,回全限定事件引用
//     (workspace/session/run/event),进 job 的 source ref。
//
// 不改 trajectory_session 的任何口子——只用既有公口(main() recorder、
// session_dir()、session_id()),P0-2 正在那边动工,接缝能不碰就不碰。
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
    runtime::TrajectorySessionLedger& ledger_;
};

}  // namespace lubancode::app
