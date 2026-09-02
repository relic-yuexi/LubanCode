// console_input 家族的内部共享头(骨架拆解反弹·问题 5 第一步)。
//
// console_input.cpp 原先一个文件塞了三台不相干的机器(前台 composer 主循
// 环、流式 footer 渲染引擎、turn 监听线程),外加菜单与一层会话级静态槽。
// 拆成四个 .cpp 后,原先匿名 namespace 里的共享底层(跨机器的状态槽与
// 小工具)在这里对齐签名:
//
//   console_input.cpp             共享底层:锁、状态行、面板会话、composer
//                                  目标、查看帧账、键位翻译、菜单机器
//   console_input_composer.cpp     前台行编辑器主循环(ReadLineKeyByKey)
//   console_input_stream_footer.cpp 流式 footer 渲染引擎 + 心跳线程
//   console_input_turn_listener.cpp turn 输入监听线程
//
// 本头只供这四个文件使用,不进任何 CMake target 的公共接口(头不编),
// 别的层不许 include——对外面口仍是 cli/console_input.hpp。挂在各机器
// 自己文件里的独占槽不在这出现;这底下是"真跨机器"的共享上下文,每枚
// 的归属与理由见 console_input.cpp 的共享段注释。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/agent_panel.hpp"
#include "cli/console_input.hpp"
#include "cli/line_editor.hpp"
#include "cli/theme.hpp"
#include "platform/console.hpp"

