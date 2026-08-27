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
// 工具条目要开画时(on_tool_start)，尚未收束的小段作罢、保持原样；下一段
// 重新取锚。已在段落边界画好的 Markdown 不受影响。
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
        std::vector<DeltaStep> steps;
        if (enabled_) {
            steps = ScanDelta(text);
        }
        // ---- 锁内:落笔(脚注擦画、分段原样打、收束重画) ----
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        // 0.21.x 流式脚注:正文落笔前先把脚注那行擦掉(免得跟正文抢行/被正文
        // 顶到中间),落笔后再重画到正文下方——见 console_input.hpp 注释。
        // footer 没启用(非真终端或能力探测失败)时这两句是空操作。
        lubancode::cli::EraseStreamFooterLocked();
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

        for (const DeltaStep& step : steps) {
            if (!step.repaint) {
                PrintPieceLocked(step.piece);
            } else {
                RepaintBlockLocked(step.plan);
            }
        }
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

    // 工具条目要开画了:当前块到此为止,屏上保持原样。
    void OnBlockBreak() {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        // 工具条目要开画/换请求了:脚注这行先擦掉,免得它残留在工具输出或
        // 下一轮"思考中"转轮当中(转轮跟 footer 同处一行会打架)。下一块
        // 正文到来时 OnDelta 会重新把脚注摆到正文下方。footer 没启用时空操作。
        lubancode::cli::EraseStreamFooterLocked();
        // 若上一个工具后接的仍是工具，TranscriptPainter::PaintNew 自会
        // 留一空行；别把这枚标志带到更后面的正文，再多垫一层。
        separate_next_body_ = false;
        if (!enabled_) {
            return;
        }
        in_block_ = false;
        buffer_.clear();
        line_probe_.clear();
        fence_open_ = false;
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
        const BlockRenderPlan plan = PrepareBlockRender(buffer_);
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        RepaintBlockLocked(plan);
        line_probe_.clear();
        fence_open_ = false;
    }

