// 流式 live UI 的三件"画板":只管画在哪、怎么擦、怎么重画,不调用模型、
// 不碰 session store:
//   - TranscriptPainter:工具条目的控制台原地改写(Win32 锚点记账)。
//   - StreamBodyTracker:流式正文的两段式 markdown 渲染记账——正文逐字
//     原样打,段落收束、代码围栏闭合后原地换成渲染版。
// 依赖只到 cli/ 与 platform/,不知道 RunTurn / InteractiveLoop 的存在。
// 纯格式(条目长什么样)在 cli/transcript.cpp,这边只管落笔与改写。

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include "cli/console_input.hpp"
#include "cli/line_editor.hpp"
#include "cli/markdown.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"

namespace lubancode::cli {

// VT 批量序列(terminal_batch)能不能用——进程内探测一次,缓存住。
inline bool SupportsVtBatch() {
    static const bool enabled = lubancode::platform::ProbeStdoutConsole().vt_enabled;
    return enabled;
}

// ---------------------------------------------------------------------------
// UI-B(0.12.0):工具条目的控制台原地改写。
//
// 纯渲染(条目长什么样)在 cli/transcript.cpp 的 FormatTranscriptItem 里,
// 这里只管"画在哪、怎么改"的 Win32 锚点记账:每画一个条目记下起始行号和
// 行数;工具结束后回到起始行,整块清掉重画成终态。工具执行期间没有别的
// 输出流(流式文本只在 API 响应期),原地回写是安全的——唯一会往下垫内容
// 的两个场景(确认交互块、子代理条目画在 agent 条目下面)各有对策:
//   1. 确认块:待确认态画好后先 ReserveRows 在缓冲区底部预留足够行,免得
//      交互期间自然滚屏把锚点推歪;用户答完 TrimBelow 把确认块整个擦掉,
//      条目重新成为屏幕最后的内容,后续改写照常。
//   2. agent 条目:终态摘要固定一行,行数跟执行中帧一致,原地改写不用长高;
//      万一要长高而下面垫着内容(比如 ESC 打断提示),砍到原有行数——完整
//      信息反正都在 full_output 里,UI-C 的 Ctrl+E 能看全。
// enabled=false(管道/重定向)时所有方法都是空操作,管道模式的稳定纯文本
// 输出由 ToolDisplay 另走一条路。
class TranscriptPainter {
public:
    // expanded:UI-D(0.16.0)紧凑/详细全局开关的会话级状态(InteractiveLoop
    // 持有,Ctrl+O 翻转),指针判空兜底(AskOnce 不传,恒紧凑)。详细态下
    // 新条目/终态改写直接按展开版画(完整参数 + full_output 全文,行数多就
    // 整屏往下铺,滚动自然发生);FormatTranscriptItem 的 width>0 截断保证
    // 每行绝不物理折行,锚点记账照旧成立。
    // atomic<bool>(根因二 part B 修复):回合执行期间 TurnInputListener 的
    // 监听线程也会翻这个开关,跟 Run() 所在的主线程之间没有别的同步手段——
    // 真机驱动器实测踩到过用普通 bool 时"翻转了但主线程这一拍还没看见新值"
    // 的可见性问题(子代理那一次仅有的一次工具调用,恰好在这个窗口期读到
    // 了旧值),换成 atomic<bool> 用 load/store 的 acquire/release 语义堵上。
    TranscriptPainter(const lubancode::cli::Theme& theme, bool enabled,
                       const std::atomic<bool>* expanded = nullptr)
        : theme_(theme), enabled_(enabled), expanded_(expanded) {}

    TranscriptPainter(const TranscriptPainter&) = delete;
    TranscriptPainter& operator=(const TranscriptPainter&) = delete;

    // 画一个新条目(跟前面的输出空一行分隔)。锚点定不下来(screen_ 关着,
    // 或者这一帧 GetScreenInfo 失败)就干脆不打印、不登记——这个条目唯一
    // 的一次落地输出留给 Repaint() 兜底打印终态,免得"运行中"先打一行、
    // "完成"又打一行,看着像重复(#二 的"一黄一绿"排查出来的根因之一:
    // 旧版这里退化成"追加打印",Repaint 的兜底分支又整段重打一遍)。
    void PaintNew(const lubancode::cli::TranscriptItem& item) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        if (!screen_) {
            return;
        }
        const std::string text = Render(item);
        std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        if (info->cursor_x > 0) {
            TermOut() << "\n";  // 流式正文多半没换行收尾,先把光标归位到行首
        }
        TermOut() << "\n";  // 空行分隔
        TermOut().flush();
        info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        int start_row = info->cursor_y;
        const int rows = lubancode::cli::CountLines(text);
        // Render 末尾带换行；除内容行外还得给换行后的光标留一格。少这
        // 一格，条目贴底时会暗滚一行，锚点便比真标题低一行，终态重画
        // 只能在旧黄行下面另起绿行。
        EnsureRoom(start_row, rows + 1);
        lubancode::platform::SetCursorPos(0, start_row);
        TermOut() << text;
        TermOut().flush();
        anchors_.push_back(Anchor{item.id, start_row, rows, BuildFrame(text, info->width)});
    }

