// ChannelIngressStore:入站耐久账(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/message-contracts.md §3-4(去重键
// 三级、入站状态机、append-only journal)。落位 configuration.md §5:
//   <state_root>/<channel>/<account>/ingress/journal.jsonl      append-only
//   <state_root>/<channel>/<account>/ingress/dead-letter.jsonl  旁路账
//
// 记账规矩:
//   - evt 行:一枚入站事件的完整落盘。写成功即 durable(状态机的
//     received→durable 迁移被这一行吸收:落盘前不进任何队列,落盘失败
//     整个 Ingest 报错,调用方不得 ack sidecar)。
//   - tr 行:状态迁移(sid + to + reason + ts)。每次迁移只追加,快照
//     可由 journal 重建——"不拿一份可覆盖 JSON 当唯一真账"。
//   - replay:Open 时逐行重读,重建内存索引(去重键 → sid、delivery_id →
//     sid、状态)。半行(崩溃时写了一半)容错跳过并记 warning;中间坏行
//     同样跳过计数,不崩宿主——账还在,只是那一段查不到。
//   - 写盘原语(QQ 接入单 Q2,§六第一项):JournalWriter PowerLoss——
//     规范化原文与稳定去重键一并落稳后才 ack,掉电不丢受理事实。
//
// 去重键三级(message-contracts.md §3,从上往下退):
//   1. provider_event_id 非空  -> p:<ch>:<acct>:<provider_event_id>(永久)
//   2. message_id 非空         -> m:<ch>:<acct>:<conv>:<message_id>(永久)
//   3. 指纹(前两级都空)      -> f:<sender>:<parts_sha256>:<time_bucket>
//      只作短窗去重(kFingerprintWindowMs 内查),不冒充永久 id。
//
// 依赖:标准库 + nlohmann::json + trajectory/journal(既有提交原语,Q2 起
// 显式引入——channel 库不反向依赖 runtime/app 的铁律不变)。
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "channel/types.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 入站事件状态机(message-contracts.md §4)
// ---------------------------------------------------------------------------

enum class IngressEventState {
    Durable,              // evt 行落盘即 durable(received 瞬态被吸收)
    Authorized,
    Rejected,
    Routed,
    Queued,
    Running,
    Replied,
    CompletedWithoutReply,
    Delivered,
    DeliveryFailed,
    Archived,
    // 旁路终态。
    Duplicate,
    RateLimited,
    Unsupported,
    DeadLettered,
    Cancelled,
};

const char* IngressEventStateName(IngressEventState state);
std::optional<IngressEventState> IngressEventStateFromName(const std::string& name);
bool IsIngressTerminalState(IngressEventState state);
// 主线/旁路迁移合法性(状态机表驱动;终态无出边,Archived 是主线的坟)。
bool CanIngressTransition(IngressEventState from, IngressEventState to);

// ---------------------------------------------------------------------------
// 去重键
// ---------------------------------------------------------------------------

struct DedupeKey {
    int tier = 0;       // 1/2/3(message-contracts.md §3 的级数)
    std::string key;    // 稳定拼法(p:/m:/f: 前缀)
    std::int64_t window_until_ms = 0;  // tier 3 的短窗上界;tier 1/2 永久(0)
};

// 指纹短窗:10 分钟(message-contracts.md §3"短窗",首版钉死不进配置)。
inline constexpr std::int64_t kFingerprintWindowMs = 10 * 60 * 1000;
// 指纹时间桶粒度:10 秒(bucket = provider_at_ms / 10s)。
inline constexpr std::int64_t kFingerprintBucketMs = 10 * 1000;

// 算一枚事件的去重键。parts_sha256_out 可空;给时回填 parts 指纹(第三级
// 用的同一份 digest,诊断可复用)。
DedupeKey ComputeDedupeKey(const ChannelInboundEvent& event, std::string* parts_sha256_out);

// ---------------------------------------------------------------------------
// 账本
// ---------------------------------------------------------------------------

class ChannelIngressStore {
public:
    // 值语义被 mutex 禁:工厂返回 unique_ptr,装配方持指针。
    ChannelIngressStore() = default;
    ChannelIngressStore(const ChannelIngressStore&) = delete;
    ChannelIngressStore& operator=(const ChannelIngressStore&) = delete;

    struct Record {
        std::int64_t sid = 0;             // 账序号(journal 内单调,1 起)
        DedupeKey key;
        std::string parts_sha256;
        ChannelInboundEvent event;
        IngressEventState state = IngressEventState::Durable;
        std::string last_transition_reason;  // 最近一次迁移的稳定名(诊断/dead letter 用)
    };

    struct DeadLetterEntry {
        std::int64_t sid = 0;
        std::string channel_id;
        std::string account_id;
        std::string delivery_id;
        std::string reason;      // 稳定名
        std::int64_t at_ms = 0;
        ChannelInboundEvent event;  // 完整事件留档(dead letter 可 replay)
    };

