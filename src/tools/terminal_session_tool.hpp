// 内置 "terminal" 工具:POSIX 交互终端会话。
//
// run_command 的子进程 stdin 是 /dev/null、没有 PTY,驱动不了 vim、python/
// sqlite REPL、交互式安装向导、ssh、gdb 这类程序。本工具起一个 PTY 会话
// 跑 bash(没有就 sh),模型按 action 分派:
//   open  起会话,回 session_id 与首屏;
//   send  送按键(尖括号记号见 terminal_keys.hpp),等输出静默或到时再回屏;
//   read  只看屏(可选等一会儿新输出);
//   close 收会话(SIGHUP -> SIGTERM -> SIGKILL,打子进程组与前台进程组);
//   list  列出在册会话。
// 屏幕是 TerminalScreen 仿真出来的纯文本快照,可附主屏滚动历史末尾若干行。
//
// 会话台账是进程级单例:同时最多 kMaxTerminalSessions 只,满了先挤掉
// 已退出的旧会话,仍满则拒;lubancode 退出时(单例析构)全部收掉。
// 每只会话一条读线程,非阻塞读主端喂仿真器(加锁),顺带探子进程退出,
// 退出码进快照头。
//
// Windows:本单不做 ConPTY。类照常编译(公共头干净),execute 直接回
// "仅 POSIX";装配层用 #ifndef _WIN32 不注册,模型在 Windows 上看不到它。
//
// 审批与 run_command 同档:needs_confirm + ApprovalClass::Command。入参
// 里没有 command 字段,command 权限白名单/auto 档的只读判定都落空,
// 一律走确认(--yes / yolo 照放)。
#pragma once

#include <atomic>
#include <cstddef>

#include "tools/tool.hpp"

namespace lubancode::tools {

inline constexpr std::size_t kMaxTerminalSessions = 8;

class TerminalSessionTool : public Tool {
public:
    EffectClass effect_class() const override { return EffectClass::LocalProcessUnknown; }
    Idempotency idempotency() const override { return Idempotency::NonIdempotent; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::Command; }
    Result execute(const nlohmann::json& input) override;
    // 取消旗贯通:send/read 的等待循环每拍查旗,ESC 即回当前屏。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

private:
    Result Run(const nlohmann::json& input, const std::atomic<bool>* cancel);
};

// 收掉全部会话(单例析构也会调;测试收尾用)。Windows 上为空操作。
void CloseAllTerminalSessions();

}  // namespace lubancode::tools
