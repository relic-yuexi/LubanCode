// HookOutbox(四层生命周期单 P2):可靠 Post 的 durable outbox/ack 账本。
//
// 合同(单子 §4.3):
//   - Post 型观察事件(IsPostObservationEvent)在真跑 handler 之前先落
//     outbox(pending 行),handler 跑完(无论成败)落 ack 行销账;
//   - 幂等键 (event_id, handler_definition_hash):同一事件对同一 handler
//     重放只记一次(RecordPending 判重返回 0,不重复落盘);
//   - 对外部副作用承诺 at-least-once,不承诺 exactly-once——崩溃后 pending
//     行留在账上,是"至少投过一次"的证据;本单只提供账面判重与待办查询,
//     自动重投(重跑 handler)不接线,文档如实写明。
//
// 落盘形状:JSONL 追加,一行一事。
//   {"kind":"pending","id":N,"event_id":"hookrun_...","handler_hash":"...","event":"PostToolUse","ts":...}
//   {"kind":"ack","id":N,"ts":...}
// 打开时重放全文件重建账面,并把已 ack 的行压实掉(只留 pending 重写)——
// 账本不随会话数无界膨胀,崩溃残留的 pending 行留作待办。
//
// 线程安全:内部一把互斥,RecordPending/Ack/查询都可跨线程调。I/O 失败
// 静默降级(账面照记,盘上丢一行)——outbox 是可靠性增强,不是硬闸,不因
// 磁盘毛病拦会话;打开成败由调用方在启动诊断里报。
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace lubancode::hooks {

class HookOutbox {
public:
    // 缺省账本位置:<home>/.lubancode/hooks-outbox.jsonl。拿不到 home 回
    // nullopt(调用方降级为不挂 outbox)。
    static std::optional<std::filesystem::path> DefaultPath();

    // 打开(或建)账本并回实例;打不开回 nullptr,零行为降级。
    static std::shared_ptr<HookOutbox> Open(const std::filesystem::path& path);

    // 记一条待执行投递。幂等键 (event_id, handler_definition_hash) 已在
    // 账上(pending 或 acked)时返回 0 且不落盘——重放只记一次。否则落
    // pending 行,回条目号(1 起)。
    std::uint64_t RecordPending(const std::string& event_id, const std::string& handler_definition_hash,
                                const std::string& event_name);

    // 销账:handler 跑完(成功、失败、超时、起不来都算"跑完了这一趟")
    // 落 ack 行。认不得的号安静忽略(幂等)。
    void Ack(std::uint64_t entry_id);

    // 幂等键是否已在账上(pending 或 acked)。
    bool Contains(const std::string& event_id, const std::string& handler_definition_hash) const;

    // 尚未 ack 的条目数(诊断/测试;崩溃残留的待办)。
    std::size_t pending_count() const;
    // 账面条目总数(含已 ack)。
    std::size_t total_count() const;
    // 载入时丢弃的坏行数(诊断)。
    std::size_t dropped_lines() const;

private:
    HookOutbox() = default;

    // 打开时重放既有文件:好行进账,坏行计数丢弃;已 ack 的行压实(重写
    // 只留 pending)。重写失败不拦——账面以内存为准,追加流照开。
    void LoadAndCompact();

    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream append_;  // 追加流;LoadAndCompact 后打开
    std::map<std::pair<std::string, std::string>, std::uint64_t> keys_;
    std::set<std::uint64_t> pending_;
    std::uint64_t next_id_ = 1;
    std::size_t dropped_lines_ = 0;
};

}  // namespace lubancode::hooks
