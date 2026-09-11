// v3 事件账 writer profile(Workflow 接入 Session v3 第一棒)。
//
// agent 会话账(V3Writer)要求首行 system、message/event 两类行;编排账
// 不是 agent——只写 type=event 的编排事实,不造 system、不写 message、
// 不偷填假 session 字段(设计 §三"编排账"条)。本件把这只"受校验的
// 事件账 profile"落成原语:
//   - 信封/seq/哈希链/canonical JSON 全套共用 v3 既有纪律(envelope/
//     schema3/journal writer);
//   - Start 以调用方给的开账事实事件(create-new)开卷,首行即事件;
//   - Continue 整卷验(严格解析 + 语义校验 + seq 从 1 连续 + 哈希衔接 +
//     只认 event 行——出现 message 行即拒),重放尾状态后续写;
//   - Append 只收 EventDraft;message 行在这个 profile 里写不进去。
//
// 身份语义(schema 文档 §四 workflow 条):sessionId = 拥有这本账的领域
// 身份(workflow 场 = workflowRunId),runId = 当前写卷的执行流(workflow
// 场 = orchestrationSegmentId)。域 id(outputId/checkpointId 一类)由
// 调用方(领域账本)铸造与恢复,本件只管行身份与链。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/schema3.hpp"
#include "trajectory/v3/writer.hpp"  // WriteReceipt/V3Clock/EventDraft/Durability

namespace lubancode::trajectory::v3 {

struct V3EventLedgerOptions {
    std::string writer_version = "trajectory-v3-event-ledger-1";
    // 注入提交失败(测试专用;生产恒空):返回稳定码则该枚提交按 IoFailed
    // 收(句柄随后 broken)。锁内调用,须廉价无副作用。
    std::function<std::optional<std::string>()> inject_io_failure;
};

// 验卷报告(事件账 profile 版:无上下文链视图,只报行/链状态)。
struct V3EventLedgerReport {
    bool ok = false;
    bool truncated_tail = false;  // 尾行缺 '\n'(崩溃截断)
    std::uint64_t lines = 0;
    std::string error_code;
    std::string message;
    std::string session_id;         // 首行信封读回
    std::string run_id;             // 首行信封读回
    std::uint64_t last_seq = 0;
    std::string last_line_hash;
};

class V3EventLedger {
public:
    V3EventLedger() = default;
    V3EventLedger(V3EventLedger&&) noexcept;
    V3EventLedger& operator=(V3EventLedger&&) noexcept;
    V3EventLedger(const V3EventLedger&) = delete;
    V3EventLedger& operator=(const V3EventLedger&) = delete;
    ~V3EventLedger();

    // 开新卷:create-new(已存在即失败);opening_event 是本卷首行的域开账
    // 事实(如 workflow.definition.loaded)。session_id/run_id 进信封。
    static std::expected<V3EventLedger, std::string> Start(
        const std::filesystem::path& jsonl_path, std::string_view session_id,
        std::string_view run_id, EventDraft opening_event,
        V3EventLedgerOptions options = V3EventLedgerOptions{}, const V3Clock* clock = nullptr);

    // 续卷:整卷验(严格解析 + 语义校验 + seq 连续 + 哈希衔接 + 只认
    // event 行),重放到尾后续写。验不过拒开(错误码 v3ledger.*);尾行
    // 截断明报拒开(删尾修复归读取侧,写入侧不偷偷裁)。
    static std::expected<V3EventLedger, std::string> Continue(
        const std::filesystem::path& jsonl_path, V3EventLedgerOptions options = {},
        const V3Clock* clock = nullptr);

    // 追加一枚事件(校验 -> 发号 -> 哈希 -> 落盘 -> 状态前移)。失败不动
    // 状态(IoFailed 时置 broken,后续提交空收)。
    WriteReceipt Append(EventDraft draft, Durability durability = Durability::ProcessCrash);

    // 行身份发号:<prefix>-<六位号>。计数从卷内 eventId 前缀恢复,续卷
    // 不撞号。域 id(outputId 等)归领域账本,不经这。
    std::string NextId(std::string_view prefix);

    const std::filesystem::path& path() const;
    std::uint64_t next_seq() const;  // 下一行将拿到的 seq
    std::uint64_t last_seq() const;  // 已落稳的最后一行 seq(0 = 空)
    std::string last_line_hash() const;
    bool broken() const;
    const std::string& session_id() const;
    const std::string& run_id() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit V3EventLedger(std::unique_ptr<Impl> impl);
};

// 只读验卷(事件账 profile):VerifyV3File 的同门纪律,去掉 agent 会话
// 语义(无 system 首行要求、无上下文链重放),加"只认 event 行"。
V3EventLedgerReport VerifyV3EventLedgerFile(const std::filesystem::path& path);

}  // namespace lubancode::trajectory::v3
