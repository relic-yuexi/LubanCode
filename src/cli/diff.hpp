// UI-C(0.13.0):edit_file/write_file 确认前统一 diff 预览的纯函数层。
//
// 这里只管"两段文本怎么比、比出来怎么排版",不碰任何 IO——读文件在
// main.cpp(cli 接线层)做,tools/ 层一个字不动。三层拆开,各自可单测:
//   1. ComputeLineDiff:行级 LCS(手写朴素 DP,先剥公共前后缀,中段规模
//      超限退化成整删整增,不引库、不爆内存);
//   2. FormatDiff:排版——行号栏 + -/+/空格前缀,变更行前后各留 3 行
//      上下文,中间未变的长段折叠成一行标注;彩色主题走 Claude Code
//      Update 样式:- 行整行红底(theme.diff_del_bg)、+ 行整行绿底
//      (theme.diff_add_bg),底色只铺到内容实际结尾(行尾即 reset,不
//      填充到终端宽),上下文行行号栏淡色(theme.diff_line_no)、正文
//      原色,折叠/截断标注不上底;超行数/字节双限截断,并标注省略了
//      多少行;
//   3. BuildEditDiff / BuildWriteDiff:按两个工具的入参语义拼 diff 输入
//      (edit 拿真文件内容做整文替换后对比,天然带上下文和真实行号;
//      找不到 old_string 回退成只比 old/new 两段;write 对比旧文件与新
//      内容,新文件全部算新增)。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/theme.hpp"

namespace lubancode::cli {

enum class DiffLineKind { Context, Del, Add };

struct DiffLine {
    DiffLineKind kind = DiffLineKind::Context;
    std::string text;
    int old_no = 0;  // 旧文件里的行号(1 起);Add 行没有,是 0
    int new_no = 0;  // 新文件里的行号(1 起);Del 行没有,是 0
};

// 按 \n 切行:行尾的 \r 一并剥掉(CRLF 磁盘文件跟模型给的 LF 文本才对
// 得上,不然满屏全是假差异);末尾没有换行符的最后一截也算一行;空串
// 给空 vector。
std::vector<std::string> SplitDiffLines(const std::string& text);

// 行级 diff:先剥公共前后缀(都记成 Context),中段跑朴素 LCS DP;中段
// 规模(行数乘积)超过内置上限时不硬算,退化成"旧的整删 + 新的整增"——
// 结果照样是一份合法的编辑脚本,只是不追求最小。
std::vector<DiffLine> ComputeLineDiff(const std::vector<std::string>& old_lines,
                                       const std::vector<std::string>& new_lines);

// 排版成可打印的多行文本(每行 \n 收尾)。
//   width     每行显示宽度上限(含行号栏),超宽按显示宽度截断加 "...";
//             <= 0 不截宽。
//   max_lines 正文最多多少行;<= 0 不限。
//   max_bytes 正文(不含 ANSI 色码)最多多少字节;0 不限。
// 两限任一触顶就停,补一行 "… 已省略 N 行(Ctrl+E 查看完整)"。
// 变更行前后各 3 行上下文,更远的未变段折叠成 "…(N 行未变)"。没有任何
// 变更时给 "(无变化)"。
std::string FormatDiff(const std::vector<DiffLine>& diff, const Theme& theme, int width, int max_lines,
                        std::size_t max_bytes, std::string_view file_path = {});

// edit_file 的 diff 输入。located 为真:old_string 在文件里找到了,lines
// 是"整份旧文件 vs 替换后全文"的 diff(FormatDiff 折叠后自然只剩变更处
// ±3 行上下文,行号是真实行号);为假:文件里找不到(或文件读不出来),
// lines 回退成只对比 old_string/new_string 两段,行号是段内行号。
// replaced_count:预计替换几处(replace_all 时是出现次数,否则 1;没找到
// 是 0)。
struct EditDiffPreview {
    std::vector<DiffLine> lines;
    std::size_t replaced_count = 0;
    bool located = false;
};
EditDiffPreview BuildEditDiff(const std::string& file_content, const std::string& old_string,
                               const std::string& new_string, bool replace_all);

// write_file 的 diff 输入:old_content 有值(目标已存在)做行级对比;
// nullopt(新文件)全部记成 Add。
std::vector<DiffLine> BuildWriteDiff(const std::optional<std::string>& old_content,
                                      const std::string& new_content);

}  // namespace lubancode::cli
