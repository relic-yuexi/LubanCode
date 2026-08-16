// @ 项目文件/目录模糊提及菜单(交互抛光总账第三批)的纯逻辑层:模糊
// 匹配、菜单行渲染、光标处 @ 词元的识别与替换。终端层(console_input)管
// 按键与落笔;文件索引由应用层(interactive_session)扫 cwd/Git 根给
// (排除 .git/构建产物)。
//
// 规矩(规格第二批第 2 条):
//   - 显示用相对路径;图片走既有视觉附件路(@路径 + 图片扩展名那套,
//     PrepareImageInput),文本与目录留作正文提及,提交前由应用层校验
//     目标还在、没跑出工作区,并附一份"相对 → 绝对"的提及账,不叫模型
//     猜裸路径;
//   - 词元 = '@' + 一串非空白字符;带空格/中文的路径插成 @<相对路径>
//     形式,尖括号内允许空白;
//   - Backspace 在词元尾按下删整枚(编辑器核心 DeleteBackward 认这一条)。
//
// 纯逻辑,不碰终端/磁盘;tests/test_mention_menu.cpp 钉死。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::cli {

// 一条可提及的文件/目录(相对根的路径,UTF-8,正斜杠)。
struct FileMentionEntry {
    std::string relative_path;
    bool is_dir = false;
};

// @ 词元:编辑行(u32)里从 '@' 起到词尾(或 <...> 闭角)的一段。
// start/end 是码点下标,含 '@'。
struct MentionToken {
    std::size_t start = 0;
    std::size_t end = 0;      // 半开区间
    std::string query;        // '@' 后的查询串(<> 形式剥掉括号)
    bool bracketed = false;   // @<...> 形式(允许空白)
};

// 找光标(cursor,码点下标)处正在输入的 @ 词元:从 cursor 往回找,前面
// 必须是行首或空白('@' 只有词首才当提及);query 取到行尾或第一个空白
// (bracketed 时到 '>' 或行尾)。不是提及给 nullopt。
std::optional<MentionToken> FindMentionToken(const std::u32string& line, std::size_t cursor);

// 模糊匹配:子序列匹配(大小写不敏感,ASCII 折小写;中文原样),得分按
// 连续命中长度/路径段边界加权,同分按字典序。query 空(只敲了 '@')给
// 根层条目优先。截 limit 条。
std::vector<std::size_t> FuzzyMatchMentions(const std::vector<FileMentionEntry>& entries,
                                            const std::string& query, std::size_t limit = 8);

// 菜单行:首行键提示,随后每条 "❯/␣␣ [类型图标] 相对路径"。selected 越界
// 无选中标记。宽度只做截断(TruncateUtf8ToDisplayWidth)。
std::vector<std::string> BuildMentionMenuLines(const std::vector<FileMentionEntry>& entries,
                                               const std::vector<std::size_t>& matches, int selected,
                                               int width);

// 选中条目要插进编辑行的字符串:路径无空白 → "@rel/path";有空白/角括号
// → "@<rel/path>";目录带尾斜杠。末尾不带空格(调用方自己补)。
std::string MentionInsertionString(const FileMentionEntry& entry);

// 把编辑行里 [start,end) 的词元换成 insertion,返回新行(纯函数,光标
// 位置由调用方按返回串自算——插完光标落词元尾)。
std::u32string ReplaceMentionToken(const std::u32string& line, const MentionToken& token,
                                   const std::string& insertion);

// 提交前校验用的提及抽取:整段已提交文本(多行拼 '\n' 的 UTF-8)里全部
// 文本提及词元(@词/@@ 无;图片路径不在内——由调用方拿图片扩展名分流)。
// 返回去重后的词元串(去掉 '@' 与 <>)。
std::vector<std::string> ExtractTextMentions(std::string_view submitted);

}  // namespace lubancode::cli
