// 统一原子写(src 重复职责收口审计 P1"原子写文件有多套私房实现"):
// gateway/manifest/telemetry/derived_store/agent/mcp/workflow/config 各养一
// 份"tmp + rename"的私房协议,失败保证已经分叉——有的固定 .tmp(并发互踩),
// 有的替换失败先删正式文件再 rename(留出文件不存在的窗口,不能再叫原子
// 替换),有的另造 .tmp + .old 搬移回滚。这里收成唯一的平台件:
//   - 唯一临时名:<target>.<pid>-<序号>.tmp,同目录(同文件系统),同目标
//     的并发/重入写不再互踩临时件;
//   - close 检查:写完显式 flush + close + 查错,close 上的失败不放行;
//   - 平台原子替换:走 ReplaceFileAtomically(Windows MoveFileExW
//     REPLACE_EXISTING|WRITE_THROUGH / POSIX rename),任何失败路径都不
//     先删正式文件换取成功;
//   - 失败清理:自己的临时件在失败路径上删净,不留孤尾;
//   - 结构化错误:稳定码 + 人话,调用方按码记账,不解析文案;
//   - 提交阶段合同(FD-04):失败不再一律宣称"target 保持原样"。换名前
//     的失败(建目录/开临时件/写临时件/替换)确是 target 原样;换名后的
//     目录刷盘失败是新内容已可见、只差耐久确认。错误带 outcome 字段把
//     两个阶段分开说,上层按阶段处置:后者不得当"未写盘"回滚内存,更
//     不得删新文件换回旧内容——那再开一个非原子窗口。
//
// 两档保证,明分合同,不许混叫"原子":
//   - AtomicVisibility:读者要么看到旧整份、要么看到新整份,绝无半截;
//     进程崩溃/断电不额外承诺(内核页缓存层面可能丢最近的写)。
//   - ProcessCrashDurability:在可见性原子之上,close 前文件数据落盘
//     (fsync/_commit),换名后父目录条目落盘(目录 fsync)。保到"进程崩
//     了/机器摔了,换名要么已见效要么没生效"这一层;不承诺写到一半的
//     断电一致性之外的东西。绝大多数调用点(manifest/快照/派生缓存)
//     AtomicVisibility 就够;真正的事实账(信任账、pairing 账)按需升档。
//
// JSON 序列化留在业务层:这里只管"字节可靠落盘 + 原子可见",不管字节是
// 什么。
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 原子写的持久档位(见文件头注释:两档保证,明分合同)。
enum class WriteDurability {
    AtomicVisibility,        // 换名原子可见;崩溃/断电不额外承诺
    ProcessCrashDurability,  // 加上文件 fsync + 目录 fsync
};

// 提交阶段合同(FD-04):一次原子写走到哪一步。成功回执与失败错误共用
// 这一份词表,上层不得再凭 has_value 猜盘面。
enum class WriteOutcome {
    NotCommitted,                     // 换名未发生:target 保持原样(或仍不存在)
    CommittedDurabilityNotRequested,  // 换名已生效:AtomicVisibility 档成功,耐久没承诺也没请求
    CommittedDurabilityUnconfirmed,   // 换名已生效:新内容可见,父目录刷盘未确认(durability_flush_failed)
    CommittedDurable,                 // 换名已生效:文件数据与父目录条目都确认落盘
};

// 失败的短拒分类(FD-04):Windows 上防病毒/索引过滤驱动会让原子换名被
// 短拒数十毫秒(work_pump.cpp / workspace/manifest.cpp 的三案 CI 实测注
// 记:320 次换名被拒 48-57 次)。短拒有界重试可过;真失败重试同样结果。
enum class WriteFailureKind {
    Permanent,        // 真失败:权限/路径形状/盘满/跨设备,照实报死
    TransientReject,  // 短拒:调用方有界重试(work_pump/manifest/cursor 同款纪律)
};

// 结构化错误:code 是稳定码(机器可读),message 是人话(日志/诊断用)。
// 稳定码集合:
//   atomic.mkdir_failed    父目录建不成
//   atomic.tmp_open_failed 临时文件打不开(父目录只读/被占/路径形状坏)
//   atomic.tmp_write_failed  写入/close/文件刷盘阶段失败(盘满/配额/IO 错)
//   atomic.replace_failed  原子替换失败(目标被占/跨设备/权限;Windows 短拒类)
//   atomic.durability_flush_failed  换名已生效,父目录刷盘未确认(outcome=
//                                   CommittedDurabilityUnconfirmed——不是未写盘)
struct AtomicWriteError {
    std::string code;
    std::string message;
    // 失败发生时写走到哪一步:NotCommitted = 旧文件未动,重写安全;
    // CommittedDurabilityUnconfirmed = 新文件已可见,不得当未写盘处理,
    // 也不得靠删新文件回滚(那再开一个非原子窗口)。
    WriteOutcome outcome = WriteOutcome::NotCommitted;
    // 短拒可重试还是真失败(见 WriteFailureKind)。
    WriteFailureKind failure_kind = WriteFailureKind::Permanent;
};

// 成功回执:换名生效后耐久确认到哪一档。AtomicVisibility 档成功回
// CommittedDurabilityNotRequested——上层不得记作"已确认耐久"。
struct AtomicWriteReceipt {
    WriteOutcome outcome = WriteOutcome::CommittedDurabilityNotRequested;
};

// 把 bytes 原子写到 target。父目录不在就建(建目录不是原子的,首建目录后
// 崩溃会留下空目录——无害,重写即愈)。成功后 target 是 bytes 的整份新内
// 容,临时件不复存在,回执按档记耐久;失败时错误带阶段(见
// AtomicWriteError::outcome)与稳定码:
//   - outcome == NotCommitted:target 保持原样(或维持不存在),自己的
//     临时件删净,重写安全;
//   - outcome == CommittedDurabilityUnconfirmed(仅 atomic.durability_
//     flush_failed):换名已生效,target 已经是新内容,失败只说明父目录
//     条目没确认落盘(断电那一层)。不许按未写盘回滚,更不许删 target
//     换回旧内容。
// target 已存在/不存在都合法;target 是目录按 replace_failed 报错,绝不
// 删除目录换成功。裸文件名(无父段)落在进程当前目录:ProcessCrash-
// Durability 档照刷文件数据,父目录一步无路径可开、按合同视为已确认
// (回 CommittedDurable;现行行为,FD-04 钉死)。
std::expected<AtomicWriteReceipt, AtomicWriteError> AtomicWriteFile(
    const std::filesystem::path& target, std::string_view bytes,
    WriteDurability durability = WriteDurability::AtomicVisibility);

// ---- 测试注入面(仅 tests/ 使用,生产代码不得调用) -------------------------
// 失败注入分阶段(FD-04 验收):注入只替代对应的真实刷盘动作,其余阶段照
// 常。置回 false 恢复真实刷盘。读侧线程安全(原子读),装/卸约定在测试
// 主线完成。
// SetFileFlushFailureForTest(true):ProcessCrashDurability 档的文件刷盘
// (换名之前)一律报失败——走 tmp_write_failed / NotCommitted 路径。
void SetFileFlushFailureForTest(bool fail);
// SetDirectoryFlushFailureForTest(true):换名之后的父目录刷盘一律报失败
// ——走 durability_flush_failed / CommittedDurabilityUnconfirmed 路径。
void SetDirectoryFlushFailureForTest(bool fail);

}  // namespace lubancode::platform
