// 隔离范围(tools 层的写边界,0.27.x"模型侧 worktree 工具与子代理隔离"):
// 住在 worktree 房里的会话/子代理,写操作不得越出房门、落回主 checkout。
// 这里定义"隔离范围"这一份中立数据 + 线程本地的范围栈,以及一个纯谓词
// "路径是否落进禁写根"。三道闸的执行点分三处:
//   1. 文件闸:write_file/edit_file 的 execute() 开头问 Current();
//   2. cwd 闸:run_command 解析完 cwd(显式入参或缺省进程 cwd)后问;
//   3. git 改道闸:command_safety.cpp 的 FindIsolationViolation 静态识别
//      (git -C 主树 / --git-dir / GIT_DIR、GIT_WORK_TREE / cd 主树再 git)。
//
// 范围栈是 thread_local 的:主代理 enter 时在主线程压一份;隔离子代理
// (前台在主线程、后台在自己的线程)跑动前压自己的、收工弹出——同进程
// 并行的多个隔离子代理各在各的线程里,互不串门。cli/worktree 与
// agent_tool 负责压/弹,工具层只读。

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::tools {

// 一份隔离范围。路径都是 UTF-8 字符串(std::filesystem::path 的 u8 通道)。
struct IsolationScope {
    std::string name;       // 房名(报错文案里要报给模型听)
    std::string base_dir;   // 房路径:允许干活的地方
    std::string main_root;  // 主 checkout:禁写根(房自身的 .lubancode/worktrees 除外)
};

// 线程本地的隔离范围栈。Push/Pop 成对(RAII 请用下面的 ScopedIsolation);
// Current() 给栈顶,没在任何隔离里给 nullptr。栈而不是单值:主代理住着房
// 时再派一个前台隔离子代理,内层范围得能盖住外层,收工还原。
class IsolationGuard {
public:
    static void Push(IsolationScope scope);
    static void Pop();
    static const IsolationScope* Current();

    IsolationGuard() = delete;
};

// Push/Pop 的 RAII 外壳,异常路径也能还原。
class ScopedIsolation {
public:
    explicit ScopedIsolation(IsolationScope scope) { IsolationGuard::Push(std::move(scope)); }
    ~ScopedIsolation() { IsolationGuard::Pop(); }

    ScopedIsolation(const ScopedIsolation&) = delete;
    ScopedIsolation& operator=(const ScopedIsolation&) = delete;
};

// 纯谓词:utf8_path 落不落进 scope 的禁写根。相对路径按 scope.base_dir
// 解析(隔离子代理的相对路径基准是房,不是进程 cwd)。判定规则:
//   归一化后在 main_root 之内、且不在 main_root/.lubancode/worktrees 之下
//   → 拦(主 checkout 本体禁写,自家房区放行)。
// 解析失败(比如路径压根不存在的诡异形状)按"不在禁写根"放行——文件闸
// 的语义是拦"写主树",不是拦一切;run_command 侧另有自己的兜底。
bool PathBlockedByIsolation(const std::string& utf8_path, const IsolationScope& scope);

}  // namespace lubancode::tools
