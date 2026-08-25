// Capability 层(交互抛光总账第一步"三层地基"之三):终端能力探测。
//
// 规矩(规格"总规矩"5 / 第 15 条):认不出就写"不知",绝不瞎报"支持"
// ——不支持的终端能力不露假快捷键,不假装按到了键。探测分三档:
//   Yes      = 有据可查(平台 API 应答、明确的终端标识)
//   No       = 有据可查的不支持
//   Unknown  = 没问过终端、问不出、或答案取决于用户本机配置
//
// 为什么不做 DA1/XTGETTCAP 主动询问:这类多段应答尚无路由器,不能只靠
// 一把输入锁分清各类终端报告。平台层那枚单问单答的 DSR 已经串行化；
// 这里仍只做"被动探测":平台 console 探针 + 环境变量 + 已知终端名录,
// 拿不准一律 Unknown。每项都带"来源"说明,/doctor 与 /config 可贴进
// bug 单。
//
// 纯逻辑 + 一次平台探针,不向终端写任何查询序列;单测直接喂环境变量
// 快照钉行为(tests/unit/cli/test_terminal_caps.cpp)。
#pragma once

#include <string>
#include <vector>

namespace lubancode::cli {

enum class CapState { Yes, No, Unknown };

const char* CapStateWord(CapState state);  // "支持" / "不支持" / "不知"(i18n 在展示层)

// 一项能力的结论 + 来源说明(来源也走 i18n key,展示层拼)。
struct CapabilityRow {
    std::string key;      // i18n key(如 "caps.vt")
    CapState state = CapState::Unknown;
    std::string note;     // 短注(如 "Windows Terminal(WT_SESSION)",可空)
    std::string note_key;  // i18n 化的注(如 "caps.note.tmux_passthrough"),可空
};

// 探测用的环境快照(测试可注入;生产实现读真实环境)。
struct CapabilityEnv {
    std::string term;          // $TERM
    std::string term_program;  // $TERM_PROGRAM
    std::string wt_session;    // $WT_SESSION(Windows Terminal)
    std::string tmux;          // $TMUX
    std::string screen;        // $STY(screen)
    std::string ssh_tty;       // $SSH_TTY
    std::string ssh_connection;
    std::string conemu;        // $ConEmuANSI
    std::string vscode;        // $TERM_PROGRAM == "vscode"
    std::string ostype;        // _WIN32/其他,由实现填
    bool stdin_interactive = false;
    bool stdout_console = false;  // 平台探针:stdout 是真控制台
    bool vt_enabled = false;      // 平台探针:VT 处理已开
};

// 生产环境快照:platform::StdinIsInteractive / ProbeStdoutConsole +
// GetEnvVar 逐项取。
CapabilityEnv CollectCapabilityEnv();

struct TerminalCapabilities {
    CapabilityRow vt;                 // ANSI/VT 转义(平台探针)
    CapabilityRow bracketed_paste;    // ?2004(程序已启用;终端是否遵守)
    CapabilityRow sync_output;        // DEC 2026(已用;私有模式,终端不报)
    CapabilityRow osc52;              // 剪贴板转义
    CapabilityRow desktop_notify;    // 桌面通知
    CapabilityRow modify_other_keys;  // xterm modifyOtherKeys
    CapabilityRow kitty_keyboard;     // kitty keyboard protocol
    int width = 0;                    // 终端列宽(探不到 0)
    int height = 0;
    std::vector<CapabilityRow> environment;  // tmux/screen/SSH 一眼的处境行

    std::vector<CapabilityRow> AllRows() const;  // 展示次序排好
};

// 主探测口:快照 -> 结论。纯函数(喂什么环境出什么结论),生产路走
// CollectCapabilityEnv() 再进这里。
TerminalCapabilities ProbeTerminalCapabilities(const CapabilityEnv& env);

}  // namespace lubancode::cli