namespace lubancode::cli {

// ---- 会话级共享槽(跨机器的"共享上下文";定义在 console_input.cpp) ----

// 贯穿整条交互会话存活的编辑器实例:空闲 composer(ReadLineKeyByKey)、
// footer 画状态行(读确认档)、监听线程 Shift+Tab 切档、CurrentConfirmMode
// 导出口,全共用这一份——历史列表、确认模式才有地方跨多轮读取存住。
LineEditorCore& SharedEditor();

// 会话级面板状态机:空闲 composer 与流式 footer/监听线程共用同一份
// 选择/焦点/详情/两段确认——流式转空闲状态不跳,不靠两边各自记账。
AgentPanelSession& PanelSessionSlot();
std::vector<int> PanelEntryIds(const std::vector<AgentPanelEntry>& entries);

// 查看帧的跨读取账(回流单"查看帧零扰动"):铺查看帧那一拍记"正文顶行
// + 当时缓冲宽",重进 composer 时凭三条判据原处认账。composer(读写)与
// 监听线程(流式期间切看也记账)共用;InvalidateViewFrameLedger 导出口
// 写它。
struct ViewFrameLedger {
    int body_top = -1;  // 查看帧正文顶行(缓冲绝对行号);-1 = 无账(main 帧作废)
    int width = 0;      // 铺帧那一拍的缓冲宽(列)
};
ViewFrameLedger& ViewFrameLedgerSlot();

// 视图切换钩子的槽(viewed_task_id 变了才被调):composer(空闲切看)与
// 监听线程(流式切看)都调它铺帧。
std::function<void(int, int)>& AgentViewSwitchHookSlot();

// composer 收件目标(查看态那只子代理的任务号):composer 写(每次读取
// 开头/首帧)、footer 写(RedrawStreamFooterLocked)、监听线程读(落队带
// 目标),读写都过 ComposerTargetMutex。
void SetComposerTarget(std::optional<int> target);
std::optional<int> GetComposerTarget();

// ---- 跨机器的小工具(定义在 console_input.cpp) ----

// platform 语义按键 -> 核心层 KeyEvent 的搬运口(composer/监听/菜单共用)。
std::optional<KeyEvent> MapKey(const platform::KeyInput& key);

// 键位缝:platform 语义按键 -> 面板动作 id(composer/监听线程共用;键位
// 从 keymap 查表,/keymap 改绑面板跟脚换键)。
std::optional<PanelKey> MapToPanelKey(const platform::KeyInput& key);

// 模式行:左端钉当前档与下一档提示,右端钉会话技能数。右端信息优先保留;
// 窄屏先收左端,最后才截右端。yolo 只给档名本身套危险色。
struct BoxChrome {
    bool enabled = false;
    const Theme* theme = nullptr;
    ConfirmMode mode = ConfirmMode::Confirm;
};
std::string BuildComposerModeLine(const BoxChrome& chrome, int skill_count, int max_width);
std::size_t SessionSkillCount();

// 状态行组行(兼容其余调用点;输入区现由 BuildComposerModeLine 画专用模式行)。
std::string BuildStatusLine(const BoxChrome& chrome, int max_width);

// 行级双缓冲的兜底画法(VT 批量不可用的老终端):空闲路 RedrawEditArea
// 与 footer 路 RedrawStreamFooterLocked 共用同一套擦写账。
void PaintInlineFrameLegacy(const InlineFrame* previous, const InlineFrame& next, int origin_y);

// Agent 会话切页认整块可视区:composer(空闲切看)与监听线程(流式切看)
// 共用;调用方须持 StdoutWriteMutex。
std::optional<platform::ScreenInfo> ClearVisibleAgentPanelLocked();

// 帧账审计开关(LUBANCODE_FRAME_AUDIT):空闲 composer 路与 footer 路各自
// 落账,开关一处读。
bool FrameAuditEnabled();

// ---- footer 机器露给监听线程的窄口(定义在 console_input_stream_footer.cpp) ----
// 监听线程原先与 footer 同住一个编译单元,直接摸 FooterSlot();拆开后经
// 这三只窄口过——语义与原先逐字相同(读 enabled / 打断收场置 false /
// 把监听编辑器的 RenderState 镜像进 footer)。写口约定调用方已持
// StdoutWriteMutex,与原先直改 FooterSlot() 同一副锁规矩。

// footer 这一场开没开(只有能可靠重画的真终端才为真;打断/管道为假)。
bool StreamFooterEnabled();
// 打断收场/监听线程异常兜底:关掉 footer,别让残余正文把提示行重画回来。
void DisableStreamFooter();
// 监听编辑器的完整镜像交给 footer(忙路 composer 的正文/提示/光标全从
// 这份 RenderState 画;调用方持 StdoutWriteMutex)。
void SetStreamFooterComposer(const RenderState& composer);
// 正文行数账作废钩子:监听线程在流式区插打了整行(打断提示/面板回执/
// 队列拒绝那类)后调——收束重画不能再按旧锚点描。调用方持
// StdoutWriteMutex(钩子实现不得再拿 stdout 锁,见 console_input.hpp)。
void RunStreamScreenPrintHook();

// ---- composer 机器露给壳的口(定义在 console_input_composer.cpp) ----

// 逐键读入这一次输入,真控制台专用(ReadLine 的交互路分身)。
std::optional<std::string> ReadLineKeyByKey(const std::string& prompt, const Theme& theme, bool esc_rejects,
                                            bool composer, ReadExitReason* exit_reason);

// 一次空闲读取的帧账审计收尾(LUBANCODE_FRAME_AUDIT 置位时往 stderr 落
// 一行"真画了几帧、写了多少字节",落完清零)。ReadLine 壳在交互路返回后
// 调;非审计环境空操作。
void ReportAndResetIdleFrameAudit();

// ---- footer 机器露给共享层的常量(定义侧见 stream_footer 文件) ----
// (Begin/End/Redraw/Erase 等对外口全在 console_input.hpp,不在这重复。)

// synchronized output(DEC 私有模式 2026):菜单帧与 footer 重画都用它把
// 一次落笔钉成一帧提交。已用 web 检索核实过假设:未知但格式合法的 CSI
// 私有模式终端必须安全忽略(xterm ctlseqs/iTerm2 规范明文),老 Windows
// Terminal/conhost 静默吞掉无副作用。
inline constexpr const char* kSyncOutputBegin = "\x1b[?2026h";
inline constexpr const char* kSyncOutputEnd = "\x1b[?2026l";

// 待发队列区常态最多画几行、导航坞常态最多单列几只代理(空闲路与
// footer 路同一本密度账)。
inline constexpr std::size_t kMaxVisibleQueuedLines = 3;
inline constexpr int kDockMaxVisibleEntries = 5;

}  // namespace lubancode::cli
