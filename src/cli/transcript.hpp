// UI-B(0.12.0):工具调用条目化渲染的数据模型 + 纯渲染函数。
//
// 一次工具调用在屏幕上是一个"条目":
//   ● run_command(git log --oneline -3)
//     ⎿ Done · 退出码 0 · 1.2s
// 首行是状态灯 + 工具名(关键参数摘要),次行起缩进两空格、⎿ 开头是结果
// 摘要,续行再缩两空格。工具启动先画"执行中"态,结束后由 cli 层原地改写成
// 终态(成功/失败/拒绝/打断)——改写本身是 Win32 控制台的活(main.cpp 的
// TranscriptPainter),这个文件只管两件事:
//   1. TranscriptItem:一个条目的全部数据(UI-C/D 的 Ctrl+E 全文查看、
//      会话回放都靠它,full_output 现在就存好,渲染只用摘要);
//   2. FormatTranscriptItem 及一串摘要生成小函数:纯函数,不碰 IO,单测
//      主战场。
//
// plain 主题(theme.reset 为空)下状态灯换成文字 [RUNNING]/[OK]/[ERROR]/
// [CANCELLED]/[INTERRUPTED](待确认态是 [CONFIRM]),不靠颜色也能辨。
// 彩色主题只染状态灯,正文参数不染色。

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

// 条目状态。Pending 是"needs_confirm 的工具画出来了、还在等用户点头"那个
// 档;Running 起是正经五态(执行中/成功/失败/拒绝/ESC 打断)。
enum class TranscriptStatus { Pending, Running, Ok, Error, Cancelled, Interrupted };

// 主循环的工具是 Tool,子代理内层的工具是 SubTool(渲染时整体再缩四空格)。
// Thinking 是模型思考过程折叠块("思考 Xs",Ctrl+O 展开看正文)。
enum class TranscriptKind { Tool, SubTool, Thinking };

// full_output 的截断上限:64KB。UI-C/D 的 Ctrl+E 全文查看用,别让一次
// 超大输出把会话内存吃穿。
inline constexpr std::size_t kFullOutputCapBytes = 64 * 1024;

struct TranscriptItem {
    int id = 0;
    TranscriptKind kind = TranscriptKind::Tool;
    std::string tool_name;                    // 原始工具名(run_command / mcp__x__y ……)
    std::string title;                        // 首行正文:工具名(关键参数摘要),不含状态灯
    std::string input_json;                   // 完整入参紧凑 JSON(UI-D 展开版/聚焦查看用;空 = 没有入参)
    std::vector<std::string> summary_lines;   // ⎿ 之后那几行
    std::string full_output;                  // 完整结果,截 kFullOutputCapBytes
    TranscriptStatus status = TranscriptStatus::Running;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
};

// 把一个条目渲染成完整的多行文本(每行以 \n 收尾)。width 是终端宽度,
// 首行超宽按显示宽度截断加 "..."(绝不物理折行——折行会毁掉原地改写的
// 行数记账);width <= 0 表示不截断。摘要行不截(可能夹着 todo 清单自带
// 的颜色序列,按显示宽截断会把 ANSI 剪碎)。
//
// UI-D(0.16.0)两个新开关:
//   expanded —— 详细版:摘要之后追加完整入参 JSON 一行("参数: {...}"),
//     再接 full_output 全文(含 UI-C 并进去的完整 diff),一行分隔标题
//     "── 完整输出(N 行)──"、正文每行缩进;full_output 为空补一行
//     "(无完整输出)"。width > 0 时这些行同样按显示宽截断(Ctrl+O 的整块
//     重打走 TranscriptPainter 之外的裸打印也不许物理折行,免得铺屏乱套);
//     width <= 0 不截(Ctrl+E 聚焦查看给 0,全文如实铺,终端自然折行/滚动)。
//     夹 ANSI 的行照旧不截。
//   focused —— 焦点条目:首行行首加 "► " 醒目标记(占两列,宽度记账让位)。
std::string FormatTranscriptItem(const TranscriptItem& item, const Theme& theme, int width,
                                  bool expanded = false, bool focused = false);