    struct OpenResult {
        bool ok = false;
        std::string error;
        int skipped_lines = 0;  // replay 容错跳过的行数(半行/坏行)
    };

    // 打开(或新建)账号的 ingress 账。account_dir =
    // <state_root>/<channel>/<account>;journal 在其下 ingress/ 子目录。
    // 失败不落异常:错误经 result 带,返回的 store 处于"只读拒绝写"
    // (write_blocked)状态,调用方决定收还是弃。
    static std::unique_ptr<ChannelIngressStore> Open(const std::filesystem::path& account_dir,
                                                     std::string channel_id,
                                                     std::string account_id,
                                                     OpenResult* result = nullptr);

    bool write_blocked() const { return write_blocked_; }
    const std::string& last_error() const { return last_error_; }

    struct IngestOutcome {
        enum class Status { Accepted, Duplicate } status = Status::Accepted;
        std::int64_t sid = 0;             // Accepted:新账序号;Duplicate:原事件 sid
        DedupeKey key;
        bool ack = false;                 // 两种结局都该 ack sidecar(见下)
    };

    // 落一枚入站事件:durable 写盘 + 去重判定。重复事件不重新落 evt 行,
    // 只追加一行 duplicate 迁移到原事件账上。durable 写失败 → unexpected
    // (调用方不得 ack,sidecar 会按退避重发)。
    // Duplicate 也回 ack:事件已 durable 过,重送即重复,ack 让 sidecar
    // 清 spool(bridge-protocol.md §5 channel.inbound)。
    std::optional<IngestOutcome> Ingest(const ChannelInboundEvent& event);

    // 状态迁移(追加 tr 行)。非法迁移/未知 sid 报错。
    std::optional<std::string> Transition(std::int64_t sid, IngressEventState to,
                                          const std::string& reason);

    // 挪进 dead letter(状态旁路 + dead-letter.jsonl 留档)。
    std::optional<std::string> MoveToDeadLetter(std::int64_t sid, const std::string& reason,
                                                std::int64_t at_ms);

    // ---- 查询(快照,锁内拷贝) ----
    std::vector<Record> Records() const;
    std::optional<Record> FindBySid(std::int64_t sid) const;
    std::optional<std::int64_t> FindByDeliveryId(const std::string& delivery_id) const;
    // 状态计数(/channels、doctor 的水位投影)。
    std::map<std::string, std::size_t> StateCounts() const;
    std::size_t dead_letter_count() const;
    std::vector<DeadLetterEntry> DeadLetters() const;
    std::int64_t next_sid() const;

private:
    std::optional<std::string> AppendLine(const std::string& line);
    // dead-letter 旁路账的行写入(尽力而为:主 tr 行已落,旁路档写不进
    // 不阻塞迁移——last_error_ 留痕)。
    std::optional<std::string> AppendDeadLetterLine(const nlohmann::json& entry);
    void ReplayLocked();

    std::filesystem::path journal_path_;
    std::filesystem::path dead_letter_path_;
    std::string channel_id_;
    std::string account_id_;
    bool write_blocked_ = false;
    std::string last_error_;
    int replayed_bad_lines_ = 0;  // replay 容错跳过的行数(OpenResult 带出)
    // 提交原语(Q2:PowerLoss)。journal 主账 Open 即持柄;dead-letter 惰性。
    std::optional<trajectory::JournalWriter> journal_writer_;
    std::optional<trajectory::JournalWriter> dead_letter_writer_;

    mutable std::mutex mutex_;
    std::int64_t next_sid_ = 1;
    std::deque<Record> records_;  // sid 升序
    // 去重索引:tier1/2 永久;tier3 由 ScanDuplicateLocked 现查(短窗)。
    std::unordered_map<std::string, std::int64_t> permanent_keys_;  // key -> sid
    std::unordered_map<std::string, std::int64_t> delivery_ids_;    // delivery_id -> sid
};

// ---------------------------------------------------------------------------
// SV-05:journal 行合同与 sid 折叠核
// ---------------------------------------------------------------------------