private:
    // 收束重画的预案(条 4:锁外算好,锁内照单落笔)。hit=false 就是
    // "没探到 markdown 结构"——锁内只清账不画。
    struct BlockRenderPlan {
        bool hit = false;                      // DetectMarkdownStructure 命中
        std::vector<std::string> lines;        // 命中:RenderMarkdown 的渲染行
        bool ended_with_newline = false;       // 原样块末尾带不带换行(老账)
    };

    // OnDelta 锁外拼装出的一步:要么落一笔原文,要么按预案收束重画。
    struct DeltaStep {
        bool repaint = false;
        std::string piece;        // repaint=false:原样落笔的段(空段不生成步,
                                  // 与老路 PrintPieceLocked 的空笔 no-op 等价)
        BlockRenderPlan plan;     // repaint=true:收束重画的预案
    };

    // OnDelta 的锁外半边(条 4):切段 + 围栏账 + 段落边界探测 + 边界处的
    // Markdown 解析。line_probe_/fence_open_ 只在这一路动;动它们的人(这
    // 里与锁内 OnBlockBreak 清账)全被泵的画笔锁串着,锁外的这一窗不会
    // 与别人劈腿。buffer_ 只读起步一份投影(收束预案要"边界那一刻"的整
    // 块正文),锁内 PrintPieceLocked 落一笔补一笔,账对得上。
    std::vector<DeltaStep> ScanDelta(const std::string& text) {
        std::vector<DeltaStep> steps;
        std::string projected = in_block_ ? buffer_ : std::string();
        // 长回答不能等到整轮结束才重画：那时块首多半早滚出屏幕。按空行
        // 切成小段，逐段记锚、逐段收束；代码围栏没闭合时不切，免得把块内
        // 空行错当段落边界。这样前一段失去锚点，后一段仍能重新起账。
        std::string piece;
        for (const char c : text) {
            piece += c;
            if (c != '\n') {
                line_probe_ += c;
                continue;
            }
            std::size_t first = 0;
            while (first < line_probe_.size() && (line_probe_[first] == ' ' || line_probe_[first] == '\t')) {
                ++first;
            }
            if (line_probe_.compare(first, 3, "```") == 0) {
                fence_open_ = !fence_open_;
            }
            const bool blank = first == line_probe_.size();
            line_probe_.clear();
            if (blank && !fence_open_) {
                if (!piece.empty()) {
                    projected += piece;
                    DeltaStep print_step;
                    print_step.piece = std::move(piece);
                    steps.push_back(std::move(print_step));
                }
                piece.clear();
                DeltaStep repaint_step;
                repaint_step.repaint = true;
                repaint_step.plan = PrepareBlockRender(projected);
                steps.push_back(std::move(repaint_step));
            }
        }
        if (!piece.empty()) {
            DeltaStep print_step;
            print_step.piece = std::move(piece);
            steps.push_back(std::move(print_step));
        }
        return steps;
    }

    // 收束重画的解析半边(条 4:锁外)。block_text 是"边界那一刻"的整块
    // 正文。宽度探测与渲染都在锁外——探测是只读查询,PrintDivider 一类
    // 早有不持锁调它的先例;渲染纯 CPU。
    BlockRenderPlan PrepareBlockRender(const std::string& block_text) const {
        BlockRenderPlan plan;
        plan.hit = lubancode::cli::DetectMarkdownStructure(block_text);
        if (!plan.hit) {
            return plan;
        }
        plan.ended_with_newline = !block_text.empty() && block_text.back() == '\n';
        plan.lines = lubancode::cli::RenderMarkdown(
            block_text, theme_, lubancode::cli::DetectConsoleWidth().value_or(80));
        return plan;
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

    // 收束重画的落笔半边(条 4:吃锁外算好的预案,锁内只剩几何与写屏)。
    // unsafe_ 在这一刻现读(老路同款——监听线程插行的作废标要走到落笔
    // 这一步才算数),预案白算的情形(作废/几何绝境)只浪费 CPU,不差画。
    void RepaintBlockLocked(const BlockRenderPlan& plan) {
        if (!in_block_) {
            return;
        }
        in_block_ = false;
        if (unsafe_ || buffer_.empty() || !plan.hit) {
            buffer_.clear();
            return;
        }
        const std::optional<lubancode::platform::ScreenInfo> info = lubancode::platform::GetScreenInfo();
        if (!info.has_value()) {
            buffer_.clear();
            return;
        }
        const int buffer_height = info->height;
        const int buffer_width = info->width;
        int viewport_x = info->viewport_x;
        int viewport_y = info->viewport_y;
        // 原样块占的物理行数按光标位移算(末行没换行、光标停在行中时也算
        // 一行),不逐字模拟折行。
        const int old_rows = info->cursor_y - start_row_ + (info->cursor_x > 0 ? 1 : 0);
        if (old_rows <= 0) {
            buffer_.clear();
            return;
        }
        const std::vector<std::string>& lines = plan.lines;
        const bool ended_with_newline = plan.ended_with_newline;
        buffer_.clear();
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
            }
            // 平移视口(返回 0)与滚内容(返回 >0)都会动 viewport 原点,
            // 重探不问返回值。
            if (const auto after_scroll = lubancode::platform::GetScreenInfo(); after_scroll.has_value()) {
                viewport_x = after_scroll->viewport_x;
                viewport_y = after_scroll->viewport_y;
            }
        }
        const int rows_to_clear = (std::max)(old_rows, new_rows);
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
        // 渲染版每行都截到 width-1,绝不物理折行;末行不带换行收梢,跟原样
        // 流式一致——RunTurn 随后那个 "\n" 照常把行关上,下游行为分毫不差。
    }

    const lubancode::cli::Theme& theme_;
    bool enabled_;
    bool silent_ = false;
    std::string silent_body_;
    bool in_block_ = false;
    bool unsafe_ = false;
    int start_row_ = 0;
    std::string buffer_;
    std::string line_probe_;
    bool fence_open_ = false;
    bool separate_next_body_ = false;
};

}  // namespace lubancode::cli