    // 原地改写一个已登记的条目(执行中 -> 待确认 -> 终态)。锚点没登记成
    // (PaintNew 那次定不下位,或者 screen_ 这个平台压根不开)——兜底追加
    // 打印这一次状态,这是这个条目唯一一次落地的输出,跟 PaintNew 的
    // "不打印" 配套,保证运行中/待确认/完成这条链路总归有且只有一行终态
    // 落地(交互态 Pending/中途确认需要多次可见更新时,每次都会因为同样
    // 找不到锚点而各打一行——这不是"重复同一状态",是用户必须看见的
    // 交互链路,合理)。
    void Repaint(const lubancode::cli::TranscriptItem& item) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        if (!screen_) {
            TermOut() << "\n" << Render(item);
            TermOut().flush();
            return;
        }
        Anchor* anchor = Find(item.id);
        if (anchor == nullptr) {
            TermOut() << "\n" << Render(item);
            TermOut().flush();
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const int buffer_width = info->width;
        const int saved_cursor_x = info->cursor_x;
        const int saved_cursor_y = info->cursor_y;
        std::string text = Render(item);
        int new_rows = lubancode::cli::CountLines(text);

        // 条目是不是屏幕上最后的内容(光标正停在条目下一行行首)。不是的话
        // 不能长高——往下多画会盖住垫在下面的输出,只能砍到原有行数。
        const bool at_tail = saved_cursor_x == 0 && saved_cursor_y == anchor->start_row + anchor->rows;
        if (!at_tail && new_rows > anchor->rows) {
            text = FirstNLines(text, anchor->rows);
            new_rows = anchor->rows;
        }

        const int rows_to_clear = (std::max)(anchor->rows, new_rows);
        int viewport_x = info->viewport_x;
        int viewport_y = info->viewport_y;
        if (at_tail) {
            int start_row = anchor->start_row;
            const int before_start_row = start_row;
            EnsureRoom(start_row, rows_to_clear + 1);  // Render 末尾带换行，须多留一行
            if (start_row != before_start_row) {
                if (const auto after_scroll = lubancode::platform::GetScreenInfo();
                    after_scroll.has_value()) {
                    viewport_x = after_scroll->viewport_x;
                    viewport_y = after_scroll->viewport_y;
                }
            }
        }
        lubancode::cli::InlineFrame next_frame = BuildFrame(text, buffer_width);
        if (SupportsVtBatch()) {
            lubancode::cli::InlineFrame paint_frame = next_frame;
            if (!at_tail) {
                paint_frame.cursor_x = saved_cursor_x;
                paint_frame.cursor_row = saved_cursor_y - anchor->start_row;
            }
            lubancode::platform::TerminalBatch batch(viewport_x, viewport_y);
            lubancode::cli::QueueInlineFrameDiff(batch, &anchor->frame, paint_frame,
                                                  anchor->start_row);
            batch.Flush();
        } else {
            for (int r = 0; r < rows_to_clear; ++r) {
                lubancode::platform::ClearRowFrom(0, anchor->start_row + r, buffer_width);
            }
            lubancode::platform::SetCursorPos(0, anchor->start_row);
            TermOut() << text;
            TermOut().flush();
            if (!at_tail) {
                lubancode::platform::SetCursorPos(saved_cursor_x, saved_cursor_y);
            }
        }
        anchor->rows = new_rows;
        anchor->frame = std::move(next_frame);
    }

    // 状态型条目(todo 计划)会跨多次工具调用复用。锚点还在，便原地
    // 改；已经滚出屏幕，则重新 PaintNew，给这一轮留一枚新锚点。
    bool HasAnchor(int item_id) {
        if (!enabled_ || !screen_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        return Find(item_id) != nullptr;
    }

    // 这台终端能不能原地改写条目(enabled 且平台支持 screen repaint)。
    // 思考露尾预览只在这台子上开——改不动的(POSIX 老终端/重定向)明走
    // plain 降级:不逐帧铺,收定时一行"思考 Xs",不画半只框。
    bool CanRepaintInPlace() const { return enabled_ && screen_; }

    // 把条目末尾到当前光标之间的行全部擦掉、光标回到条目末尾——确认交互块
    // (参数详情 + [y/a/N] 提示)答完之后收尾用,让条目重新成为屏幕最后的
    // 内容,后续原地改写不受它牵连。
    void TrimBelow(int item_id) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        if (!screen_) {
            return;  // 原地改写不开的平台上,确认块留在滚动历史里,不擦
        }
        Anchor* anchor = Find(item_id);
        if (anchor == nullptr) {
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const int end_row = anchor->start_row + anchor->rows;
        if (info->cursor_y < end_row) {
            return;
        }
        if (SupportsVtBatch()) {
            lubancode::platform::TerminalBatch batch(info->viewport_x, info->viewport_y);
            for (int r = end_row; r <= info->cursor_y; ++r) {
                batch.ClearRowHardFrom(0, r, info->width);
            }
            batch.MoveTo(0, end_row);
            batch.Flush();
        } else {
            for (int r = end_row; r <= info->cursor_y; ++r) {
                // diff 预览带整行背景色；只擦字符会留下成片红绿底，须连
                // 字符属性一道归零。
                lubancode::platform::ClearRowHardFrom(0, r, info->width);
            }
            lubancode::platform::SetCursorPos(0, end_row);
        }
    }

    // 把一个已登记条目从屏幕上整段收走、忘记锚点——UI-D 折叠(#三)用:
    // 子代理内层工具条目落定终态那一刻,紧凑态默认不逐条铺屏,PaintNew/
    // Repaint 画出来的执行中/终态那几行随手收走(子代理的状态另有代理面板
    // 一处画);TranscriptItem 数据本身不受影响,还在 transcript 数组里。
    // 只有条目正好是屏幕最后内容(光标紧跟在条目末尾)才真的擦——中途
    // 垫了别的内容(理论上不该发生,ESC 打断这类极端时序除外)就只忘记
    // 锚点、屏幕不动,不冒险乱擦不属于这个条目的内容;忘记锚点之后这个
    // item_id 对 Repaint/TrimBelow 就等于"从没画过",退化路径安全。
    void Retract(int item_id) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        if (!screen_) {
            return;  // 没有锚点这回事,压根没画过,没什么好收的
        }
        Anchor stored{};
        bool found = false;
        for (auto it = anchors_.begin(); it != anchors_.end(); ++it) {
            if (it->item_id == item_id) {
                stored = *it;
                anchors_.erase(it);
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const bool at_tail = info->cursor_x == 0 && info->cursor_y == stored.start_row + stored.rows;
        if (!at_tail) {
            return;
        }
        if (SupportsVtBatch()) {
            lubancode::platform::TerminalBatch batch(info->viewport_x, info->viewport_y);
            for (int r = 0; r < stored.rows; ++r) {
                batch.ClearRowHardFrom(0, stored.start_row + r, info->width);
            }
            batch.MoveTo(0, stored.start_row);
            batch.Flush();
        } else {
            for (int r = 0; r < stored.rows; ++r) {
                // 收走的子工具可能带彩色 diff 摘要，连背景属性一起清。
                lubancode::platform::ClearRowHardFrom(0, stored.start_row + r, info->width);
            }
            lubancode::platform::SetCursorPos(0, stored.start_row);
        }
    }

    // Ctrl+O 在回合中会从光标处追加一份完整转录。旧锚点的绝对行号经过
    // 这次大段滚屏已不可信；调用方正持有 stdout 锁，直接全数作废。后续
    // 工具收尾找不到锚点时会走安全的追加打印，不会回头盖错旧内容。
    void ForgetAnchorsLocked() { anchors_.clear(); }

    // 在缓冲区底部预留 rows 行(必要时主动滚屏、同步平移所有锚点)。确认
    // 交互开始前调一次,免得交互期间的自然滚屏让锚点失效。
    void ReserveRows(int rows) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        if (!screen_) {
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const int saved_x = info->cursor_x;
        int row = info->cursor_y;
        const int before = row;
        EnsureRoom(row, rows);
        if (row != before) {
            lubancode::platform::SetCursorPos(saved_x, row);
        }
    }

    // footer / 状态块主动滚屏后同步绝对锚点。调用方已持有 stdout 锁。
    void OnScreenScrolledLocked(int rows) {
        if (!enabled_ || rows <= 0) {
            return;
        }
        anchors_.erase(std::remove_if(anchors_.begin(), anchors_.end(), [rows](Anchor& anchor) {
                           if (anchor.start_row < rows) {
                               return true;  // 整块已经滚出缓冲区，锚点作废
                           }
                           anchor.start_row -= rows;
                           return false;
                       }),
                       anchors_.end());
    }

private:
    struct Anchor {
        int item_id = 0;
        int start_row = 0;
        int rows = 0;
        lubancode::cli::InlineFrame frame;
    };

    static lubancode::cli::InlineFrame BuildFrame(const std::string& text, int width) {
        lubancode::cli::InlineFrame frame;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t end = newline == std::string::npos ? text.size() : newline;
            frame.rows.push_back(lubancode::cli::InlineFrameRow{
                0, width, false, text.substr(pos, end - pos)});
            if (newline == std::string::npos) {
                break;
            }
            pos = newline + 1;
        }
        frame.cursor_row = static_cast<int>(frame.rows.size());
        return frame;
    }

    std::string Render(const lubancode::cli::TranscriptItem& item) const {
        const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
        const bool expanded = expanded_ != nullptr && *expanded_;
        return lubancode::cli::FormatTranscriptItem(item, theme_, width, expanded);
    }

    static std::string FirstNLines(const std::string& text, int n) {
        std::string out;
        int count = 0;
        std::size_t pos = 0;
        while (pos < text.size() && count < n) {
            const std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                out += text.substr(pos);
                out += "\n";
                return out;
            }
            out += text.substr(pos, nl - pos + 1);
            pos = nl + 1;
            ++count;
        }
        return out;
    }