// Ctrl+O 整组重打用。详细档逐条铺出(含子代理内层工具)，紧凑档只留
// 顶层工具；focus_index 是原 items 下标，-1 表示不标焦点；expanded_index
// 是单独展开的那一条下标(-1 = 无),该条 expanded=true、其余照 expanded——
// Ctrl+O 用它只展开最近一条,不再全局全展开。
std::string FormatTranscriptItems(const std::vector<TranscriptItem>& items, const Theme& theme,
                                  int width, bool expanded, int focus_index = -1,
                                  int expanded_index = -1);

// plain 主题下的状态文字([RUNNING]/[OK]/…),渲染和单测共用一份映射。
std::string TranscriptStatusWord(TranscriptStatus status);

// ---- 首行参数摘要 ------------------------------------------------------

// 按工具名挑关键参数拼首行:run_command 显示命令;read/write/edit 显示
// 路径;agent 显示任务前 40 个码点;web_search 显示查询词;todo_write
// 显示几项;MCP(mcp__ 前缀)和其余工具显示入参紧凑 JSON。只拼 "name(摘要)",宽度截断交给
// FormatTranscriptItem。
std::string BuildToolTitle(const std::string& name, const nlohmann::json& input);

// /resume 的历史重放。用户/助手正文按当前主题渲染，tool_use 与随后
// tool_result 配成终态工具条目；只含工具结果的 user 消息不另画一轮用户。
std::string FormatRestoredHistory(const std::vector<api::Message>& messages, const Theme& theme,
                                  int width, const std::vector<std::size_t>& compact_positions = {});

// ---- 结果摘要小函数(每个都可单测) ------------------------------------

// 数一段文本有几行:空串 0 行;末尾没有 \n 的最后一截也算一行。
int CountLines(const std::string& text);

// run_command 的结果开头是 "[退出码 N]\n",解析出 N;对不上格式给 nullopt。
std::optional<int> ParseRunCommandExitCode(const std::string& content);

// "Done · 退出码 0 · 1.2s"。耗时由 cli 层在 on_tool_start/on_tool_done
// 之间掐出来传进(秒),退出码从结果文本解析,解析不出就省掉那一节。
std::string RunCommandDoneSummary(const std::string& content, double seconds);

// "3.2s"。思考折叠块标题用,跟工具摘要的耗时格式一致。
std::string FormatSeconds(double seconds);

// "读取 N 行"。
std::string ReadFileDoneSummary(const std::string& content);

// "新增 N 行,删除 M 行"。removed_lines 为空(写的是新文件)时只有
// "新增 N 行"。
std::string WriteDiffSummary(int added_lines, std::optional<int> removed_lines);

// "命中 N 处"。"没搜到匹配的内容"/"没找到匹配的文件" 算 0;截断提示行
// (……开头)不计入。
std::string SearchDoneSummary(const std::string& content);

// "子代理 N 轮 · M 次工具"。
std::string AgentDoneSummary(int rounds, int sub_tool_calls);

// 失败态摘要:首行固定 "Error: <首行>",接错误输出的后续行,总共最多取
// 前 5 行;更长的补一行截断标注 "(共 N 行,Ctrl+E 查看完整)"。
// run_command 的失败结果([退出码 N] 开头)首行改写成 "Error: 退出码 N"。
std::vector<std::string> ErrorSummaryLines(const std::string& tool_name, const std::string& content);

// UTF-8 安全截断到 max_bytes 字节以内(不劈开多字节字符),full_output
// 入库前过一遍。
std::string TruncateUtf8Bytes(const std::string& text, std::size_t max_bytes);

// UTF-8 按码点截断,超长加 "..."(agent 任务摘要"前 40 字"用)。
std::string TruncateUtf8Codepoints(const std::string& text, std::size_t max_codepoints);

}  // namespace lubancode::cli