// 一行 ingress journal 的统一解析结果。此前 ReplayLocked /
// ReadChannelIngressProjection / ReadChannelIngressRecentChain 三处各写一套
// JSON/schema/state 分支,同一本坏账在三处得出不同结论(缺 schema 的 tr
// 在状态页凭空建项、未知 schema 的 tr 改写合法 sid 终态、缺 event 的 evt
// 计入诊断、重复 sid 口径不一)。自此行合同只此一份:
//   - 空 行:崩溃尾部没写完换行,Kind::Empty,不计坏行。
//   - 坏 JSON、非 object、缺 schema/schema 非整数/版本不识、缺 t/t 非串、
//     未知 t:Kind::Bad。
//   - evt:缺 sid/dedupe/tier/event、sid/tier 非整数、dedupe 非串、event
//     不是 object 或解码不过 FromJsonStrict:Kind::Bad。
//   - tr:缺 sid/to、sid 非整数、to 非串或状态名不识:Kind::Bad。
//   - dup:重复投递旁注,Kind::Dup(不重建状态,不计坏行)。
struct IngressJournalLine {
    enum class Kind { Empty, Evt, Tr, Dup, Bad };
    Kind kind = Kind::Empty;
    std::int64_t sid = 0;       // Evt/Tr/Dup
    // Evt:
    std::string dedupe;
    int tier = 0;
    std::string parts_sha256;   // 旧账可缺
    ChannelInboundEvent event;  // FromJsonStrict 已验
    // Tr:
    IngressEventState to = IngressEventState::Durable;
    std::string reason;         // 可缺
};

// 解析一行 journal 文本(不含换行)。不抛:一切解码错误落 Kind::Bad。
IngressJournalLine ParseIngressJournalLine(const std::string& text_line);

// sid 状态折叠核:evt 建项(durable 起)、tr 只改已存在 sid、孤儿 tr
// (sid 无对应 evt——崩溃时 evt 半写没落成,后面的 tr 指空号)无声跳过
// 不计坏行、同 sid evt 重落首笔为准(账序破裂,计坏行)。恢复器
// (ChannelIngressStore::Open)、状态页(ReadChannelIngressProjection)、
// 最近链(ReadChannelIngressRecentChain)都经它重放,事件数量与终态天然
// 一致;去重索引、汇总计数、展示裁窗各归各家,不在此。
class IngressLedgerFold {
public:
    struct Entry {
        std::int64_t sid = 0;
        std::string dedupe;
        int tier = 0;
        std::string parts_sha256;
        ChannelInboundEvent event;
        IngressEventState state = IngressEventState::Durable;
        std::string last_reason;  // 最近一次 tr 的 reason(可空)
    };

    // 值语义:行视图整体移入,evt 的完整事件不拷贝。
    void Feed(IngressJournalLine line);

    // 折叠终态:合法 evt 按账序,每枚一条(重复 sid 已并掉)。
    const std::vector<Entry>& entries() const { return entries_; }
    int skipped_lines() const { return skipped_; }
    // 折叠后按 sid 查(最近链的 dead-letter 后处理用);无则 nullptr。
    const Entry* FindBySid(std::int64_t sid) const;

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::int64_t, std::size_t> index_by_sid_;
    int skipped_ = 0;
};

// 只读投影(QQ 接入单 Q2:gateway status 渠道栏用):从 account_dir 下的
// ingress journal 重放状态计数,零建目录零写盘、不持写柄——别的进程持锁
// 写账时读到半行属常态,跳过即可。文件不存在给空投影。
struct ChannelIngressProjection {
    bool ledger_present = false;             // journal 文件在
    std::size_t events = 0;                  // evt 行数(durable 过的事件)
    std::map<std::string, std::size_t> state_counts;  // 状态名 -> 数量
    std::size_t dead_letter = 0;             // dead-letter.jsonl 行数(在才有)
    std::size_t skipped = 0;                 // 重放跳过的坏行数(SV-05;诊断用)
};
ChannelIngressProjection ReadChannelIngressProjection(const std::filesystem::path& account_dir);

// 最近来信链的只读投影(QQBot 静默失败单 P1:channel status 不手翻 JSONL
// 就能看见 来信→准入→执行 的最近账)。同一份 journal 重放逻辑,保留每枚
// 事件的最终状态与最近迁移原因;dead-letter 旁路账一并重放(它才有 at_ms
// ——journal 的 evt/tr 行不带时间戳,来信时间取事件自带的 received_at_ms)。
// 正文/密钥一概不带:只有 sid、状态、原因、时间与会话/发件人标识。
struct ChannelIngressRecentEntry {
    std::int64_t sid = 0;
    std::string state;          // 最终状态名(dead_letter/rejected/...)
    std::string reason;         // 最近迁移原因(稳定名;可空)
    std::int64_t received_at_ms = 0;   // 来信时间(evt 行事件自带)
    std::int64_t dead_letter_at_ms = 0;  // 死信时间(旁路账;非死信为 0)
    std::string conversation_id;
    std::string sender_id;
};
struct ChannelIngressRecentChain {
    bool ledger_present = false;
    std::size_t dead_letter_count = 0;
    std::vector<ChannelIngressRecentEntry> entries;  // 最近 limit 条,sid 降序
};
ChannelIngressRecentChain ReadChannelIngressRecentChain(const std::filesystem::path& account_dir,
                                                        std::size_t limit);

}  // namespace lubancode::channel
