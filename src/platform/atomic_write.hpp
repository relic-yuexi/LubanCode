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
//   - 结构化错误:稳定码 + 人话,调用方按码记账,不解析文案。
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

// 结构化错误:code 是稳定码(机器可读),message 是人话(日志/诊断用)。
// 稳定码集合:
//   atomic.mkdir_failed    父目录建不成
//   atomic.tmp_open_failed 临时文件打不开(父目录只读/被占/路径形状坏)
//   atomic.tmp_write_failed  写入/close 阶段失败(盘满/配额/IO 错)
//   atomic.replace_failed  原子替换失败(目标被占/跨设备/权限)
struct AtomicWriteError {
    std::string code;
    std::string message;
};

// 把 bytes 原子写到 target。父目录不在就建(建目录不是原子的,首建目录后
// 崩溃会留下空目录——无害,重写即愈)。成功后 target 是 bytes 的整份新内
// 容,临时件不复存在;失败时 target 保持原样(或维持不存在),自己的临时
// 件删净,error 带稳定码。target 已存在/不存在都合法;target 是目录按
// replace_failed 报错,绝不删除目录换成功。
std::expected<void, AtomicWriteError> AtomicWriteFile(const std::filesystem::path& target,
                                                      std::string_view bytes,
                                                      WriteDurability durability = WriteDurability::AtomicVisibility);

}  // namespace lubancode::platform
