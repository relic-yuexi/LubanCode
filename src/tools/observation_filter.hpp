// 观察边界(真机实测 P2-5):search/read_file 默认不吞运行时自产的调试
// 证据。子代理把日志放进项目可搜目录(.evidence/subagents 这类)时,它
// 自己的 search 会读回 subagent-N.log——日志里装着它自己的工具流,读一
// 口就递归膨胀(实测 584,931 字节)。
//
// 边界口径(单子验收原文):
//   1. 目录名叫 .evidence 的,整棵子树默认在边界内(真机实测的证据目录
//      约定);与 search 原有的 .git/build/node_modules 跳过表同一层语义,
//      但那表只管 search 的目录遍历,这里给 read_file 与单文件点名共用。
//   2. 运行时登记的"子代理日志目录"(LUBANCODE_DEBUG_SUBAGENT 指到的
//      任意目录,AgentTool 的 TraceBackend 开日志时登记),不管它叫什么
//      名字——实测正是用户把它指进项目内才出的递归。
// 默认过滤只挡"无意间搜到":path 逐字点名到边界内的文件/目录,视为显式
// 要读,放行,但读前告知体积(超过 256KB 劝阻)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::tools {

// 证据目录名:.evidence(真机实测的会话/子代理产物落点)。
inline constexpr std::string_view kEvidenceDirName = ".evidence";

// 显式点名读取的体积劝阻线(单子:"超过阈值如 256KB 劝阻")。
inline constexpr std::uintmax_t kObservationDiscourageThreshold = 256 * 1024;

// 进程级登记账:谁在往项目里写运行时证据,谁登记。名字口径(.evidence)
// 不走账,天然常开。
class ObservationBoundary {
public:
    static ObservationBoundary& Instance();

    // 登记一枚运行时证据目录(尽力规范化:weakly_canonical 失败退
    // absolute)。线程安全——后台子代理线程也会来登记。
    void AddExcludedDir(const std::filesystem::path& dir);

    // 只清登记账(测试隔离用);.evidence 名字口径不随账清。
    void Reset();

    // 登记账快照(ripgrep 迁移单 P0-3):规范化过的绝对目录,拷贝一份走。
    // search 的新后端拿它生成 walker 排除 glob,让子代理日志目录真被剪枝
    // 而不是解析后丢命中。只含运行时登记的目录;.evidence 名字口径不走账
    // (那是硬排除表的事,与 Contains 的名字口径分工一致)。线程安全。
    std::vector<std::filesystem::path> ExcludedDirsSnapshot() const;

    // abs_path(绝对路径)是否落在观察边界内:任一路径段叫 .evidence,
    // 或落在某枚已登记目录之下(含目录本身)。
    bool Contains(const std::filesystem::path& abs_path) const;

private:
    ObservationBoundary() = default;
    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> dirs_;  // 规范化过的绝对目录
};

// 便捷口:相对/绝对路径都收,内部转绝对(按当前工作目录)再判边界。
bool PathInObservationBoundary(const std::filesystem::path& path);

// 观察记录的读取提示(P2-5 验收:显式点名才准读,读前先告知体积,超过
// kObservationDiscourageThreshold 劝阻)。path 不在边界内返回空串;在边界
// 内返回一行提示(末尾带换行)。行数刻意收在一行——提示是给模型改道用
// 的路标,不是又一段要吞进上下文的正文。
std::string ObservationReadNotice(const std::filesystem::path& path, std::uintmax_t size_bytes);

}  // namespace lubancode::tools