    Anchor* Find(int item_id) {
        for (auto& anchor : anchors_) {
            if (anchor.item_id == item_id) {
                return &anchor;
            }
        }
        return nullptr;
    }

    // 从 start_row 起要写 rows_needed 行,会不会伸出可视窗口底?会的话先按
    // 帧账原语腾够(长缓冲平移视口、贴底滚内容),再把所有登记过的锚点
    // (和调用方手里这个 start_row)随内容滚动一起往上平移——跟
    // console_input.cpp 的 EnsureRoomForRows 同一套账,全认
    // EnsureViewportRowsLocked 一处。这里不带锚点护栏(老行为:滚了再说,
    // 锚点夹 0),护栏是 footer/正文块各自的规矩。
    void EnsureRoom(int& start_row, int rows_needed) {
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const int overflow = lubancode::cli::EnsureViewportRowsLocked(start_row, rows_needed);
        if (overflow <= 0) {
            return;  // 平移视口(0):锚点不动;拿不到屏幕信息同样一笔不滚
        }
        for (auto& anchor : anchors_) {
            anchor.start_row = (std::max)(0, anchor.start_row - overflow);
        }
        start_row = (std::max)(0, start_row - overflow);
    }

    const lubancode::cli::Theme& theme_;
    bool enabled_;
    // 原地改写在这个平台上开不开(POSIX 暂不开,原因见
    // platform::SupportsScreenRepaint 注释);不开时各方法退化成顺序打印。
    const bool screen_ = lubancode::platform::SupportsScreenRepaint();
    const std::atomic<bool>* expanded_ = nullptr;  // UI-D:紧凑/详细会话级开关,见构造函数注释
    std::vector<Anchor> anchors_;
};

