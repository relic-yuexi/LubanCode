// TUI 排版批 7:CLI 子命令(非交互形态)共用的人看输出小样板。
//
// 批 1-6 的 frame 落点都在 src/app/commands(slash 命令,会话主题由调用
// 方递);批 7 起进入 src/cli 的裸子命令——七个命令文件共用同一套"框宽
// 探测 / 逐行落盘 / 句子拆键值对 / 键值对提示框"小件,与其每个文件抄
// 一份匿名空间(批 2 hook_check 的做法),不如收进这一只 header-only
// 头。全 inline,零构建清单改动。
//
// 合同(单子批 7,与 docs/development/tui_style.md 总规矩同源):
//   - 只动"人看"提示的排版;--json/机器消费分支字节级不变;
//   - 颜色全走 Theme(无 tool_accent,一律 table_pass 裁量);plain 主题
//     下零转义零框,由 frame 三助手保;
//   - 文案全由调用方递(既有句子原样,SentenceField 只拆句内既有冒号,
//     不添不改一个字)——i18n 键不新增。
#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "cli/terminal_frame.hpp"  // frame::RenderKeyValues 等三助手
#include "cli/terminal_port.hpp"   // TermOut
#include "cli/theme.hpp"           // Theme/ResolveTheme/DetectConsoleCapability
#include "platform/console.hpp"    // GetScreenInfo:框宽探测

namespace lubancode::cli {

// CLI 子命令没有会话主题,按批 2 hook_check 先例现起一只:管道/重定向/
// 测试进程自然降 plain(--no-color、T3 同路)。
inline Theme CliTheme() {
    return ResolveTheme(std::string(), DetectConsoleCapability().colors_enabled);
}

// 框宽:探测到的终端列数;探不到(管道/测试)给 0 = 按内容自适应。
inline int CliFrameWidth() {
    if (const auto info = platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

// frame 行逐行落盘(行内无换行符,这里补上)。
inline void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

// 句子拆键值对(批 1 裁量 2 的"SentenceField 模式"):按第一个半角冒号
// 拆两列,拆的是既有文案,不添不改一个字;没有冒号的整句进 value。
inline frame::Field SentenceField(const std::string& sentence,
                                  frame::FieldAccent accent = frame::FieldAccent::None) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    std::string key = sentence.substr(0, colon);
    std::string value = sentence.substr(colon + 1);
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), not_space));
    key.erase(std::find_if(key.rbegin(), key.rend(), not_space).base(), key.end());
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return frame::Field{key, value, accent};
}

// 键值对提示框(批 2 PrintNotice 先例):整句列表逐句拆列进框,零标题。
// 自带换行的多行文案(record.started 一族)按行拆、行序不变逐行进列——
// frame 行内不装换行符,塞进去框就破了。
inline void PrintNotice(const Theme& theme, std::initializer_list<std::string> sentences,
                        frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        std::size_t start = 0;
        while (start <= sentence.size()) {
            const std::size_t end = sentence.find('\n', start);
            const std::string piece = end == std::string::npos
                                          ? sentence.substr(start)
                                          : sentence.substr(start, end - start);
            if (!piece.empty()) {
                fields.push_back(SentenceField(piece, accent));
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), CliFrameWidth()));
}

// 引导下文的尾冒号剥掉(批 2 裁量 3:标题化时尾冒号是废话)。半角 ":" 一
// 字节,全角 "：" UTF-8 三字节(EF BC 9A),都剥;尾随空白顺手扫掉。
inline std::string StripTrailingColon(std::string text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[text.size() - 3]) == 0xEF &&
        static_cast<unsigned char>(text[text.size() - 2]) == 0xBC &&
        static_cast<unsigned char>(text[text.size() - 1]) == 0x9A) {
        text.resize(text.size() - 3);
    } else if (!text.empty() && text.back() == ':') {
        text.pop_back();
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

}  // namespace lubancode::cli
