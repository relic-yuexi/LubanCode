// 三个菜单面板共用的 UTF-8 文本槽编辑内核(HC-04 合并单)。ChoiceMenu 自填
// 项、ProviderSwitch 筛选词、SessionPicker 搜索词原先各养一份逐分支相同的
// AppendUtf8/EraseLastUtf8 私有函数,合并成这一份。三套按键状态机、粘贴归一
// 化、过滤规则仍归各面板,这里只管两件事:往 UTF-8 字符串末尾放一个码点、
// 删末尾一个码点。
//
// 刀口语义(与合并前三份实现逐字节相同,行为零变更):
//   - AppendUtf8:cp <= 0x10FFFF 才落盘(1~4 字节),越界值整码点丢弃,一个
//     字节不出;UTF-16 代理项落在 3 字节支路照编(WTF-8 风格)——这是既有
//     行为,原样保留,不许顺手"修"成拒绝。
//   - EraseLastUtf8:删末尾一个码点。从末字节起退过续字节(10xxxxxx),连头
//     字节一并抹掉;空串原样不动。字节流若以孤立续字节开头,会一路退到串首
//     整段抹掉(既有行为,原样保留)。
//   - 删的是码点,不是字素簇:组合附标、ZWJ 序列各算独立码点,逐个退。整簇
//     删除另立需求,不许在这份内核里偷改。
//
// 界限两条:
//   - line_editor.cpp 的 Utf32ToUtf8 不是同一合同:那边末支无条件编码,越界
//     值不丢。整串编码的非法码点策略未对照定案前,不许拿这份替换那份。
//   - platform/text_encoding.hpp 的 TruncateUtf8Prefix 是按字节帽截前缀的
//     另一把尺(码点/列宽截断它声明不归管);尾删的"退续字节"循环与
//     Utf8PrefixBoundary 同形,但纯逻辑头不为此挂 platform 依赖,内核自带。
#pragma once

#include <cstddef>
#include <string>

namespace lubancode::cli {

// 向 UTF-8 文本槽末尾追加单个码点(合同见文件头)。
inline void AppendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 删除 UTF-8 文本槽末尾的一个码点(合同见文件头)。
inline void EraseLastUtf8(std::string& text) {
    if (text.empty()) {
        return;
    }
    std::size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    text.erase(pos);
}

}  // namespace lubancode::cli