// ---------------------------------------------------------------------------
// #52:子代理状态条——RunTurn 期间常驻在输出末尾的一块浮动展示,ticker
// 线程按固定间隔(400ms)醒一次现刷,子代理内部长时间"闷头想"(没有任何
// tool_start 事件的空窗期,agent_tool.cpp 里子代理自己的文本流被静音,见
// AgentTool::execute 的 on_text_delta 留空)照样能看见耗时在跳,不会让人
// 以为程序卡死。
//
// 跟 TranscriptPainter 不是一回事:后者改写一个已登记条目,这个管屏幕
// 末尾那块浮动状态。每帧先收起 footer,回到正文续写点,再用相对光标移动
// 擦旧状态。若新状态贴底，EnsureStreamScreenRowsLocked 会先滚够位置，
// 再把滚动量报给 TranscriptPainter 与 StreamBodyTracker；三本账一同挪。
// enabled 仍取 "is_console && 彩色主题"，相对光标移动须有 VT 支持。
//
// 并发协议:写 stdout 一律经 StdoutWriteMutex()。ticker 线程自己按固定
// 间隔醒;主线程侧(ToolDisplay 每个会打印新内容的方法)在真正打印之前
// 先调 Hide() 把状态块从屏幕上擦掉——不用显式"打印完再 Show()",ticker
// 线程最多等一个间隔就会自己重画,新内容不会被状态块盖住,状态块的
// 残影也不会污染新内容。
//
// "ask_user 被子代理状态遮挡"补的两条:
// ---------------------------------------------------------------------------
// markdown(0.18.x):模型正文的两段式渲染记账员。
//
// 正文仍逐字原样打，同时按空行切成小段；一段收齐、代码围栏闭合，便过
// DetectMarkdownStructure，命中后原地换成 RenderMarkdown 结果。最后没以
// 空行收尾的一段由 FinalizeRepaint 收账。长回答不再攒到整轮末尾才重画，
// 免得块首滚出屏幕后整篇放弃渲染。
//
// 问题 1(真实实测:流式粗体不渲染)补的两层:
//   1. 增量重画——块内每新到一行完整行、或行内标记新配成一对,就按当前
//      累计正文整块重画一次(ScanBodyDelta 产增量步,防洪峰预算见
//      kBodyIncrementalMax*);闭合的 `**` 不必再苦等段尾空行。
//   2. 让路定格——工具条目/思考块开画(OnBlockBreak)时,未收束的正文块
//      先按渲染版定格再清账。老路直接丢账,模型"正文段 + tool_use 同条
//      消息、正文后无空行"的写法(实测真机最常见)会让星号永远露着。
//
// 问题 3(真实实测:分块渲染吃掉标题前空行)补的两笔:
//   1. 前距自带——块的渲染预案在"上一块已定格成渲染版"时自带一行前距
//      (PrepareBodyRenderPlan 的 leading_gap)。前一块的重画从块首写到
//      光标,把它块尾那行分隔空行一并擦了(渲染版头尾空行都剪),后一块
//      不自带前距的话两块贴死;上一块原样保留时空行还在屏上,不自带,
//      免得两行。整篇开头/工具边界后的第一块不带,不凭空多首行空白。
//   2. 空行吸收——ScanBodyDelta 把空行连发的第二枚起整个吞掉(不落笔、
//      不产空块收束步):第一枚已随收束段上屏,多枚再打只会越撑越松,
//      "连续多空行收成一行"在落笔这层就办掉。
//
// 工具条目要开画时(on_tool_start)，尚未收束的小段定格成渲染版(画不动
// 才保持原样)；下一段重新取锚。已在段落边界画好的 Markdown 不受影响。
//
// 行数记账不猜折行:块首记起始行号,收束时按光标位移算物理行数——原样
// 流式的长行由控制台自然折行,逐字模拟折行规则(延迟 EOL 那套)不可靠。
// 滚屏对策跟 TranscriptPainter::EnsureRoom 同一套账:每个增量落笔前按
// "换行数 + 显示宽/控制台宽 + 余量"高估一下要占的行数,快撞缓冲区底就
// 自己先滚够、把 start_row_ 同步往上平移——滚动始终出自自己之手,行号
// 永远对得上;正文块比整个缓冲区还高时才真的救不了。靠不住的账一律放弃
// 重画(宁可漏渲染,不可错渲染):块首不在行首、探测失败、块高过整个
// 缓冲区,全都原样保留。
//
// enabled=false(管道/重定向/plain 主题)时 OnDelta 退化成"拿锁原样打印"
// (跟 0.18.0 的 on_text_delta 逐字节一致),其余方法全是空操作。
// ---------------------------------------------------------------------------
// 一笔 delta 切步的纯函数(P2-4 抽出来钉单测):输出"原样落笔/收束重画"
// 两型步骤,围栏账与空行边界都在这里判。旧实现同一笔 delta 里第二段的
// 重画会把第一段再渲染一遍、铺在错的锚点上——UI 泵按 33ms 并批后,一笔
// delta 常跨好几段,重复渲染成了重绘洪峰的大头(Plan 采样 166k 字符对
// 16k 正文,十倍的账)。规矩改为:空行一到,先落最后一段原文,再以"自
// 上次收口以来攒的全部正文"触发一次收束重画,随后攒账清零——一段只
// 渲染一次,渲染的永远是自己的原文。
//
// 增量重画(真实实测问题 1:流式粗体不渲染):老路只在空行收束时重画,
// 段没等到空行就遇到工具边界(正文后直接跟 tool_use)的话,块被整个丢
// 掉,`**` 星号永远露着。切步器再多产一型"行/标记边界增量步":块内每
// 新到一行完整行、或行内标记新配成一对,就按当前累计正文整块重画一次
// (未闭合标记 RenderMarkdown 天然原样保留,半行的渲染版与原样一字不
// 差,续打无缝);块的攒账不清,空行收束照旧。防洪峰:块换行数/字节超
// 预算后退回"只等空行收束"。
struct BodyDeltaStep {
    bool repaint = false;
    // repaint=true 时这步是哪种重画:
    //   finalize=true:空行收束——块定格成渲染版,攒账清零,下段另起;
    //   finalize=false:行/标记边界增量——块重画但继续攒,后续正文续接。
    bool finalize = false;
    std::string text;  // repaint=false:原样落笔的段;repaint=true:待整块渲染的块原文
};

struct BodyScanState {
    bool fence_open = false;
    std::string line_probe;
    // 增量重画账:上次重画那一刻块内的换行数/行内标记对数。新完整行或
    // 新配对到达才再画;空行收束/块作废时随块清零。
    int last_newlines = 0;
    int last_pairs = 0;
    // 问题 3(分块渲染吃掉标题前空行)的两笔账:
    //   blank_run:正处空行连发当中(或正文还没落过一笔)——收束步的原样
    //   段已把第一枚空行带上屏,后续空行整枚吸收,多枚只当一枚用。非空行
    //   一完成便翻回 false。初值 true:正文开头的空行也算"连发",照吸。
    //   rendered_before:上一块是否已定格成渲染版——是,则它块尾那行分隔
    //   空行已被重画擦掉,本块的渲染预案要自带一行前距;否(原样保留/开
    //   头第一块),空行还在屏上或压根没有,不带,免得凭空多出空白。
    bool blank_run = true;
    bool rendered_before = false;
};

