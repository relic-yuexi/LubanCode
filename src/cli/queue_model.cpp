// 待发消息队列的纯逻辑核。设计说明见 queue_model.hpp 文件头;键位规则与
// 规格(todo"排队输入要改得自然"一节)逐条对齐,单测钉在
// tests/test_queue_model.cpp。

#include "cli/queue_model.hpp"

#include <utility>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // Utf32ToUtf8

namespace lubancode::cli {

namespace {

// UTF-8 -> UTF-32 解码。line_editor.cpp 里那份是匿名命名空间的,这里自备
// 一份小的(取出来的待发消息装回编辑 buffer 用)。输入都是本程序自己拼的
// UTF-8,非法序列按"跳过一个字节"处理,同 line_editor 的取舍。
std::u32string DecodeUtf8(const std::string& text) {
    std::u32string out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (c0 < 0x80) {
            cp = c0;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            extra = 1;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            extra = 2;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        bool ok = true;
        for (std::size_t k = 0; k < extra; ++k) {
            if (i + 1 + k >= n) {
                ok = false;
                break;
            }
            const unsigned char ck = static_cast<unsigned char>(text[i + 1 + k]);
            if ((ck & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

}  // namespace

std::vector<std::string> PendingQueueCore::display_items() const {
    if (!editing_) {
        return items_;
    }
    std::vector<std::string> out;
    out.reserve(items_.empty() ? 0 : items_.size() - 1);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != edit_index_) {
            out.push_back(items_[i]);
        }
    }
    return out;
}

std::string PendingQueueCore::echo_text() const { return Utf32ToUtf8(buffer_); }

PendingQueueCore::Event PendingQueueCore::TypeChar(char32_t ch) {
    if (ch < 0x20) {
        return Event::None;  // 控制字符不进 buffer(调用方也只会喂可打印键)
    }
    buffer_.push_back(ch);
    return Event::Edited;
}

PendingQueueCore::Event PendingQueueCore::Backspace() {
    if (buffer_.empty()) {
        return Event::None;
    }
    buffer_.pop_back();
    return Event::Edited;
}

void PendingQueueCore::EnterEditAt(std::size_t index) {
    editing_ = true;
    edit_index_ = index;
    restore_text_ = items_[index];
    buffer_ = DecodeUtf8(restore_text_);
}

PendingQueueCore::Event PendingQueueCore::MoveUp() {
    if (editing_) {
        // 先把改到一半的 buffer 写回当前位,再往旧走;到头钳住。
        items_[edit_index_] = Utf32ToUtf8(buffer_);
        if (edit_index_ == 0) {
            return Event::Edited;
        }
        EnterEditAt(edit_index_ - 1);
        return Event::Edited;
    }
    if (!buffer_.empty() || items_.empty()) {
        return Event::None;  // 只有"空输入"才取回待发消息(规格)
    }
    EnterEditAt(items_.size() - 1);
    return Event::Edited;
}

PendingQueueCore::Event PendingQueueCore::MoveDown() {
    if (!editing_) {
        return Event::None;
    }
    items_[edit_index_] = Utf32ToUtf8(buffer_);
    if (edit_index_ + 1 < items_.size()) {
        EnterEditAt(edit_index_ + 1);
        return Event::Edited;
    }
    // 已经是最后(最新)一条:退出编辑态,回到"敲新消息"的起点。
    editing_ = false;
    buffer_.clear();
    restore_text_.clear();
    return Event::Edited;
}

PendingQueueCore::Event PendingQueueCore::Submit() {
    if (editing_) {
        items_[edit_index_] = Utf32ToUtf8(buffer_);  // 原位替换
        editing_ = false;
        buffer_.clear();
        restore_text_.clear();
        return Event::Submitted;
    }
    if (buffer_.empty()) {
        return Event::None;
    }
    items_.push_back(Utf32ToUtf8(buffer_));
    buffer_.clear();
    return Event::Submitted;
}

PendingQueueCore::Event PendingQueueCore::CancelEdit() {
    if (!editing_) {
        return Event::None;
    }
    items_[edit_index_] = restore_text_;  // 原文放回,未提交的修改丢弃
    editing_ = false;
    buffer_.clear();
    restore_text_.clear();
    return Event::Restored;
}

PendingQueueCore::Event PendingQueueCore::DeleteCurrent() {
    if (!editing_) {
        return Event::None;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(edit_index_));
    editing_ = false;
    buffer_.clear();
    restore_text_.clear();
    return Event::Deleted;
}

std::vector<std::string> PendingQueueCore::TakeAll() {
    std::vector<std::string> out = std::move(items_);
    items_.clear();
    editing_ = false;
    buffer_.clear();
    restore_text_.clear();
    return out;
}

std::vector<std::string> BuildPendingQueueRows(const std::vector<std::string>& items, std::size_t visible_cap) {
    std::vector<std::string> rows;
    if (items.empty()) {
        return rows;
    }
    const std::size_t visible = items.size() < visible_cap ? items.size() : visible_cap;
    if (items.size() > visible_cap) {
        // 只添一行汇总,不逐条摆最旧的多余项(规格:超过可见上限,只添一行)。
        rows.push_back(trf("input.queue_more", items.size() - visible_cap));
    }
    const std::size_t first = items.size() - visible;
    for (std::size_t i = 0; i < visible; ++i) {
        rows.push_back("  \xE2\x80\xBA " + items[first + i]);  // "  › "
    }
    return rows;
}

}  // namespace lubancode::cli
