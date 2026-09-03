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
#include "cli/bottom_chrome.hpp"
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

// 仅用户当场 Shift+Tab 才刷新；空闲 composer 与流式 footer 共读这一份。
ModeNoticeState& ModeNoticeSlot();

// 当场切档的持久化钩子槽:空闲 composer(mode_changed)与监听线程(流式
// Shift+Tab)都经 NotifyApprovalModeChanged 调它;装配层挂接(见
// console_input.hpp 的导出口注释)。空 = 未挂,通知 no-op。
// [共享] composer(空闲切档)/监听线程(流式切档)/导出口 SetApprovalModeChangeHook。
std::function<void(ConfirmMode)>& ApprovalModeChangeHookSlot();

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
// right_hint(可空):skills 右侧附的帮助入口小字(收口单 P2,和弦文本由
// 装配器从 ActiveKeymap 反查拼好传入——本函数保持纯,不碰 keymap;Busy
// 路传空)。右槽整段放不下时先丢 hint 段保 skills(窄屏折叠)。
std::string BuildComposerModeLine(const BoxChrome& chrome, int skill_count, int max_width,
                                  const std::string& right_hint = std::string());
std::size_t SessionSkillCount();

// ---------------------------------------------------------------------------
// 底栏公共装配器(收口审计单 §二 P1):空闲 composer(console_input_composer)
// 与流式 footer(console_input_stream_footer)不再各拼一遍 BottomChromeModel
// ——两条路只给场景差量,归槽(mode notice、assist 行、状态投影、dock、
// rule tag)只在装配器里写一次,忙闲不分会各搬一半。
// ---------------------------------------------------------------------------

// 场景差量:Idle 与 Busy 只许在这份差量里不同。menu_rows 是类型化的应用层
// 面板行(历史搜索/@提及/slash 候选/临时通知,画输入框下 transient 槽)
// ——不再拿 composer_empty && hint_lines.size()==1 的外形猜身份;空
// composer 的速览行(ShortcutAssist)由装配器按场景自置,不进这份差量。
struct BottomChromeScene {
    ComposerMode mode = ComposerMode::Idle;  // Idle / BusyQueue
    bool framed = true;                      // 无框单行读取(向导/确认提示)退化态
    const Theme* theme = nullptr;            // 组行配色(模式行/资料行)
    RenderState editor;                      // composer 内容(编辑器自有的 slash 候选随 hint_lines)
    std::string prompt = "> ";               // 首行提示符
    std::string placeholder;                 // 草稿真空时的占位提示(Busy)
    std::vector<std::string> activity_rows;  // Working 活动条(Busy;空闲空)
    bool help_visible = false;               // ? 帮助层(空闲场景;Busy 无帮助层)
    std::vector<std::string> queue_rows;     // 待发队列(调用方按场景拼好)
    std::vector<std::string> dock_rows;      // 导航坞行(调用方拼好,与 tints 按位对齐)
    std::vector<AgentHealthTint> dock_tints;
    std::string rule_tag;                    // 上横线右端短标签(查看态)
    int selected_task_id = 0;                // 导航当前选中(0=main)
    std::vector<std::string> menu_rows;      // 应用层面板行(transient 槽;空 = 无面板)
    int width = 80;                          // 组行宽度(模式行/资料行/速览右对齐)
};

// 唯一的底栏模型装配口:从同一份场景差量组 BottomChromeModel。确认档与
// 技能数读会话级共享槽(SharedEditor/SessionSkillCount),模式行与资料行从
// 同一份 StatusDataSlot 活账投影。空闲路在主线程调;footer 路在
// StdoutWriteMutex 内调(与既有的状态行读写纪律同款)。
BottomChromeModel BuildBottomChromeModel(const BottomChromeScene& scene);

// 空 composer 的左右槽速览行(Idle 场景):左槽常用键(keymap 反查,改绑
// 跟脚),右槽帮助入口——只认 ChordFor(HelpShow),无绑定整段不画。Busy
// 场景没有帮助层与这些快捷键,不置速览行(是否显示由场景决定,不由翻译
// 硬补)。读 ActiveKeymap,只在空闲主线程调。
ChromeAssistRow BuildComposerAssistRow();

// ---- Ctrl+G 键贴 composer 框(收口单 P0) ----------------------------------
// 草稿够长才画「Ctrl+G 编辑器」小注(贴 composer 框右下角,和弦文本从
// keymap 反查,改绑跟脚)。门槛:逻辑行数 >= kComposerGHintMinRows,或
// 全文码点数 >= kComposerGHintMinChars——短草稿不添噪,真用得上编辑器的
// 时候才露脸。未过门槛/无绑定给空串(调用方不画)。
inline constexpr int kComposerGHintMinRows = 4;
inline constexpr int kComposerGHintMinChars = 80;
std::string BuildComposerGHint(const RenderState& editor);

// 正式资料行构造器(收口审计单 §二 P0):输入框下横线之下的完整状态资料
// (model/effort/cwd/branch/context/tokens/cache/REC/WT/tools/Plan/goal/
// background)。读 StatusDataSlot 那一份活账,审批段(permission_mode)恒剥
// 出——档位语义归 BuildComposerModeLine 的模式行独占,两行同一快照投影。
// 空闲 composer 与流式 footer 组帧前都调它;一段资料都没有时返回空串
// (调用方据此不进帧,不留空行)。
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