// 增量重画的防洪峰预算:块高/块宽超过这份就不再逐行重画(整块重画的账
// 是 O(块高^2),超预算的长段退回"空行收束才画",与老路一致)。
constexpr int kBodyIncrementalMaxNewlines = 48;
constexpr std::size_t kBodyIncrementalMaxBytes = 16384;

// 块内行内标记的成对数(** 对 + ` 对,数法与 DetectMarkdownStructure 同款:
// 非重叠扫)。对数增加 = 又有标记闭合,值得重画一次。
inline int CountInlineMarkPairs(const std::string& text) {
    std::size_t bold_marks = 0;
    for (std::size_t pos = text.find("**"); pos != std::string::npos; pos = text.find("**", pos + 2)) {
        ++bold_marks;
    }
    const std::size_t backticks = static_cast<std::size_t>(std::count(text.begin(), text.end(), '`'));
    return static_cast<int>(bold_marks / 2 + backticks / 2);
}

inline std::vector<BodyDeltaStep> ScanBodyDelta(BodyScanState& state, const std::string& text,
                                                const std::string& block_so_far) {
    std::vector<BodyDeltaStep> steps;
    std::string projected = block_so_far;  // 自上次收口以来攒的整块正文
    std::string piece;
    for (const char c : text) {
        piece += c;
        if (c != '\n') {
            state.line_probe += c;
            continue;
        }
        std::size_t first = 0;
        while (first < state.line_probe.size() &&
               (state.line_probe[first] == ' ' || state.line_probe[first] == '\t')) {
            ++first;
        }
        if (state.line_probe.compare(first, 3, "```") == 0) {
            state.fence_open = !state.fence_open;
        }
        const bool blank = first == state.line_probe.size();
        state.line_probe.clear();
        if (blank && !state.fence_open) {
            if (state.blank_run) {
                // 问题 3:空行连发的第二枚起整枚吸收——第一枚已随收束段
                // 原样上屏(或正文开头压根还没落过笔),这枚再落笔只会把
                // 画面越撑越松。不打印、不产空块收束步,吞掉完事。
                piece.clear();
                continue;
            }
            if (!piece.empty()) {
                projected += piece;
                BodyDeltaStep print_step;
                print_step.text = piece;
                steps.push_back(std::move(print_step));
            }
            piece.clear();
            BodyDeltaStep repaint_step;
            repaint_step.repaint = true;
            repaint_step.finalize = true;
            repaint_step.text = projected;
            steps.push_back(std::move(repaint_step));
            projected.clear();  // 收口:下一段从空串重新攒,不再回头渲染旧段
            state.last_newlines = 0;
            state.last_pairs = 0;
            state.blank_run = true;  // 进入空行连发:后续空行吸收
        } else if (!blank) {
            state.blank_run = false;  // 有内容的行完成:连发到此为止
        }
    }
    if (!piece.empty()) {
        BodyDeltaStep print_step;
        print_step.text = piece;
        steps.push_back(std::move(print_step));
    }
    // 行/标记边界增量重画:当前未收束块(projected + piece)比上次重画时
    // 多了完整行或新配对的行内标记,且块还在预算内——整块重画一次,块
    // 照常继续攒。空行收束刚画过的空块(pending 为空)不触发。
    const std::string pending = projected + piece;
    if (!pending.empty()) {
        int newlines = 0;
        for (const char c : pending) {
            newlines += c == '\n' ? 1 : 0;
        }
        const int pairs = CountInlineMarkPairs(pending);
        if ((newlines > state.last_newlines || pairs > state.last_pairs) &&
            newlines <= kBodyIncrementalMaxNewlines && pending.size() <= kBodyIncrementalMaxBytes) {
            state.last_newlines = newlines;
            state.last_pairs = pairs;
            BodyDeltaStep repaint_step;
            repaint_step.repaint = true;
            repaint_step.finalize = false;
            repaint_step.text = pending;
            steps.push_back(std::move(repaint_step));
        }
    }
    return steps;
}

// 收束/增量重画的渲染预案(条 4:锁外算好,锁内照单落笔)。hit=false 就是
// "没探到 markdown 结构"——锁内只清账不画。
struct BodyRenderPlan {
    bool hit = false;                      // DetectMarkdownStructure 命中
    std::vector<std::string> lines;        // 命中:RenderMarkdown 的渲染行
    bool ended_with_newline = false;       // 原样块末尾带不带换行(老账)
};

// 备一份重画预案:没探到 markdown 结构给 hit=false(锁内不画、原样保留)。
// leading_gap(问题 3:分块渲染吃掉标题前空行):本块渲染版自带一行前距。
// 前一块定格成渲染版时,它的重画把块尾分隔空行擦了(渲染版头尾空行都
// 剪),本块再不自带前距,两块在屏上贴死——标题前那行空行就是这么丢的。
// 恰好一行:多枚空行在 ScanBodyDelta 已收成一枚,这里只垫这一行,不叠
// 加;整篇开头/工具边界后的第一块传 false,不凭空多首行空白。
inline BodyRenderPlan PrepareBodyRenderPlan(const std::string& block_text, const lubancode::cli::Theme& theme,
                                            int width, bool leading_gap = false) {
    BodyRenderPlan plan;
    plan.hit = lubancode::cli::DetectMarkdownStructure(block_text);
    if (!plan.hit) {
        return plan;
    }
    plan.ended_with_newline = !block_text.empty() && block_text.back() == '\n';
    plan.lines = lubancode::cli::RenderMarkdown(block_text, theme, width);
    if (leading_gap && !plan.lines.empty()) {
        plan.lines.insert(plan.lines.begin(), std::string());
    }
    return plan;
}

