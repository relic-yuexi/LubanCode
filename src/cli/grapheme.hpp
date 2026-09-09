// 扩展字素簇(EGC)分段与终端列宽策略——Unicode emoji 治理单 P1 的共享地基。
//
// 四个单位不许互相顶替(单子 §6.1):UTF-8 字节、Unicode scalar value
// (char32_t)、扩展字素簇(用户看见的一个"字")、终端 cell。本文件只管
// 后三者的换算:码点序列 -> 簇边界,簇 -> 终端列数。缓冲区结构与原文
// 不动——不做 NFC/NFKC,不改用户原文一字节,只改移动/删除/截断/换行/
// 量宽的裁决。
//
// 数据口径:Unicode 15.1(UCD 15.1 的 Grapheme_Cluster_Break=Extend 与
// East Asian Width W/F + Emoji_Presentation 机器生成全量表,scripts/
// gen_unicode_tables.py 可重跑,见 grapheme.cpp 文件头的来源与回退说明)。
// 不引 ICU 等第三方依赖。
//
// 宽度策略(主流终端口径,独立测试钉死,不许拿本函数自证):
//   - 组合附标、变体选择符(VS15/VS16)、ZWJ、肤色修饰:零宽,并入所属簇;
//   - 基础 emoji / CJK 宽字:两列;旗帜(区域指示符配对):两列;
//   - keycap(数字/#/* + FE0F? + 20E3):两列;
//   - 文字 + VS16(emoji 呈现):升两列;emoji + VS15(文字呈现):降一列;
//   - ZWJ 序列(如 👩‍💻)整簇取首字宽:两列;
//   - 孤立区域指示符、其余窄字:一列。
// 字素簇 != 恒两列;同一字符在不同终端/字体下实测宽度可能不同,真机
// 校准属于单子 P3,本表只承诺"与主流终端的多数行为一致"。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::cli {

// 码点的簇角色(简化 UAX#29 Grapheme_Cluster_Break,只留分段需要的类)。
enum class GraphemeClass {
    Base,                // 常规基础字:自成簇首,后可跟 Extend/ZWJ
    Extend,              // 组合附标/变体选择符/肤色修饰:并入前一簇
    Zwj,                 // U+200D:并入前一簇,并把下一字符拉进同一簇
    RegionalIndicator,   // U+1F1E6..1F1FF:两两配对成旗帜
    KeycapBase,          // '0'..'9'、'#'、'*':可组 keycap 序列
    HangulL,             // 韩文字母 L/V/T/预成音节:EGC 把音节序列并成一簇
    HangulV,
    HangulT,
    HangulSyllable,
};

GraphemeClass ClassifyCodepoint(char32_t cp);

// 单码点终端列宽。注意:簇的整体列宽不是成员单宽之和(旗帜两枚指示符各
// 一列、整簇两列;VS16 单宽零、整簇升两列),整簇量宽必须用
// ClusterDisplayWidth。单宽只描述"孤立出现时"的占位,以及供逐码点旧
// 调用方(已在 P1 逐一迁走)过渡。
int GraphemeCodepointWidth(char32_t cp);

// 一个字素簇:码点区间 [begin, end) 与整簇终端列宽。
struct GraphemeCluster {
    std::size_t begin;
    std::size_t end;
    int width;
};

// UTF-32 文本按扩展字素簇分段。返回的簇首尾相接、覆盖整个串;空串给空
// vector。任何输入都前进(每簇至少一个码点),不会死循环。
std::vector<GraphemeCluster> SplitGraphemes(const std::u32string& text);

// UTF-8 文本按簇分段(给 markdown 折行、footer ANSI 截断这类字节串消费
// 者)。坏的 UTF-8 序列按"一个坏字节一簇"回退(与 Utf8ToUtf32 的逐字节
// 跳过同口径),簇宽记 1——回退簇必前进,不让截断/折行循环空转。
struct Utf8Grapheme {
    std::size_t begin;  // 字节下标
    std::size_t len;    // 字节数(>= 1)
    int width;          // 整簇终端列宽
};
std::vector<Utf8Grapheme> SplitUtf8Graphemes(std::string_view utf8);

// 整簇终端列宽。簇内码点按"宽度策略"合议:旗帜/keycap/VS16 升两列、
// VS15 降一列、ZWJ 序列取首字宽、附标一律零宽贡献。
int ClusterDisplayWidth(const char32_t* cps, std::size_t count);
int ClusterDisplayWidth(std::u32string_view cluster);

// 编辑边界:pos 往左/往右最近的簇边界。PrevGraphemeBoundary(text, pos)
// 返回 < pos 的最大簇起点(0 < pos <= size 时必有);NextGraphemeBoundary
// (text, pos) 返回 > pos 的最小簇起点(0 <= pos < size 时必有)。
// pos 落在簇中间时,Prev 给该簇起点、Next 给下一簇起点——左右键、
// Backspace/Delete 按"整簇"移动/删除全靠这两枚。
std::size_t PrevGraphemeBoundary(const std::u32string& text, std::size_t pos);
std::size_t NextGraphemeBoundary(const std::u32string& text, std::size_t pos);

}  // namespace lubancode::cli
