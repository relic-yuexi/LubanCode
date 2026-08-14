// 待发消息队列的纯逻辑核(跨会话传话与自然排队 · 第一步)。
//
// TurnInputListener 流式监听期间攒下的"待发消息"以前只是一根 vector,
// 键入、落队之外没有别的操作。规格(todo"排队输入要改得自然"一节)要:
// 空输入按上键取回最后一条待发消息,改好按 Enter 原位替换;再按上、下在
// 待发消息间走;Delete 删当前项;Esc 放回队列。这些规则全部收在这里,
// 不认终端、不认线程,单测直接钉(doctest 见 tests/test_queue_model.cpp);
// console_input.cpp 的监听线程只管把 platform 按键翻成对这里的方法调用、
// 再把 items 摆到流式 footer 的待发区。
//
// 编辑模型(与规格逐条对齐):
//   - 不在编辑态:敲字进 buffer,Enter 把 buffer 追加进 items(老语义)。
//   - buffer 为空按 Up:进入编辑态,取出 items 末尾一条(最新落队的那条)
//     装进 buffer,该条从待发区消失(挪进了输入行)。
//   - 编辑态按 Up/Down:先把 buffer 写回当前位(改到一半的 edits 不丢),
//     再挪到相邻一条;Down 挪过最后一条时退出编辑态、清空 buffer(回到
//     "敲新消息"的起点)。首位/末位处 Up/Down 钳住不绕圈。
//   - 编辑态按 Enter:buffer 原位替换当前项,退出编辑态。
//   - 编辑态按 Delete:删掉当前项,退出编辑态。
//   - 编辑态按 Esc:把取出来那条的原文放回队列(丢弃未提交的修改),
//     退出编辑态。不在编辑态时 Esc 归调用方(打断当前轮),这里不管。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

class PendingQueueCore {
public:
    // 键处理的结果,调用方据此决定要不要重画 footer、要不要触发发送语义。
    enum class Event {
        None,      // 这个键没改动状态
        Edited,    // buffer/items 变了,重画 footer 即可
        Submitted, // Enter:新增了一条(items 尾部)或原位替换了一条
        Restored,  // Esc:取出的一条按原文放回了队列
        Deleted,   // Delete:当前项被删掉
    };

    // ---- 查询 ----
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    bool editing() const { return editing_; }
    // 待发区展示用:items 去掉正在编辑的那条(它已经挪进输入行了)。
    std::vector<std::string> display_items() const;
    // 发送用:整份 items(TakeQueuedLines 的语义,顺序 = 落队顺序)。
    const std::vector<std::string>& items() const { return items_; }
    // 输入行实时回显:编辑态/敲字态都是 buffer 本身,不拼任何前缀。
    std::string echo_text() const;
    bool buffer_empty() const { return buffer_.empty(); }

    // ---- 键处理(返回 Event;不认的键返回 None、状态不动) ----
    Event TypeChar(char32_t ch);
    Event Backspace();
    Event MoveUp();     // Up
    Event MoveDown();   // Down
    Event Submit();     // Enter
    Event CancelEdit(); // Esc(仅编辑态有意义)
    Event DeleteCurrent(); // Delete(仅编辑态有意义)

    // 取走全部待发消息(原顺序),内部清空。Stop() 之后的线程安全由调用方
    // 保证(监听线程已 join),这里只管搬。
    std::vector<std::string> TakeAll();

private:
    void EnterEditAt(std::size_t index);

    std::vector<std::string> items_;
    std::u32string buffer_;
    bool editing_ = false;
    std::size_t edit_index_ = 0;
    std::string restore_text_;  // 取出那条的原文,Esc 时放回
};

// 待发区怎么摆(纯函数,规格逐条):
//   - 空:没有行。
//   - 不超过 visible_cap 条:逐条摆,每行 "  › <正文>"。一条也不写
//     "待发送消息 1 条"这种汇总头。
//   - 超过 visible_cap 条:最旧的多余条目不逐条摆,顶部添一行
//     "另有 N 条"(i18n key input.queue_more),下面摆最近的 visible_cap 条。
std::vector<std::string> BuildPendingQueueRows(const std::vector<std::string>& items,
                                               std::size_t visible_cap = 3);

}  // namespace lubancode::cli