// 一笔正文 delta 的完整渲染决策(锁外纯拼装,条 4):ScanBodyDelta 切步,
// 每个重画步就地备好渲染预案。StreamBodyTracker::ScanDelta 是它的薄壳
// (只补终端宽度探测);单测直接钉这里——分块怎么切、每步渲染成什么行,
// 不用真终端就能穿过"真实分块收束路径"(问题 1 验收)。
struct BodyRenderStep {
    bool repaint = false;
    bool finalize = false;             // repaint=true 时:收束清账还是增量保账
    std::string piece;                 // repaint=false:原样落笔的段
    BodyRenderPlan plan;               // repaint=true:渲染预案
};

inline std::vector<BodyRenderStep> PlanBodyDelta(BodyScanState& state, const std::string& block_so_far,
                                                 const std::string& text, const lubancode::cli::Theme& theme,
                                                 int width) {
    const std::vector<BodyDeltaStep> raw_steps = ScanBodyDelta(state, text, block_so_far);
    std::vector<BodyRenderStep> steps;
    steps.reserve(raw_steps.size());
    for (const BodyDeltaStep& raw : raw_steps) {
        BodyRenderStep step;
        step.repaint = raw.repaint;
        step.finalize = raw.finalize;
        if (raw.repaint) {
            step.plan = PrepareBodyRenderPlan(raw.text, theme, width, state.rendered_before);
            if (raw.finalize) {
                // 下一块的前距看这一块的收束:定格成渲染版(块尾空行被擦)
                // → 下一块自带前距;原样保留(空行还在屏上)→ 不带,免得
                // 两行空行。空块收束步(hit=false)照原样保留论。
                state.rendered_before = step.plan.hit;
            }
        } else {
            step.piece = raw.text;
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

class StreamBodyTracker {
public:
    // 原地重画能力探不到时直接按 enabled=false 走：OnDelta 退化成“拿锁
    // 原样打印”，信息不丢。POSIX 会先发一次 DSR 真探，不凭平台名猜。
    // silent(查看态回流单):正文一个字节都不上屏——只攒进 silent_body_,
    // 收口时由调用方取走进台账。用户此刻正看别的子代理的查看帧,main 的
    // 回流输出不能糊上去;数据不能丢,回 main 重铺要看得见。
    StreamBodyTracker(const lubancode::cli::Theme& theme, bool enabled, bool silent = false)
        : theme_(theme), enabled_(enabled && lubancode::platform::SupportsScreenRepaint()), silent_(silent) {}

    StreamBodyTracker(const StreamBodyTracker&) = delete;
    StreamBodyTracker& operator=(const StreamBodyTracker&) = delete;

    // 静默档攒下的正文全文(截 kFullOutputCapBytes);非静默档恒空串。
    std::string TakeSilentBody() { return std::move(silent_body_); }

    // 流式正文增量:原样打印 + 记账。条 4(画面隔网):正文拼装与
    // Markdown 解析挪到输出锁外——锁外先把这笔增量切段(段落边界/围栏
    // 账)、把要收束重画的段落渲染成行,锁内只剩脚注擦画、落笔与锚点账。
    // 解析窗口内心跳/监听线程照样拿得到锁补画,正文解析再重也不饿心跳。
    // M10 的锁规矩不变:流式期间的 std::cout 写都拿 StdoutWriteMutex,
    // 跟监听线程的 "[已打断]"/"[已排队]" 错开。
    void OnDelta(const std::string& text) {
        if (silent_) {
            silent_body_ += text;
            if (silent_body_.size() > lubancode::cli::kFullOutputCapBytes) {
                silent_body_ = lubancode::cli::TruncateUtf8Bytes(silent_body_,
                                                                lubancode::cli::kFullOutputCapBytes);
            }
            return;  // 不打印、不碰 footer、不动行数账——屏幕此刻归查看帧
        }
        // ---- 锁外:拼装(切段/围栏账/段落边界)与 Markdown 解析 ----
        // (enabled_=false 时老路压根不进这套账,照旧整笔直打,这里不扫。)
        std::vector<BodyRenderStep> steps;
        if (enabled_) {
            steps = ScanDelta(text);
        }
        // ---- 锁内:落笔(脚注擦画、分段原样打、收束重画) ----
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        // P2-4 流式重绘风暴:旧路每笔 delta 都整框擦掉再整框重画,行级
        // diff 与指纹跳帧形同虚设。改外科准备——光标钉回正文续写点,正文
        // 压到哪行才清哪行、只标脏那些行;正文写在自家行上、脚注没动的
        // 那一拍,收尾的 diff 一个字节都不写。改宽照旧走整框追账。
        if (enabled_) {
            int newlines = 0;
            for (const char c : text) {
                newlines += c == '\n' ? 1 : 0;
            }
            lubancode::cli::PrepareStreamBodyWriteLocked(
                newlines, static_cast<int>(lubancode::cli::DisplayWidthUtf8(text)));
        } else {
            // footer 没启用(非真终端或能力探测失败)时这句是空操作。
            lubancode::cli::EraseStreamFooterLocked();
        }
        // 工具终态行本身以换行收尾。下一段正文若直接落笔，视觉上便会
        // 贴住最后一条工具；这里另起一空行，且不把它塞进 Markdown 缓冲，
        // 免得收束重画时被 RenderMarkdown 的头尾裁剪吃掉。
        if (separate_next_body_) {
            TermOut() << "\n";
            TermOut().flush();
            separate_next_body_ = false;
        }
        if (!enabled_) {
            TermOut() << text;
            TermOut().flush();
            lubancode::cli::RedrawStreamFooterLocked();
            return;
        }

        for (const BodyRenderStep& step : steps) {
            if (!step.repaint) {
                PrintPieceLocked(step.piece);
            } else if (step.finalize) {
                RepaintBlockLocked(step.plan);
            } else {
                RepaintBlockInPlaceLocked(step.plan);
            }
        }
        // 正文这笔写完:帧账里的续写点拨到新光标,脚注框顶跟着算对。
        lubancode::cli::NoteStreamBodyCursorLocked();
        lubancode::cli::RedrawStreamFooterLocked();  // 脚注重画到正文当前底部下方
    }

    // 监听线程在流式正文当中插打了整行提示([已排队]/[已打断]):这几行
    // 不在本块的行数账上,"块首行号 + 光标位移"这笔账从此骗人——作废当前
    // 块,收束时保持原样不重画,用户的回显一根汗毛不动;下一块(工具条目
    // 之后)照常重新取锚,不受牵连。经 cli::SetStreamScreenPrintHook 由监听
    // 线程调,调用方彼时正持有 StdoutWriteMutex(跟 OnDelta 里读写 unsafe_
    // 的锁是同一把),这里不再锁、也不能再锁(非递归)。
    void InvalidateBlockAnchor() { unsafe_ = true; }

    // footer / 状态块主动滚屏后的对账口子。调用方已持有 stdout 锁。
    void OnScreenScrolledLocked(int rows) {
        if (!enabled_ || !in_block_ || unsafe_ || rows <= 0) {
            return;
        }
        if (rows > start_row_) {
            unsafe_ = true;
            return;
        }
        start_row_ -= rows;
    }

    // 工具条目/思考块要开画了:当前正文块到此为止。老路只清账、屏上保持
    // 原样——真实实测问题 1 的根因即此:模型把正文段与 tool_use 写在同一
    // 条消息里、正文后没有空行,块等不到收束就被丢,`**` 星号永远露着。
    // 现在先把未收束的块按渲染版定格(锁外备预案,锁内落笔,条 4 同款),
    // 几何绝境(unsafe)时照老路原样保留——宁可漏渲染,不可错渲染。
    void OnBlockBreak() {
        std::optional<BodyRenderPlan> pending_plan;
        if (enabled_ && in_block_ && !buffer_.empty()) {
            pending_plan = PrepareBodyRenderPlan(buffer_, theme_,
                                                 lubancode::cli::DetectConsoleWidth().value_or(80),
                                                 scan_.rendered_before);
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        // 工具条目要开画/换请求了:脚注这行先擦掉,免得它残留在工具输出或
        // 下一轮"思考中"转轮当中(转轮跟 footer 同处一行会打架)。下一块
        // 正文到来时 OnDelta 会重新把脚注摆到正文下方。footer 没启用时空操作。
        lubancode::cli::EraseStreamFooterLocked();
        if (pending_plan.has_value()) {
            RepaintBlockLocked(*pending_plan);  // 收束重画(内部自清 in_block_/buffer_)
        }
        // 若上一个工具后接的仍是工具，TranscriptPainter::PaintNew 自会
        // 留一空行；别把这枚标志带到更后面的正文，再多垫一层。
        separate_next_body_ = false;
        if (!enabled_) {
            return;
        }
        in_block_ = false;
        buffer_.clear();
        scan_.line_probe.clear();
        scan_.fence_open = false;
        scan_.last_newlines = 0;
        scan_.last_pairs = 0;
        // 正文另起(问题 3 的账随块清):下一块按"开头第一块"论——不带
        // 前距(与工具条目的空行由 separate_next_body_ 管),开头空行照吸。
        scan_.blank_run = true;
        scan_.rendered_before = false;
    }

    // 工具终态已经画完。暂不落笔，等正文真来了再补分隔；若没有后续
    // 正文，回合末尾也不会凭空多出空白。
    void OnToolBlockDone() { separate_next_body_ = true; }

    // 回合收束:最后一块正文已完整——检测到 markdown 结构就整块擦掉重画
    // 渲染版,否则一字不动。只在 Run() 正常返回且没被打断时由 RunTurn 调
    //(此刻 UI 泵已收,画面单线程)。条 4 同款:解析在锁外,锁内只做
    // 几何与落笔。
    void FinalizeRepaint() {
        if (!enabled_ || !in_block_) {
            return;
        }
        const BodyRenderPlan plan = PrepareBodyRenderPlan(buffer_, theme_,
                                                          lubancode::cli::DetectConsoleWidth().value_or(80),
                                                          scan_.rendered_before);
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        RepaintBlockLocked(plan);
        scan_.line_probe.clear();
        scan_.fence_open = false;
        scan_.last_newlines = 0;
        scan_.last_pairs = 0;
        scan_.blank_run = true;
        scan_.rendered_before = false;
    }

private:
    // OnDelta 的锁外半边(条 4):切段 + 围栏账 + 段落边界探测 + 边界处的
    // Markdown 解析。全部决策在纯函数 PlanBodyDelta(见上,单测钉死:
    // 一段只渲染一次,收口即清零;行/标记边界增量重画问题 1);这里只补
    // 终端宽度探测。line_probe_/fence_open_ 只在这一路动;动它们的人(这
    // 里与锁内 OnBlockBreak 清账)全被泵的画笔锁串着,锁外的这一窗不会
    // 与别人劈腿。buffer_ 只读起步一份投影(收束预案要"边界那一刻"的整
    // 块正文),锁内 PrintPieceLocked 落一笔补一笔,账对得上。
    std::vector<BodyRenderStep> ScanDelta(const std::string& text) {
        return PlanBodyDelta(scan_, in_block_ ? buffer_ : std::string(), text, theme_,
                             lubancode::cli::DetectConsoleWidth().value_or(80));
    }

    void StartBlockLocked() {
        in_block_ = true;
        unsafe_ = false;
        buffer_.clear();
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (info.has_value() && info->cursor_x == 0) {
            start_row_ = info->cursor_y;
        } else {
            unsafe_ = true;
        }
    }

    void PrintPieceLocked(const std::string& text) {
        if (text.empty()) {
            return;
        }
        if (!in_block_) {
            StartBlockLocked();
        }
        if (!unsafe_) {
            const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
            if (info.has_value()) {
                int rows_needed = 2;
                for (const char c : text) {
                    rows_needed += c == '\n' ? 1 : 0;
                }
                rows_needed += static_cast<int>(lubancode::cli::DisplayWidthUtf8(text)) /
                               (std::max)(1, info->width);
                // 帧账原语(带锚点护栏):长缓冲平移视口,贴底滚内容;要滚的
                // 比块首上方还多(-1)就记 unsafe,块保持原样不重画。
                const int overflow =
                    lubancode::cli::EnsureViewportRowsForAnchorLocked(start_row_, info->cursor_y, rows_needed);
                if (overflow < 0) {
                    unsafe_ = true;
                } else if (overflow > 0) {
                    start_row_ -= overflow;
                    // 滚屏对账:脚注帧随内容上移,帧账跟上,不然 diff 会拿
                    // 旧行当新行跳过(见 ShiftStreamFooterFrameOriginLocked)。
                    lubancode::cli::ShiftStreamFooterFrameOriginLocked(overflow);
                    lubancode::platform::SetCursorPos(info->cursor_x, info->cursor_y - overflow);
                }
            } else {
                unsafe_ = true;
            }
        }
        TermOut() << text;
        TermOut().flush();
        buffer_ += text;
    }

    // 收束重画(空行收束/OnBlockBreak 让路/FinalizeRepaint):画完清账,
    // 块就此定格。unsafe_ 在这一刻现读(老路同款——监听线程插行的作废标
    // 要走到落笔这一步才算数),预案白算的情形(作废/几何绝境)只浪费
    // CPU,不差画。
    void RepaintBlockLocked(const BodyRenderPlan& plan) {
        if (!in_block_) {
            return;
        }
        PaintBlockLocked(plan);
        in_block_ = false;
        buffer_.clear();
    }

    // 增量重画(问题 1:行/标记边界):块按当前累计正文整块重画,但攒账
    // 不清、块继续——后续正文从渲染版末尾原样续打,下次边界再整块对齐。
    // 画不动(unsafe/没探到结构/几何绝境)就什么都不动,原样账还在。
    void RepaintBlockInPlaceLocked(const BodyRenderPlan& plan) {
        if (!in_block_ || unsafe_) {
            return;
        }
        PaintBlockLocked(plan);
    }

    // 重画的落笔半边(条 4:吃锁外算好的预案,锁内只剩几何与写屏)。
    // 只动屏幕与锚点账,不动 in_block_/buffer_——收束与否由外壳定。
    void PaintBlockLocked(const BodyRenderPlan& plan) {
        if (unsafe_ || buffer_.empty() || !plan.hit) {
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            return;
        }
        const int buffer_height = info->height;
        const int buffer_width = info->width;
        int viewport_x = info->viewport_x;
        int viewport_y = info->viewport_y;
        // 原样块占的物理行数按光标位移算(末行没换行、光标停在行中时也算
        // 一行),不逐字模拟折行。增量重画后光标停在渲染版末尾,这笔账同样
        // 成立——渲染版行数即物理行数。
        const int old_rows = info->cursor_y - start_row_ + (info->cursor_x > 0 ? 1 : 0);
        if (old_rows <= 0) {
            return;
        }
        const std::vector<std::string>& lines = plan.lines;
        const bool ended_with_newline = plan.ended_with_newline;
        if (lines.empty()) {
            return;
        }
        const int new_rows = static_cast<int>(lines.size());
        // 渲染版比原样块高(标题前后的空行、表格边线都要地方)、又伸出可视
        // 窗口底:照 OnDelta 同一套,先按帧账原语腾够(长缓冲平移视口、贴底
        // 滚内容)、start_row_ 随滚动上移;要滚的比块首上方还多(-1)才放弃
        // (原样保留,信息不丢)。平移/滚动都会动 viewport 原点,随后
        // TerminalBatch 的坐标系要重探。
        {
            const int overflow =
                lubancode::cli::EnsureViewportRowsForAnchorLocked(start_row_, start_row_, new_rows + 1);
            if (overflow < 0) {
                return;
            }
            if (overflow > 0) {
                start_row_ -= overflow;
                // 滚屏对账:同 PrintPieceLocked,脚注帧账跟着内容上移。
                lubancode::cli::ShiftStreamFooterFrameOriginLocked(overflow);
            }
            // 平移视口(返回 0)与滚内容(返回 >0)都会动 viewport 原点,
            // 重探不问返回值。
            if (const auto after_scroll = lubancode::platform::GetScreenInfo(); after_scroll.has_value()) {
                viewport_x = after_scroll->viewport_x;
                viewport_y = after_scroll->viewport_y;
            }
        }
        const int rows_to_clear = (std::max)(old_rows, new_rows);
        // 渲染块要压进脚注区时,把压住的脚注行在帧账里标脏(物理清行下面
        // 自己办)——不然下一拍 diff 以为它们还在屏上,跳过不重画。
        lubancode::cli::MarkStreamFooterRowsDirtyLocked(start_row_, rows_to_clear);
        std::string rendered;
        for (int i = 0; i < new_rows; ++i) {
            rendered += lines[static_cast<std::size_t>(i)];
            if (i + 1 < new_rows) {
                rendered += '\n';
            }
        }
        if (ended_with_newline) {
            rendered += '\n';
        }

        if (SupportsVtBatch()) {
            lubancode::platform::TerminalBatch batch(viewport_x, viewport_y);
            for (int r = 0; r < rows_to_clear && start_row_ + r < buffer_height; ++r) {
                batch.ClearRowFrom(0, start_row_ + r, buffer_width);
            }
            batch.MoveTo(0, start_row_);
            batch.Write(rendered);
            batch.Flush();
        } else {
            for (int r = 0; r < rows_to_clear && start_row_ + r < buffer_height; ++r) {
                lubancode::platform::ClearRowFrom(0, start_row_ + r, buffer_width);
            }
            lubancode::platform::SetCursorPos(0, start_row_);
            TermOut() << rendered;
            TermOut().flush();
        }
        // 渲染版每行都截到 width-1,绝不物理折行;末行按原样块带不带换行
        // 收梢——原样流式一致,RunTurn 随后那个 "\n" 照常把行关上,下游
        // 行为分毫不差。
    }

    const lubancode::cli::Theme& theme_;
    bool enabled_;
    bool silent_ = false;
    std::string silent_body_;
    bool in_block_ = false;
    bool unsafe_ = false;
    int start_row_ = 0;
    std::string buffer_;
    BodyScanState scan_;
    bool separate_next_body_ = false;
};

}  // namespace lubancode::cli
