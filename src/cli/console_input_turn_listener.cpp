// turn 输入监听线程(骨架拆解反弹·问题 5 第一步自 console_input.cpp 拆
// 出):TurnInputListener 一只类的全部——流式期间 ESC 打断、消息排队/取回
// 编辑、Shift+Tab 切档、面板键位分派、Ctrl+C 两段退出。监听线程只在抢到
// ConsoleReadMutex 的间隙读键,与空闲 composer(ReadLineKeyByKey,住在
// console_input_composer.cpp)天然错开;它画的 footer 与读的队列账在
// console_input_stream_footer.cpp 与队列层。类注释见 console_input.hpp。
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include "cli/bottom_chrome.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"  // kMaxImageBytes(Alt+V 贴图的上限)
#include "cli/keymap.hpp"
#include "cli/mention_menu.hpp"
#include "cli/queue_model.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/transcript.hpp"  // FormatUserPromptBlock/CountLines(提交收框铺用户块)
#include "platform/clipboard.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"
#include "cli/console_input_internal.hpp"

namespace lubancode::cli {

namespace {

// 问题二:忙碌期 slash 排队被拒时的一行人话。两档理由分开说——白名单外
// (要即时交互/会改会话去向,等空闲再敲)与子代理目标(本地命令没有代理
// 可执行)。命令词取 ParseSlashCommand 的原文,不猜用户敲了什么。
std::string SlashQueueRejectionLine(const std::string& text, const MessageTarget& target) {
    const ParsedSlashCommand parsed = ParseSlashCommand(text);
    if (!target.is_main()) {
        return trf("queue.slash_not_for_subagent", parsed.raw_word);
    }
    return trf("queue.slash_rejected_busy", parsed.raw_word);
}
}  // namespace

TurnInputListener::TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme,
                                      std::atomic<bool>* transcript_expanded,
                                      ExpandRenderer expand_renderer)
    : cancel_flag_(cancel_flag),
      theme_(theme),
      transcript_expanded_(transcript_expanded),
      expand_renderer_(std::move(expand_renderer)) {
    if (platform::StdinIsInteractive()) {
        enabled_ = true;
        thread_ = std::thread([this] {
            try {
                ThreadMain();
            } catch (const std::exception& e) {
                // 监听线程里漏出异常时,std::thread 会直接 std::terminate；
                // Windows 上便是 0xC0000409,整场连同会话一起倒下。这里先
                // 收掉 footer,用不抛异常的窄 stdio 留下原始病名。主线程仍
                // 能收束本轮、回到下一枚提示符。
                std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                DisableStreamFooter();
                std::fprintf(stderr, "\n[input-listener] %s\n", e.what());
                std::fflush(stderr);
            } catch (...) {
                std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                DisableStreamFooter();
                std::fprintf(stderr, "\n[input-listener] unknown exception\n");
                std::fflush(stderr);
            }
        });
    }
}

TurnInputListener::~TurnInputListener() { Stop(); }

void TurnInputListener::Stop() {
    if (!enabled_) {
        return;
    }
    stop_requested_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    enabled_ = false;  // 幂等:重复调用/析构再调都不会再 join 一次已经空了的 thread_
    // 线程已 join,没人再动账:还挂在半途的编辑事务按 Esc 同款收尾——
    // 原文还原、解冻。不收的话那条会一直冻着,投递泵跳过它,editable_size
    // 也数不到它,像"丢了一条"。
    if (open_edit_.has_value()) {
        SessionSteeringQueue().CancelEdit(*open_edit_);
        open_edit_.reset();
    }
}

void TurnInputListener::ThreadMain() {
    // 监听期间两端都进逐键、无回显模式。Windows 虽用
    // ReadConsoleInputW，也得关 LINE/ECHO；否则字符仍躺到回车才放行，
    // conhost 还会把它画到 footer 的物理光标处。
    platform::KeyListenScope listen_scope;
    platform::KeyReader key_reader;  // 跨事件状态(代理对配对)整条线程存活

    // 会话层队列:监听线程只提交编辑动作,不拥有最终数据(见 queue_model.hpp)。
    SteeringQueue& steering = SessionSteeringQueue();

    // 排队输入/取回编辑共用一只真正的 LineEditorCore(composer 模式)。不用
    // SharedEditor():那份是前台"真正在读一行"的编辑器,历史/档位是会话级
    // 状态,监听线程不能去碰;这里只要光标/退格/粘贴/多行软换行这些纯编辑
    // 能力。补全候选走与空闲路同一只 BuildSlashCompletionCandidates():slash
    // 提示、Tab 补全/公共前缀/轮转全在编辑器一处记账,footer 只读它的
    // RenderState(见下面 refresh_footer),不再养第二套提示状态机。方向键
    // 直选菜单关掉:流式期间 Up/Down 分给代理面板、队列条目编辑与历史浏览
    // (规格五),本单只补 Tab。
    LineEditorCore editor(BuildSlashCompletionCandidates());
    editor.set_menu_selection_enabled(false);
    editor.BeginLine(/*composer=*/true);

    // 代理面板(规格三/四):流式期间与空闲 composer 共用同一枚会话级
    // AgentPanelSession,选择按稳定 task id。面板交互只在 footer 真画得动的
    // 场合(能力实探通过的真终端)承诺;plain/管道 footer 不开,键位照旧归队列
    // 编辑与历史,不承诺交互切换。
    auto panel_ids_now = [&]() -> std::vector<int> {
        if (!StreamFooterEnabled()) {
            return {};
        }
        const AgentPanelProvider& provider = SessionAgentPanelHost().provider();
        if (!provider) {
            return {};
        }
        // 导航表与空闲路同源:条目经闲置折叠后的可导航 id 序列(含汇总哨兵)。
        const std::vector<AgentPanelEntry> entries = provider();
        const AgentPanelSession::Snapshot snap0 = PanelSessionSlot().SnapshotFor(PanelEntryIds(entries));
        return DockNavigationIds(entries, snap0.idle_expanded, snap0.target_task_id.value_or(0));
    };
    // 面板动作分派(x 停止/清除、两段确认停全部):按稳定 task id 找条目,
    // 绝不按列表下标。停全部的回执插打一行,正文行数账由 print hook 作废。
    auto dispatch_panel_outcome = [&](const AgentPanelController::Outcome& outcome) {
        const AgentPanelActions& actions = SessionAgentPanelHost().actions();
        if (outcome.stop_all && actions.cancel_all) {
            const int stopped = actions.cancel_all();
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            EraseStreamFooterLocked();
            TermOut() << "\n" << theme_.stats << trf("agent_panel.stop_all_notice", stopped) << theme_.reset << "\n";
            TermOut().flush();
            RunStreamScreenPrintHook();
        }
        if (outcome.stop_current && outcome.stop_current_task_id > 0) {
            const AgentPanelProvider& provider = SessionAgentPanelHost().provider();
            const std::vector<AgentPanelEntry> entries = provider ? provider() : std::vector<AgentPanelEntry>{};
            for (const auto& entry : entries) {
                if (entry.task_id != outcome.stop_current_task_id) {
                    continue;
                }
                if (entry.running && actions.cancel_task) {
                    // 停止回执(与空闲路同一套文案):流式期间插打一行,正文
                    // 行数账由 print hook 作废;行随后显"停止中"。
                    const bool accepted = actions.cancel_task(entry.task_id);
                    std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                    EraseStreamFooterLocked();
                    TermOut() << "\n"
                              << theme_.stats
                              << (accepted ? trf("agent_panel.stop_notice", entry.task_id)
                                           : trf("agent_panel.stop_not_running", entry.task_id))
                              << theme_.reset << "\n";
                    TermOut().flush();
                    RunStreamScreenPrintHook();
                } else if (!entry.running && actions.clear_task) {
                    actions.clear_task(entry.task_id);
                }
                break;
            }
        }
    };

    // 正在编辑的排队消息(编辑事务凭据)。空 = 在敲新消息。
    std::optional<SteeringQueue::EditHandle> edit;
    // Del 两段删除的计时(规格:经明确提示后删掉——第一下亮提示,第二下
    // 2 秒窗口内才真删;提示就写在队列区标题的编辑态文案里)。
    bool delete_armed = false;
    std::chrono::steady_clock::time_point delete_armed_until{};

    // 流式路查看帧的擦账(与空闲路 ReadLineKeyByKey 里那本同款规矩,各自
    // 一份、只在本段读取内有效):真切会话前先按上一帧的缓冲顶行把旧查看
    // 帧从可视区擦净再铺新帧。app 侧视图切换钩子只打印不擦(查看态完成
    // 退场花屏单,2026-08-17)——擦账全程序只认"铺帧前现记的 console 侧
    // 这一本",绝不并立第二本。
    std::optional<int> view_body_top;
    const auto erase_previous_view_body = [&]() {
        if (!view_body_top.has_value()) {
            return;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            view_body_top.reset();
            return;  // 拿不到屏幕信息:退回旧行为,不硬擦
        }
        int top = *view_body_top;
        if (top < info->viewport_y) {
            top = info->viewport_y;  // 只管当前 viewport,滚屏历史不动
        }
        for (int y = top; y < info->height; ++y) {
            platform::ClearRowHardFrom(0, y, info->width);
        }
        platform::SetCursorPos(0, top);
        view_body_top.reset();
    };
    const auto print_view_frame = [&](int viewed_after) {
        // 流式期间也按 Panel 整页换源。先正式收掉 footer，再清可视内容区；
        // view hook 铺完目标会话后会把独立 footer 原样画回。
        std::optional<platform::ScreenInfo> before;
        {
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            EraseStreamFooterLocked();
            before = ClearVisibleAgentPanelLocked();
            view_body_top.reset();
        }
        const auto& view_hook = AgentViewSwitchHookSlot();
        if (view_hook) {
            view_hook(viewed_after, /*tail_rows=*/0);
        }
        if (before.has_value()) {
            view_body_top = before->cursor_y;
        }
        // 跨读取账同步(见 ReadLineKeyByKey 里那本的同款注释):流式期间切看
        // 的帧也记账——不过本轮流式正文还会继续写屏,RunTurn 收口(非静默)
        // 会把账作废,这里的记录只在"切看后本轮再没写过屏"时才活得过收口;
        // main 帧直接作废。
        ViewFrameLedger& view_ledger = ViewFrameLedgerSlot();
        if (viewed_after != 0 && before.has_value()) {
            view_ledger.body_top = before->cursor_y;
            view_ledger.width = before->width;
        } else {
            view_ledger.body_top = -1;
        }
    };

    // footer 快照:整份 RenderState 搬进 footer 状态(队列区在
    // RedrawStreamFooterLocked 里现拉 SteeringQueue 快照,这里不用搬)。
    // Composer 合流 P1 起 footer 不再压成"首行 + 另有 N 行"——布局函数拿
    // 完整逻辑行画软换行、算真实光标,与空闲 composer 同一颗脑袋。slash
    // 提示也随 RenderState 带过去(hint_lines:候选名单、Tab 轮转的 "> "
    // 选中标记、收起门槛全在编辑器一处记账,footer 不另算一份没有状态的
    // 菜单)。footer.enabled 为假时这些都是空操作,
    // 退回老的"不回显、只 Enter 时整条落队"。
    auto refresh_footer = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        SetStreamFooterComposer(editor.CurrentRenderState());
        RedrawStreamFooterLocked();
    };

    // 屏上已经铺的是哪只会话。状态机可能由三条路改动：按键切页、footer
    // 心跳发现任务退场、别处清理条目。监听线程每拍把“想看谁”与“屏上是谁”
    // 对一遍，凡有变化都走同一只整页换源口；不许只改导航灯和收件目标。
    int rendered_viewed_task_id = CurrentAgentViewedTaskId();
    const auto sync_agent_panel_view = [&]() {
        if (!StreamFooterEnabled()) {
            return false;
        }
        const std::vector<int> ids = panel_ids_now();
        PanelSessionSlot().OnEntriesChanged(ids);
        const int viewed_now = PanelSessionSlot().SnapshotFor(ids).viewed_task_id;
        if (viewed_now == rendered_viewed_task_id) {
            return false;
        }
        print_view_frame(viewed_now);
        rendered_viewed_task_id = viewed_now;
        return true;
    };

    // 取回一条排队消息进编辑器(光标落末尾),Del 待确认解除。
    auto begin_edit = [&](SteeringQueue::EditHandle handle) {
        editor.LoadText(Utf8ToUtf32(handle.text));
        delete_armed = false;
        edit = std::move(handle);
        refresh_footer();
    };
    auto close_edit_clear = [&]() {
        edit.reset();
        delete_armed = false;
        editor.BeginLine(/*composer=*/true);
    };
    auto editor_text_utf8 = [&] { return Utf32ToUtf8(editor.CurrentRenderState().line); };

    // 编辑态里上下键在队列条目间走:先把改到一半的正文按 Esc 语义提交到
    // 当前位(原位替换;提交失败说明那条已被边界送走,正文留在编辑器里
    // 继续当草稿),再取相邻一条。过末条往下 = 退出编辑态回"敲新消息"。
    auto browse_edit = [&](bool up) {
        if (!edit.has_value()) {
            return;
        }
        std::size_t index = 0;
        const auto snapshot = steering.Snapshot();
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            if (snapshot[i].id == edit->id) {
                index = i;
            }
        }
        const auto commit_status = steering.CommitEdit(*edit, editor_text_utf8());
        if (commit_status == SteeringQueue::CommitStatus::Ok) {
            close_edit_clear();
        } else {
            // 版本冲突:那条已变动。编辑器正文保留,事务就此了结。
            edit.reset();
            delete_armed = false;
        }
        // 相邻一条(方向钳住;正常情况下只有刚才那条冻着,BeginEdit 认不出
        // 冻结条目自然返回空,防御到位)。
        const bool go_prev = up && index > 0;
        const bool go_next = !up && index + 1 < snapshot.size();
        if (go_prev || go_next) {
            const std::size_t next = go_prev ? index - 1 : index + 1;
            if (auto handle = steering.BeginEdit(snapshot[next].id); handle.has_value()) {
                begin_edit(std::move(*handle));
                return;
            }
        }
        refresh_footer();
    };

    // Esc 和单击 Ctrl+C 打断当前轮的收场动作完全一致,抽成一个共用 lambda——
    // 置 cancel_flag、擦脚注、打一行 "[已打断]"、通知正文块作废锚点、关脚注。
    auto interrupt_turn = [&] {
        cancel_flag_.store(true);
        // 用户打断广播(监督器单 P1-0):叫醒可能睡在 agent_watch 等待里的
        // watcher——取消旗它们看得见,但没有这声 notify 就要等到超时。
        BroadcastTurnInterrupted();
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        EraseStreamFooterLocked();  // 先把脚注那行擦掉,[已打断] 才打得干净
        TermOut() << "\n" << theme_.stats << tr("input.interrupted") << theme_.reset << "\n";
        TermOut().flush();
        RunStreamScreenPrintHook();  // 插打了整行,正文块的行数账作废(锁还攥着,见头文件约定)
        // 打断后本轮就要收场:关掉脚注,别让残余正文再把提示行重画回来。
        DisableStreamFooter();
    };

    // Ctrl+C 双击退出的计时基准——只在这一次 ThreadMain() 存活期(即这一轮
    // Run() 的窗口)内有效,跨轮不留痕:TurnInputListener 是 RunTurn() 每轮
    // 现建的新实例,下一轮按键节奏重新计时,不会因为"上一轮末尾按过一次"
    // 而在下一轮开头误判成双击。
    bool has_last_ctrlc = false;
    std::chrono::steady_clock::time_point last_ctrlc_time{};
    // 双击窗口:参照 bash/Python/Node REPL 以及 Claude Code 官方文档"Ctrl+C
    // 打断、双击退出"的通用约定(经 web 检索核实,见交活报告),1200ms 内
    // 认作"这是刚才那下的续拍"。
    constexpr auto kCtrlCDoubleTapWindow = std::chrono::milliseconds(1200);

    while (!stop_requested_.load()) {
        // 没按键也要查：正在看的 subagent 若完成退场，心跳会先把状态切回
        // main；这一拍随即把上半屏也换回 main，不留旧 sub Panel。
        if (sync_agent_panel_view()) {
            refresh_footer();
        }
        // 先等“有键可读”，再去抢输入权。旧代码把这 50ms 等待也攥在锁
        // 里，POSIX 的 DSR 光标查询便无门可入；更糟的是监听线程可能先
        // 吞掉 CPR 应答。poll 不消费字节，等完再抢锁；若前台编辑器或
        // DSR 已接手，try_lock 失败就让过这一拍。
        if (!platform::WaitForKeyEvent(50)) {
            continue;
        }
        // try_lock:抢不到就说明编辑器(ReadLineKeyByKey,含工具确认提示)
        // 正在读,乖乖让出、睡一下再抢,绝不跟前台读键盘的那次调用抢同一份
        // 控制台输入。
        std::unique_lock<std::recursive_timed_mutex> read_lock(ConsoleReadMutex(), std::try_to_lock);
        if (!read_lock.owns_lock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // 阻塞式交互菜单(ask_user 选择菜单、工具确认……)开屏期间,键盘
        // 全归菜单一处:挂起计数>0 就算这一拍抢到了读权也不碰输入——
        // WaitForKeyEvent/ReadOne 一律不做,连"问题刚打印完、菜单还没抢到
        // 读权"的空窗期也不留给监听线程消费一枚键。菜单退出(计数归零)
        // 后流式排队输入、ESC 打断恢复原语义。
        if (RepaintSuspendActive()) {
            read_lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (stop_requested_.load()) {
            break;
        }
        const std::optional<platform::KeyInput> key = key_reader.ReadOne();
        read_lock.unlock();  // 这一个事件读完了,处理逻辑不用再攥着锁

        if (!key.has_value()) {
            continue;  // 读失败/EOF:跟老逻辑一样跳过,循环靠 stop_requested_ 退出
        }
        using PK = platform::KeyInput::Kind;

        // 取回键(Shift+←,备用 Ctrl+←):正文空、非编辑态、队列里有可取的
        // 条目,三者齐备才取最新一条;其余场合 Shift+Left 落到 MapKey 的缺省
        // 映射,仍是普通光标移动,不抢输入。
        if (IsQueueRecallKey(key->kind) && ShouldRecallQueuedMessage(editor.CurrentRenderState().line.empty(),
                                                                     edit.has_value(), steering.editable_size())) {
            if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                begin_edit(std::move(*handle));
            }
            continue;
        }

        if (key->kind == PK::Esc) {
            // Esc 逐层退(规格四):队列编辑态先取消编辑;面板层再依次退两段
            // 确认/详情/代理焦点;三者都没有,这一下才打断当前流式响应——
            // 不可第一下便掐掉整轮。
            if (edit.has_value()) {
                steering.CancelEdit(*edit);
                close_edit_clear();
                refresh_footer();
                continue;
            }
            if (StreamFooterEnabled()) {
                const std::vector<int> ids = panel_ids_now();
                const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                if (!ids.empty() &&
                    (snapshot.stop_all_armed || snapshot.viewed_task_id != 0 || snapshot.focused)) {
                    (void)PanelSessionSlot().HandleKey(PanelKey::Esc, ids,
                                                       editor.CurrentRenderState().line.empty(),
                                                       std::chrono::steady_clock::now());
                    (void)sync_agent_panel_view();
                    refresh_footer();
                    continue;
                }
            }
            if (steering.HasAnyDeliverable()) {
                steering.RequestImmediateDelivery();
            }
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlC) {
            // 有字先清字(规格《Ctrl+C 优先清空非空输入框》流式 footer 路):
            // footer 草稿非空时,Ctrl+C 只清正在敲、尚未 Enter 的那一条——
            // cancel_flag 不动、模型继续输出,清字那一下也不进双击退出计时
            // (免得用户清完字再按一下便误退进程)。已落队消息不受影响;
            // 取回编辑排队消息时同样只清编辑框,编辑事务与原文还原(Esc)
            // 规矩不变。空了才走下面既有的打断/双击退出状态机。
            if (!editor.CurrentRenderState().line.empty()) {
                editor.BeginLine(/*composer=*/true);
                delete_armed = false;
                refresh_footer();
                continue;
            }
            // 根因二:这里以前完全没有 PK::CtrlC 分支,按了什么反应都没有,
            // 直接被上面 "其余按键不理会" 那句注释描述的空路径吞掉——不管
            // 外层 cancel_flag 有没有别的办法置位,Ctrl+C 本身在流式期间是
            // 死键。语义对齐 bash/Python/Node REPL 和 Claude Code 官方文档
            // 确认过的通用约定:单击等同 Esc(打断当前轮,不退出程序),
            // 短时间内(kCtrlCDoubleTapWindow)连按两次才是"我要强制退出
            // 整个程序"——双击不再走 cancel_flag 那套"打断收场"流程(它可能
            // 被挂起的工具调用/子代理拖住迟迟不收场),直接 std::exit,保证
            // "用户想跑路"这条路径永远畅通。
            const auto now = std::chrono::steady_clock::now();
            const bool double_tap = has_last_ctrlc && (now - last_ctrlc_time) < kCtrlCDoubleTapWindow;
            has_last_ctrlc = true;
            last_ctrlc_time = now;
            if (double_tap) {
                {
                    std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                    EraseStreamFooterLocked();
                    TermOut() << "\n" << theme_.stats << tr("input.ctrlc_exit") << theme_.reset << "\n";
                    TermOut().flush();
                }  // 退出前先放锁——std::exit 会跑静态对象析构,别让它们卡死在这把锁上。
                std::exit(130);  // 130 = 128+SIGINT,"被 Ctrl+C 中断"的约定退出码
            }
            // 单击对齐 Esc 的两层语义:编辑态先取消编辑;队列非空时同样
            // "打断 + 立即送"。
            if (edit.has_value()) {
                steering.CancelEdit(*edit);
                close_edit_clear();
                refresh_footer();
                continue;
            }
            if (steering.HasAnyDeliverable()) {
                steering.RequestImmediateDelivery();
            }
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlO) {
            if (transcript_expanded_ != nullptr) {
                const bool expanded = !transcript_expanded_->load(std::memory_order_acquire);
                transcript_expanded_->store(expanded, std::memory_order_release);
                std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                EraseStreamFooterLocked();
                TermOut() << "\n" << theme_.stats
                          << (expanded ? tr("ui.expanded") : tr("ui.compact")) << theme_.reset << "\n";
                if (expand_renderer_) {
                    TermOut() << expand_renderer_(expanded);
                }
                TermOut().flush();
                RunStreamScreenPrintHook();  // 模式行和转录快照都不在正文行数账里,旧锚点作废
                RedrawStreamFooterLocked();
            }
            continue;
        }
        if (key->kind == PK::ShiftTab) {
            // 流式期间 Shift+Tab 切确认档——跟空闲路(ReadLineKeyByKey ->
            // LineEditorCore::HandleKey)同一套语义:同一枚 SharedEditor 状态、
            // 同一个 NextConfirmMode 纯函数,不抄第二份档序。档位来源也只有
            // 这一处:RedrawStreamFooterLocked 画状态行时现查
            // SharedEditor().confirm_mode(),切完下一帧自然带出新档,不用另
            // 记账。改档拿 ConsoleReadMutex(try_lock):前台真在读一行
            // (工具确认/ask_user)时说明那条路自己会处理按键,这里让路、
            // 不抢。重画走 stdout 锁:footer 挂起期间(确认菜单里)自动只记
            // 不画,退场第一帧带出;连切只原地换状态行,不铺提示行残骸
            // (对齐空闲路 M11 的取舍)。Tab 与它分工:Shift+Tab 只切档,非空
            // 的 Tab 落进本地编辑器走 slash 补全,空正文 Tab 在下面明拦 no-op。
            {
                std::unique_lock<std::recursive_timed_mutex> mode_lock(ConsoleReadMutex(), std::try_to_lock);
                if (!mode_lock.owns_lock()) {
                    continue;
                }
                LineEditorCore& shared_editor = SharedEditor();
                const ConfirmMode next_mode = NextConfirmMode(shared_editor.confirm_mode());
                shared_editor.set_confirm_mode(next_mode);
                ModeNoticeSlot().Show(next_mode);
            }
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            RedrawStreamFooterLocked();
            continue;
        }
        if (key->kind == PK::Up || key->kind == PK::Down) {
            if (edit.has_value()) {
                // 优先级一:正在编辑排队消息,上下键只在队列条目间走。
                browse_edit(key->kind == PK::Up);
                continue;
            }
            // 优先级二/三:面板已聚焦或详情开着,上下键只切 main/代理;composer
            // 为空、面板有代理、未处于队列编辑,上下键进面板焦点并切换。
            if (StreamFooterEnabled()) {
                const std::vector<int> ids = panel_ids_now();
                if (!ids.empty()) {
                    const bool empty_now = editor.CurrentRenderState().line.empty();
                    const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                    if (snapshot.focused || snapshot.viewed_task_id != 0 || empty_now) {
                        const auto outcome = PanelSessionSlot().HandleKey(
                            key->kind == PK::Up ? PanelKey::Up : PanelKey::Down, ids, empty_now,
                            std::chrono::steady_clock::now());
                        if (outcome.consumed) {
                            refresh_footer();
                            continue;
                        }
                        // 未消费(正文非空):落回编辑器历史,不抢键。
                    }
                }
            }
            if (editor.CurrentRenderState().line.empty() && steering.editable_size() > 0) {
                // 旧"空输入按上键取回最后一条"入口,保留作别名;有代理面板时
                // 上面已经抢先把键给了面板,这里只在无面板场合生效(规格四:
                // 主入口仍是 Shift+←)。
                if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                    begin_edit(std::move(*handle));
                }
                continue;
            }
            // 优先级四:队列空/正文非空,进编辑器(翻历史,composer 上下键的
            // 老现职)。
            editor.HandleKey(KeyEvent::Simple(key->kind == PK::Up ? KeyKind::Up : KeyKind::Down));
            refresh_footer();
            continue;
        }

        if (key->kind == PK::Delete) {
            // 编辑态:两段删除——第一下亮提示(编辑态标题自带"Del 再按一次
            // 删除"),第二下 2 秒窗口内真删。
            if (edit.has_value()) {
                const auto now = std::chrono::steady_clock::now();
                if (delete_armed && now < delete_armed_until) {
                    steering.DeleteMessage(*edit);
                    close_edit_clear();
                } else {
                    delete_armed = true;
                    delete_armed_until = now + std::chrono::seconds(2);
                }
                refresh_footer();
            }
            continue;  // 非编辑态的 Del 维持不理会(老行为)
        }

        if (key->kind == PK::NewLine) {
            // Shift/Alt+Enter:光标处插换行(编辑排队消息也享受多行能力)。
            editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
            refresh_footer();
            continue;
        }

        if (key->kind == PK::Enter) {
            if (edit.has_value()) {
                // 原位替换:保 id、目标、排队次序。版本对不上 = 那条已在工具
                // 边界送走/被别处改过——提交失败,打一行提示,编辑器正文保留
                // 当新草稿(再 Enter 即落新队),绝不"一边送旧文一边显示已保存"。
                // 问题二提交门:改写成 slash 也要过忙碌排队门(白名单外/子代理
                // 目标的命令不进队列),拒绝时编辑事务保持开着,正文留着再改。
                const std::string edited_text = editor_text_utf8();
                if (!QueueTextAdmittedDuringBusy(edited_text, edit->target)) {
                    {
                        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                        EraseStreamFooterLocked();
                        TermOut() << "\n"
                                  << theme_.error
                                  << SlashQueueRejectionLine(edited_text, edit->target)
                                  << theme_.reset << "\n";
                        TermOut().flush();
                        RunStreamScreenPrintHook();  // 插打了整行,正文块的行数账作废(锁还攥着)
                    }
                    refresh_footer();
                    continue;
                }
                const auto status = steering.CommitEdit(*edit, edited_text);
                if (status == SteeringQueue::CommitStatus::Ok) {
                    close_edit_clear();
                } else {
                    edit.reset();
                    delete_armed = false;
                    {
                        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                        EraseStreamFooterLocked();
                        TermOut() << "\n" << theme_.stats << tr("queue.commit_conflict") << theme_.reset << "\n";
                        TermOut().flush();
                        RunStreamScreenPrintHook();  // 插打了整行,正文块的行数账作废(锁还攥着)
                    }
                }
                refresh_footer();
                continue;
            }
            // 面板聚焦且 composer 空:Enter 设 viewed_task_id(与空闲同语义,
            // composer 收件目标切给该代理,上方视口换源);否则 Enter 才落队。
            if (StreamFooterEnabled() && !edit.has_value()) {
                const std::vector<int> ids = panel_ids_now();
                const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                if (!ids.empty() && snapshot.focused && editor.CurrentRenderState().line.empty()) {
                    (void)PanelSessionSlot().HandleKey(PanelKey::EnterView, ids, /*composer_empty=*/true,
                                                       std::chrono::steady_clock::now());
                    const int viewed_after = PanelSessionSlot().SnapshotFor(ids).viewed_task_id;
                    if (viewed_after != snapshot.viewed_task_id) {
                        // 真切会话统一走 Panel 换页口；rendered 账也在一处收。
                        (void)sync_agent_panel_view();
                    }
                    refresh_footer();
                    continue;
                }
            }
            // 落队:带目标的 QueuedMessage 进会话层队列,正文挪进上方队列区,
            // 输入行清空回占位提示。全空白不落。
            // 问题二提交门:忙碌期的 slash 只放白名单内的 main 目标条目——
            // 命令身份由正文自带(IsQueuedSlashText),工具边界让路,轮末经
            // ProcessLine 本地执行;白名单外或投给子代理的命令明说拒绝入队,
            // 不悄悄降成普通文字送给模型。拒绝的正文留在 composer 供当场改写。
            const std::string text = editor_text_utf8();
            if (!text.empty()) {
                const std::optional<int> agent_target = GetComposerTarget();
                const MessageTarget target = agent_target.has_value() ? MessageTarget::Agent(*agent_target)
                                                                      : MessageTarget::Main();
                if (!QueueTextAdmittedDuringBusy(text, target)) {
                    {
                        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                        EraseStreamFooterLocked();
                        TermOut() << "\n"
                                  << theme_.error << SlashQueueRejectionLine(text, target) << theme_.reset
                                  << "\n";
                        TermOut().flush();
                        RunStreamScreenPrintHook();  // 插打了整行,正文块的行数账作废(锁还攥着)
                    }
                } else {
                    steering.Enqueue(target, text);
                    // 用户排入待发消息也是"用户输入到了"(监督器单 P1-0):
                    // 叫醒睡在 agent_watch 等待里的 watcher,单子 §9.2。
                    BroadcastTurnInterrupted();
                    editor.BeginLine(/*composer=*/true);
                }
            }
            refresh_footer();
            continue;
        }

        // 面板其余键位(x 停止/清除、Ctrl+X Ctrl+K 两段确认):composer 空、
        // 非队列编辑态时交给同一套状态机;打字中途状态机自己放行(字母 x 只
        // 进 composer)。
        if (StreamFooterEnabled() && !edit.has_value() &&
            (key->kind == PK::CtrlX || key->kind == PK::CtrlK ||
             (key->kind == PK::Char && (key->ch == U'x' || key->ch == U'X')))) {
            const std::optional<PanelKey> panel_key = MapToPanelKey(*key);
            const std::vector<int> ids = panel_ids_now();
            if (panel_key.has_value() && !ids.empty()) {
                const bool empty_now = editor.CurrentRenderState().line.empty();
                const auto outcome =
                    PanelSessionSlot().HandleKey(*panel_key, ids, empty_now, std::chrono::steady_clock::now());
                dispatch_panel_outcome(outcome);
                if (outcome.consumed) {
                    refresh_footer();
                    continue;
                }
            }
        }

        // 忙碌且 composer 为空时的 Tab:监听线程明确 no-op。这一下若喂给
        // 编辑器,composer 模式会把它翻成"进焦点态 + focus_move",而监听
        // 线程既不消费那个返回值、也不真切换焦点——画面没动,编辑器内部却
        // 换了暗状态,后续 Tab 的补全语义跟着遭殃。流式期间焦点浏览本就不
        // 开(docs/features/terminal/README.md),这里当场拦下;拦过之后再键入 /eff,第
        // 一下 Tab 照常补全,不受任何残留状态影响。
        if (key->kind == PK::Tab && editor.CurrentRenderState().line.empty()) {
            continue;
        }

        // 其余按键统一喂编辑器(字符/粘贴/退格/左右/Home/End;非空的 Tab
        // 落进编辑器走 slash 补全/轮转)。任意编辑动作解除 Del 待确认。
        if (const std::optional<KeyEvent> mapped = MapKey(*key); mapped.has_value()) {
            delete_armed = false;
            editor.HandleKey(*mapped);
            refresh_footer();
        }
    }
    // 交出还没了结的编辑事务:Stop() 在 join 之后读它并按 Esc 同款收尾。
    open_edit_ = edit;
}

}  // namespace lubancode::cli
