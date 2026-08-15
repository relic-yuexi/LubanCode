// 一轮 Run() 里工具调用的展示总管:回调层只管把事件转进来,这里统一负责
//   - 建/更新 TranscriptItem(真控制台走 TranscriptPainter 画条目、原地
//     改写状态;管道/重定向保持稳定纯文本);
//   - edit_file/write_file 确认前的统一 diff 预览(FileDiffPreview);
//   - 计时、行统计摘要全在这一层做,tools/ 层一个字不动。
// 依赖 cli/ 与 tools/,不知道 RunTurn / InteractiveLoop 的存在。

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/console_input.hpp"
#include "cli/diff.hpp"
#include "cli/i18n.hpp"
#include "cli/live_transcript.hpp"
#include "cli/theme.hpp"
#include "cli/todo_render.hpp"
#include "cli/transcript.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool.hpp"

namespace lubancode::cli {

// 统计一个磁盘文件现在有多少行(write_file 覆盖前掐一下旧行数,给 +N -M
// 摘要用)。读不到(不存在/是目录/打不开)给 nullopt——"新文件"场景。
inline std::optional<int> FileLineCount(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return lubancode::cli::CountLines(content);
}

// UI-C(0.13.0):读文件全文(二进制读入,当 UTF-8 字节串用),给 diff
// 预览当"旧内容"。读不到(不存在/是目录/打不开)给 nullopt——write_file
// 按新文件处理(全 + 新增),edit_file 走回退对比,绝不因此崩。
inline std::optional<std::string> ReadFileBytes(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// UI-C:预览截断双限——超 400 行或 32KiB 就截,截了标注省略行数,完整版
// 存进 TranscriptItem.full_output(那边另有 64KB 的库容上限)。
constexpr int kDiffPreviewMaxLines = 400;
constexpr std::size_t kDiffPreviewMaxBytes = 32 * 1024;

// UI-C:终态条目里留存的 diff 行数上限——git 那种效果,diff 直接挂在
// ⎿ 摘要底下不擦,但条目不能无限铺屏,超了标注省略(Ctrl+E 看全)。
constexpr int kDiffFinalMaxLines = 24;
constexpr std::size_t kDiffFinalMaxBytes = 8 * 1024;

// UI-C:一份拼装好的 diff 预览。colored 是直接可打印的整块(路径行 +
// diff 标题行 + diff 正文,每行缩进四空格、带主题色、按宽/行/字节截断);
// full 是 plain 全量(不截行、不截字节、不带色),终态并进 full_output
// 给 Ctrl+E 聚焦查看看全;final_lines 是终态条目摘要里留存的 diff
// (行数收紧到 kDiffFinalMaxLines,预先按宽截好——夹 ANSI 的行渲染层
// 不再截宽,物理折行会毁掉原地改写的行数记账)。
struct FileDiffPreview {
    std::string colored;
    std::string full;
    std::vector<std::string> final_lines;
    int line_count = 0;  // colored 的行数,ReserveRows 记账用
};

// 按 edit_file/write_file 的入参拼 diff 预览;别的工具给 nullopt。读旧
// 文件在这一层做(tools/ 层一字不动):edit_file 拿真文件内容做整文替换
// 后对比(变更处自带 ±3 行真实上下文和行号),找不到 old_string 回退成
// 只比 old/new 两段;write_file 旧文件存在做行级对比,不存在全部算新增。
inline std::optional<FileDiffPreview> BuildFileDiffPreview(const std::string& name, const nlohmann::json& input,
                                                     const lubancode::cli::Theme& theme) {
    namespace cli = lubancode::cli;
    if (name != "write_file" && name != "edit_file") {
        return std::nullopt;
    }
    const std::string path = input.value("path", std::string());
    const std::optional<std::string> old_content = ReadFileBytes(path);

    std::vector<cli::DiffLine> diff;
    std::string header;
    if (name == "edit_file") {
        const bool replace_all = input.value("replace_all", false);
        auto edit = cli::BuildEditDiff(old_content.value_or(std::string()), input.value("old_string", std::string()),
                                        input.value("new_string", std::string()), replace_all);
        diff = std::move(edit.lines);
        if (!edit.located) {
            header = tr("diff.not_located");
        } else if (replace_all) {
            header = trf("diff.replace_all", edit.replaced_count);
        } else {
            header = tr("diff.plain");
        }
    } else {
        diff = cli::BuildWriteDiff(old_content, input.value("content", std::string()));
        header = old_content.has_value() ? tr("diff.overwrite") : tr("diff.new_file");
    }

    const int width = cli::DetectConsoleWidth().value_or(80);
    // 每行缩四空格,diff 行本身的宽度上限就得让出这四列(再留一列,免得
    // 顶格写到最后一格触发控制台自动换行、毁掉行数记账)。
    const std::string body =
        cli::FormatDiff(diff, theme, width - 5, kDiffPreviewMaxLines, kDiffPreviewMaxBytes, path);

    FileDiffPreview out;
    out.full = header + "\n" +
               cli::FormatDiff(diff, cli::BuiltinTheme("plain"), /*width=*/0, /*max_lines=*/0, /*max_bytes=*/0);

    // 终态条目里留存的那份:行数收紧,宽度给 ⎿ 前缀让出十列(子代理条目
    // 再缩四格也够用)。
    {
        const std::string final_body =
            cli::FormatDiff(diff, theme, width - 10, kDiffFinalMaxLines, kDiffFinalMaxBytes, path);
        std::size_t p = 0;
        while (p < final_body.size()) {
            std::size_t nl = final_body.find('\n', p);
            if (nl == std::string::npos) {
                nl = final_body.size();
            }
            out.final_lines.push_back(final_body.substr(p, nl - p));
            p = nl + 1;
        }
    }

    const std::string block = trf("diff.path", path) + "\n" + header + "\n" + body;
    std::string indented;
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t nl = block.find('\n', pos);
        if (nl == std::string::npos) {
            nl = block.size();
        }
        indented += "    " + block.substr(pos, nl - pos) + "\n";
        pos = nl + 1;
    }
    out.colored = std::move(indented);
    out.line_count = cli::CountLines(out.colored);
    return out;
}

// UI-B(0.12.0):一轮 Run() 里工具调用的展示总管。回调层(BuildCallbacks)
// 只管把事件转进来,这里统一负责:
//   - 建/更新 TranscriptItem(会话级 transcript vector 持有,UI-C/D 的
//     Ctrl+E 全文查看、回放都要用,full_output 现在就存好);
//   - 真控制台:走 TranscriptPainter 画条目、原地改写状态;
//   - 管道/重定向:保持稳定纯文本——启动一行 "[工具] name {...}"(现状),
//     结束一行 "[工具完成] name: 摘要"(新增,不回写、不夹 ANSI)。
// 计时(run_command 耗时)、行统计(write/edit 的 "新增 N 行,删除 M 行")
// 全在这一层做,tools/ 层一个字不动。
struct ToolDisplay {
    // silent(查看态回流单):静默收货档——条目照建、TranscriptItem 照进台账、
    // 线程安全快照照更,但一个字节都不往终端写(painter 关死、管道模式那几
    // 行稳定纯文本也不打、diff 预览不铺)。给"用户正看别的子代理、main 在
    // 后台消化结果"的那一轮用;回 main 时台账重铺,条目全在。
    ToolDisplay(std::vector<lubancode::cli::TranscriptItem>& transcript_ref, const lubancode::cli::Theme& theme_ref,
                bool console, std::shared_ptr<lubancode::tools::TodoListState> todo,
                const std::atomic<bool>* cancel, const std::atomic<bool>* expanded = nullptr,
                bool silent = false)
        : transcript(transcript_ref),
          theme(theme_ref),
          is_console(console),
          painter(theme_ref, console && !silent, expanded),
          todo_state(std::move(todo)),
          cancel_flag(cancel),
          expanded_(expanded),
          silent_(silent),
          transcript_snapshot_(transcript_ref) {}

    std::vector<lubancode::cli::TranscriptItem>& transcript;
    const lubancode::cli::Theme& theme;
    bool is_console;
    TranscriptPainter painter;
    std::shared_ptr<lubancode::tools::TodoListState> todo_state;
    const std::atomic<bool>* cancel_flag = nullptr;
    bool silent_ = false;
    // UI-D 折叠(#三):同一份 Ctrl+O 紧凑/详细全局开关(TranscriptPainter
    // 构造函数第三个参数那份,这里再存一份指针给 OnSubToolStart/Result/
    // Blocked 判断要不要把子代理内层工具条目画到屏幕上——紧凑态(默认)
    // 紧凑态不逐条铺屏;
    // TranscriptItem 本身照旧记(NewItem/FinalizeItem 不受这个开关影响),
    // 明细留在 transcript 数组里,session 落盘/Ctrl+E 聚焦查看/切到详细态
    // 都还能看见,只是紧凑态默认不画。atomic<bool>:回合执行期间会被
    // TurnInputListener 的监听线程跨线程翻转,见 TranscriptPainter 构造
    // 函数注释里记的那次真机实测教训。
    const std::atomic<bool>* expanded_ = nullptr;

    // TurnInputListener 在另一线程响应 Ctrl+O。它不能直接读主线程正在改的
    // transcript；这里留一份只在事件收账后更新的快照，锁内只做单项复制。
    mutable std::mutex transcript_snapshot_mutex_;
    std::vector<lubancode::cli::TranscriptItem> transcript_snapshot_;

    // #52 起家的子代理状态条已删:前台/后台子代理的状态、工具次数、token、
    // 耗时全在 AgentTool 的统一台账(TaskRecord/TaskSummaries)里,由代理
    // 面板(空闲 composer 上方 + 流式 footer)一处画,ToolDisplay 不再自
    // 己另记一本账。

    // 主工具、子代理内层工具都是严格串行的,各留一个"进行中"槽位就够
    // (agent 工具执行期间 active_main 指着 agent 条目,active_sub 指着
    // 它肚子里正在跑的那个)。
    int active_main = -1;  // transcript 下标,-1 = 没有进行中的主工具
    int active_sub = -1;
    // todo_write 是一块“当前计划”，不是流水账。同一轮后续调用复用这
    // 个 transcript 条目和屏幕锚点；工具消息本身仍由 AgentLoop 完整保存。
    int reusable_todo_item = -1;
    nlohmann::json main_input;
    nlohmann::json sub_input;
    std::optional<int> main_write_old_lines;
    std::optional<int> sub_write_old_lines;
    int agent_step_count = 0;  // 子代理累计步数(每次模型请求一步)
    int agent_sub_tools = 0;
    // 思考折叠块("思考 Xs")。active_thinking 是 transcript 下标,
    // -1 = 没有正在展示的思考块。thinking_buffer 攒完整思考正文,结束时
    // 灌进 full_output 供 Ctrl+O 展开。
    int active_thinking = -1;
    std::string thinking_buffer;
    // UI-C:确认前 diff 预览的记账。*_diff_full 存 plain 全量,终态并进
    // full_output;*_diff_final 是终态条目摘要里留存的 diff 行(git 那种
    // 效果,成功后 diff 挂在 ⎿ 块底下不消失);*_preview_below 标记"自动
    // 放行路子里预览还垫在条目下面",工具执行完 TrimBelow 擦掉(确认路子
    // 的预览由 OnConfirmAnswered 的 TrimBelow 顺手带走,不用这个标记)。
    std::string main_diff_full;
    std::string sub_diff_full;
    std::vector<std::string> main_diff_final;
    std::vector<std::string> sub_diff_final;
    bool main_preview_below = false;
    bool sub_preview_below = false;

    void OnToolStart(const std::string& name, const nlohmann::json& input) {
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        if (!is_console && !silent_) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << "\n" << theme.tool_line << tr("pipe.tool_start") << name << " " << input.dump() << theme.reset
                      << "\n";
            std::cout.flush();
        }
        main_input = input;
        main_write_old_lines =
            name == "write_file" ? FileLineCount(input.value("path", std::string())) : std::nullopt;
        main_diff_full.clear();
        main_diff_final.clear();
        main_preview_below = false;
        if (name == "agent") {
            agent_step_count = 0;
            agent_sub_tools = 0;
        }
        const bool is_todo = name == "todo_write" && todo_state;
        std::size_t todo_item_count = 0;
        if (is_todo) {
            if (const auto it = input.find("items"); it != input.end() && it->is_array()) {
                todo_item_count = it->size();
            }
        }
        // 中间夹着别的工具输出时，计划块不能凭空长高，否则会盖住下面
        // 的条目。项数不变才原位换状态；计划增删项时另起一块 update。
        const bool can_reuse_todo =
            is_todo && reusable_todo_item >= 0 &&
            transcript[static_cast<std::size_t>(reusable_todo_item)].summary_lines.size() ==
                (std::max<std::size_t>)(1, todo_item_count);
        if (can_reuse_todo) {
            active_main = reusable_todo_item;
            auto& item = transcript[static_cast<std::size_t>(active_main)];
            // 旧计划下面可能早已垫着别的工具输出。此时若先把 N 行清单
            // 缩成一行 Running，完成时便不能向下长回去，会盖住后文。
            // 保住原摘要的行数，只换标题、圆点和状态色；结果回来后再
            // 在同样高的块里替换各项状态。
            const std::vector<std::string> previous_summary = item.summary_lines;
            item.tool_name = name;
            item.title = lubancode::cli::BuildToolTitle(todo_state->revision > 0 ? "todo_update" : name, input);
            item.input_json = input.is_null() ? std::string() : input.dump();
            item.status = lubancode::cli::TranscriptStatus::Running;
            item.summary_lines = previous_summary;
            item.full_output.clear();
            item.start_time = std::chrono::steady_clock::now();
            item.end_time = {};
            UpdateSnapshotItem(active_main);
            if (is_console) {
                if (painter.HasAnchor(item.id)) {
                    painter.Repaint(item);
                } else {
                    painter.PaintNew(item);
                }
            }
        } else {
            active_main = NewItem(lubancode::cli::TranscriptKind::Tool, name, input);
            auto& item = transcript[static_cast<std::size_t>(active_main)];
            if (is_todo) {
                reusable_todo_item = active_main;
                if (todo_state->revision > 0) {
                    item.title = lubancode::cli::BuildToolTitle("todo_update", input);
                }
            }
            UpdateSnapshotItem(active_main);
            if (is_console) {
                painter.PaintNew(item);
            }
        }
    }

    // hooks 框架第三步:工具状态机的两个新过渡态,按当前主/子条目路由
    // (子代理工具执行期 active_sub 活着,自动落到子条目;主代理工具落主
    // 条目)。CheckingHook 把那行 "Running..." 换成钩子检查中(单行换单行,
    // 锚点行数不乱);Blocked 标记"被钩子拦下,没有执行"——终态渲染据此
    // 区分"拦下"与"跑过又失败",不冒充。没配 hooks 的会话不会走到这里。
    void OnHookCheckingText() {
        const int idx = active_sub >= 0 ? active_sub : active_main;
        if (idx < 0) {
            return;
        }
        auto& item = transcript[static_cast<std::size_t>(idx)];
        if (item.status == lubancode::cli::TranscriptStatus::Blocked ||
            item.status == lubancode::cli::TranscriptStatus::Cancelled) {
            return;  // 已定格,不再被后续相位盖掉
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        item.summary_lines = {tr("transcript.checking_hook")};
        UpdateSnapshotItem(idx);
        if (is_console && (idx != active_sub || SubItemsExpanded())) {
            // 子条目紧凑态没画过(无锚点),Repaint 会掉进兜底追加分支——
            // 与 OnSubToolResult 同一条规矩:只有画过的才原地重画。
            painter.Repaint(item);
        }
    }

    void OnHookMarkBlocked() {
        const bool sub = active_sub >= 0;
        const int idx = sub ? active_sub : active_main;
        if (idx < 0) {
            return;
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        auto& item = transcript[static_cast<std::size_t>(idx)];
        item.status = lubancode::cli::TranscriptStatus::Blocked;
        item.summary_lines = {tr("transcript.hook_blocked")};
        item.end_time = std::chrono::steady_clock::now();
        UpdateSnapshotItem(idx);
        if (is_console) {
            if (!sub || SubItemsExpanded()) {
                painter.Repaint(item);
            }
            if (sub) {
                RetractIfCompact(item);
                active_sub = -1;  // 拦下的子工具不会再有 post 钩子,槽位在这儿收掉
            }
        } else if (sub) {
            active_sub = -1;
        }
    }

    void OnToolDone(const std::string& name, const lubancode::tools::Tool::Result& result) {
        if (active_main < 0) {
            return;
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        auto& item = transcript[static_cast<std::size_t>(active_main)];
        // UI-C:自动放行时垫在条目下面的 diff 预览,执行完了就擦——终态只
        // 留 "新增 N 行,删除 M 行" 简短摘要,不铺屏(确认路子的预览已被
        // OnConfirmAnswered 的 TrimBelow 带走,标记不会是 true)。
        if (main_preview_below && is_console) {
            painter.TrimBelow(item.id);
        }
        main_preview_below = false;
        FinalizeItem(item, name, main_input, result, main_write_old_lines, agent_step_count, agent_sub_tools,
                      main_diff_full, main_diff_final);
        UpdateSnapshotItem(active_main);
        if (is_console) {
            painter.Repaint(item);
        } else if (!silent_) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << tr("pipe.tool_done") << name << ": " << PipeSummary(item, name) << "\n";
            // 管道模式沿用 M11 的行为:todo_write 成功后紧跟着把清单打出来,
            // 重定向日志里"计划走到哪一步了"仍然可读。
            if (name == "todo_write" && !result.is_error && todo_state) {
                std::cout << lubancode::cli::FormatTodoList(todo_state->items, theme);
            }
            std::cout.flush();
        }
        active_main = -1;
    }

    void OnBuiltinToolDone(const std::string& name, const nlohmann::json& final_input,
                           const lubancode::tools::Tool::Result& result) {
        // Responses 兼容端常在 output_item.added 只给空 action，到 done
        // 才补 query。用终态参数回填同一条记录，免得绿灯后标题仍是 {}。
        if (active_main >= 0 && final_input.is_object() && !final_input.empty()) {
            main_input = final_input;
            auto& item = transcript[static_cast<std::size_t>(active_main)];
            item.title = lubancode::cli::BuildToolTitle(name, final_input);
            item.input_json = final_input.dump();
        }
        OnToolDone(name, result);
    }

    // ---- 思考折叠块 -------------------------------------------------------
    // 首 delta 到来:建一条 Thinking 条目(title="思考中…", Running),PaintNew
    // 画出来。后续 delta 攒进 thinking_buffer,不逐条刷屏(思考正文本身不往
    // 屏幕上铺,只等结束时把耗时和时间放进标题)。body_tracker 的断开由
    // BuildCallbacks 在调本方法之前做。
    void OnThinkingDelta(const std::string& text) {
        if (active_thinking < 0) {
            thinking_buffer.clear();
            lubancode::cli::TranscriptItem item;
            item.id = static_cast<int>(transcript.size()) + 1;
            item.kind = lubancode::cli::TranscriptKind::Thinking;
            item.tool_name = "thinking";
            item.title = lubancode::cli::tr("transcript.thinking_running");
            item.status = lubancode::cli::TranscriptStatus::Running;
            item.start_time = std::chrono::steady_clock::now();
            transcript.push_back(std::move(item));
            active_thinking = static_cast<int>(transcript.size()) - 1;
            UpdateSnapshotItem(active_thinking);
            if (is_console) {
                painter.PaintNew(transcript[static_cast<std::size_t>(active_thinking)]);
            } else if (!silent_) {
                std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
                std::cout << "\n" << theme.tool_line
                          << lubancode::cli::tr("transcript.thinking_running") << theme.reset << "\n";
                std::cout.flush();
            }
        }
        thinking_buffer += text;
        // 截断保护:别让超长思考把内存吃穿。
        if (thinking_buffer.size() > lubancode::cli::kFullOutputCapBytes) {
            thinking_buffer = lubancode::cli::TruncateUtf8Bytes(thinking_buffer, lubancode::cli::kFullOutputCapBytes);
        }
        // 思考进行中 Ctrl+O 展开:已到的正文回填进 transcript_snapshot_ 这条
        // 既有数据通道(锁内复制一份 full_output 副本),监听线程的
        // FormatSnapshotForToggleLocked 原样读走——不开第二条数据路,也不让
        // 它无锁直读 thinking_buffer。每笔 delta 都同步:buffer 有 64KB 上限,
        // 单笔复制量封顶,流式期间这点拷贝不值一提。屏幕上那块折叠头不跟着
        // 刷新,保持"不打断流式";展开态下看到的是按 Ctrl+O 那一刻的快门,
        // 思考收定时 OnThinkingDone 的 Repaint 会按展开档铺全文。
        {
            std::lock_guard<std::mutex> lock(transcript_snapshot_mutex_);
            if (transcript_snapshot_.size() > static_cast<std::size_t>(active_thinking)) {
                transcript_snapshot_[static_cast<std::size_t>(active_thinking)].full_output = thinking_buffer;
            }
        }
    }

    // 思考结束:算耗时,标题换成 "思考 Xs",full_output 灌入完整正文,Repaint。
    // 幂等:没开着思考块时直接返回。首个 TextDelta / 工具开始 / on_usage
    // 都会调它。body_tracker 的分隔由 BuildCallbacks 在调本方法之后做。
    void OnThinkingDone() {
        if (active_thinking < 0) {
            return;
        }
        const int idx = active_thinking;
        active_thinking = -1;
        auto& item = transcript[static_cast<std::size_t>(idx)];
        item.end_time = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(item.end_time - item.start_time).count();
        item.title = lubancode::cli::trf("transcript.thinking_done", lubancode::cli::FormatSeconds(seconds));
        item.full_output = std::move(thinking_buffer);
        thinking_buffer.clear();
        item.status = lubancode::cli::TranscriptStatus::Ok;
        UpdateSnapshotItem(idx);
        if (is_console) {
            painter.Repaint(item);
        }
    }

    bool HasActiveThinking() const { return active_thinking >= 0; }

    void OnSubToolStart(const std::string& name, const nlohmann::json& input) {
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        agent_sub_tools += 1;
        if (!is_console && !silent_) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << "\n"
                       << theme.stats << tr("pipe.subtool_start") << name << " " << input.dump() << theme.reset
                       << "\n";
            std::cout.flush();
        }
        sub_input = input;
        sub_write_old_lines =
            name == "write_file" ? FileLineCount(input.value("path", std::string())) : std::nullopt;
        sub_diff_full.clear();
        sub_diff_final.clear();
        sub_preview_below = false;
        active_sub = NewItem(lubancode::cli::TranscriptKind::SubTool, name, input);
        // UI-D 折叠(#三,修订):紧凑态(默认)下,子工具"开始执行"这一刻
        // 就不画明细了——之前只在收尾(OnSubToolResult/OnSubBlocked)的
        // Retract 才把它收走,真机 dogfood 发现子工具执行慢时(比如
        // web_fetch 卡几十秒)这段"运行中"的明细会跟代理面板的
        // 摘要行同屏铺一起,没达到"紧凑态从不铺明细"的预期。改成
        // 开头就按 SubItemsExpanded() 判断要不要画——只有详细态才画执行中
        // 态、登记锚点(确认流程 needs_confirm 的 y/a/N、UI-C 的 diff 预览/
        // TrimBelow 都靠这份锚点定位);紧凑态压根不画,自然也没有锚点可收。
        // 注:needs_confirm 的子工具即便紧凑态也会在 OnConfirmRequest 那步
        // 强制可见(Repaint 找不到锚点会退化成追加打印,这是既有设计——
        // 用户必须看见需要 y/a/N 的交互,不受这条折叠规则约束)。
        // TranscriptItem 本身不受这个开关影响,照旧记在 transcript 数组里,
        // session 落盘/Ctrl+E 聚焦查看/切到详细态都还能看见。
        if (is_console && SubItemsExpanded()) {
            painter.PaintNew(transcript[static_cast<std::size_t>(active_sub)]);
        }
    }

    // 子工具真执行完(agent 工具转发的 post_tool 钩子)——终态回写。拒绝
    // 那条路走不到这里(post_tool 只在真执行后触发),由 OnConfirmAnswered
    // 定格成 Cancelled。
    void OnSubToolResult(const std::string& name, const nlohmann::json& input,
                          const lubancode::tools::Tool::Result& result) {
        (void)input;
        if (active_sub < 0) {
            return;
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        auto& item = transcript[static_cast<std::size_t>(active_sub)];
        if (sub_preview_below && is_console) {
            painter.TrimBelow(item.id);  // 理由同 OnToolDone
        }
        sub_preview_below = false;
        FinalizeItem(item, name, sub_input, result, sub_write_old_lines, 0, 0, sub_diff_full, sub_diff_final);
        UpdateSnapshotItem(active_sub);
        if (is_console) {
            // 紧凑态下 OnSubToolStart 压根没画(没有 PaintNew、没有锚点)——
            // 这里若无脑调 Repaint,会撞进 TranscriptPainter::Repaint 的
            // "锚点没登记成"兜底分支(见该函数注释),照样把整条明细追加
            // 打印出来,等于白折叠。只有详细态(SubItemsExpanded()为真,
            // 开头确实画过、锚点确实登记了)才 Repaint;紧凑态直接跳过。
            // RetractIfCompact 本身对"从没画过"的条目是安全空操作
            // (Retract 内部 Find 不到锚点直接 return),两态都调不会出错。
            if (SubItemsExpanded()) {
                painter.Repaint(item);
            }
            RetractIfCompact(item);  // UI-D 折叠(#三):见函数注释
        }
        active_sub = -1;
    }

    // 子工具被 pre_tool 钩子拦截(post_tool 不会再来了)——条目定格成失败态。
    void OnSubBlocked(const std::string& message) {
        if (active_sub < 0) {
            return;
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        auto& item = transcript[static_cast<std::size_t>(active_sub)];
        item.status = lubancode::cli::TranscriptStatus::Error;
        item.summary_lines = lubancode::cli::ErrorSummaryLines(item.tool_name, message);
        item.full_output = lubancode::cli::TruncateUtf8Bytes(message, lubancode::cli::kFullOutputCapBytes);
        item.end_time = std::chrono::steady_clock::now();
        UpdateSnapshotItem(active_sub);
        if (is_console) {
            // 理由同 OnSubToolResult:紧凑态下开头没画、没锚点,Repaint 会
            // 掉进兜底追加打印分支,得跳过。
            if (SubItemsExpanded()) {
                painter.Repaint(item);
            }
            RetractIfCompact(item);  // UI-D 折叠(#三):见函数注释
        }
        active_sub = -1;
    }

    // UI-C:edit_file/write_file 确认前的统一 diff 预览,画在当前条目
    // (子工具优先)下面。trim_on_done=true 是自动放行那条路(auto/yolo/
    // --yes/选过 a)——打完不等确认,工具执行完 OnToolDone/OnSubToolResult
    // 里 TrimBelow 擦掉;false 是确认路子,预览随确认块一起被
    // OnConfirmAnswered 的 TrimBelow 带走。管道模式(is_console 为假)
    // 整个不打,保持稳定纯文本输出。
    void ShowDiffPreview(const std::string& name, const nlohmann::json& input, bool trim_on_done) {
        if (!is_console || silent_) {
            return;  // 管道模式保持稳定纯文本;静默档不铺预览(数据进 full_output 不丢)
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint;
        const auto preview = BuildFileDiffPreview(name, input, theme);
        if (!preview.has_value()) {
            return;
        }
        const bool sub = active_sub >= 0;
        (sub ? sub_diff_full : main_diff_full) = preview->full;
        (sub ? sub_diff_final : main_diff_final) = preview->final_lines;
        // 预览可能有几百行,先在缓冲区底部把行数留够(外加确认块的余量),
        // 免得打印期间自然滚屏把锚点推歪。ReserveRows 自己拿 stdout 锁,
        // 不能包在下面那把锁里(std::mutex 不可重入)。
        painter.ReserveRows(preview->line_count + 24);
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << preview->colored;
            std::cout.flush();
        }
        if (trim_on_done) {
            (sub ? sub_preview_below : main_preview_below) = true;
        }
    }

    // 确认真的要问出口了(自动放行的几条路都没走到)——条目改成"待确认"态,
    // 确认块(参数详情 + [y/a/N])跟在条目下面打。返回该条目的 transcript
    // 下标,答完交回 OnConfirmAnswered。
    int OnConfirmRequest() {
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        const int idx = active_sub >= 0 ? active_sub : active_main;
        if (idx >= 0) {
            auto& item = transcript[static_cast<std::size_t>(idx)];
            item.status = lubancode::cli::TranscriptStatus::Pending;
            item.summary_lines = {tr("transcript.pending")};
            UpdateSnapshotItem(idx);
            if (is_console) {
                painter.Repaint(item);
                // 确认块 + 编辑器提示行撑死二十来行,先在缓冲区底部预留好,
                // 免得交互期间自然滚屏把锚点推歪。
                painter.ReserveRows(24);
            }
        }
        return idx;
    }

    void OnConfirmAnswered(int idx, bool allowed) {
        if (idx < 0) {
            return;
        }
        const lubancode::cli::StreamFooterPaintScope footer_paint(is_console);
        auto& item = transcript[static_cast<std::size_t>(idx)];
        const bool was_sub = idx == active_sub;  // Cancelled 分支下面会把 active_sub 收掉,先记住
        if (is_console) {
            painter.TrimBelow(item.id);  // 确认块用完就擦,条目回到屏幕末尾
        }
        if (allowed) {
            item.status = lubancode::cli::TranscriptStatus::Running;
            item.summary_lines = {"Running..."};
        } else {
            item.status = lubancode::cli::TranscriptStatus::Cancelled;
            item.summary_lines = {"Cancelled"};
            if (idx == active_sub) {
                active_sub = -1;  // 子工具拒绝后 post_tool 钩子不会再来,槽位在这儿收掉
            }
        }
        UpdateSnapshotItem(idx);
        if (is_console) {
            painter.Repaint(item);
            // 拒绝的子工具:post_tool 钩子不会再来,OnSubToolResult 那条收尾
            // 路线走不到——终态就定在这儿,UI-D 折叠(#三)的收走也得在这儿
            // 补一刀,不然紧凑态会留一条"Cancelled"摊在屏幕上出不去。
            // allowed=true 是过渡态(Running),真正的终态收走留给
            // OnSubToolResult。
            if (!allowed && was_sub) {
                RetractIfCompact(item);
            }
        }
    }

private:
    // UI-D 折叠(#三):子代理内层工具条目落定终态那一刻,紧凑态(默认)
    // 下把刚画出来的那几行从屏幕上收走——代理面板已经有一行
    // running→done 摘要覆盖同样的信息,没必要逐条摊开占屏幕。TranscriptItem
    // 本身(连同刚落定的终态摘要)照旧留在 transcript 数组里,session 落盘/
    // Ctrl+E 聚焦查看/Ctrl+O 切到详细态都还能看见,只是紧凑态默认不铺屏。
    // 只对子代理内层条目(kind==SubTool)生效,主工具条目(agent 这条本身)
    // 不受影响——主区原地覆写照旧,用户能看见 agent 工具的执行中/完成态。
    void RetractIfCompact(const lubancode::cli::TranscriptItem& item) {
        if (item.kind != lubancode::cli::TranscriptKind::SubTool || SubItemsExpanded()) {
            return;
        }
        painter.Retract(item.id);
    }

    // expanded_ 没设(nullptr,AskOnce 单发模式恒紧凑)或者当前是紧凑态都
    // 算"紧凑",只有用户 Ctrl+O 切到详细态才算"展开"。
    bool SubItemsExpanded() const { return expanded_ != nullptr && *expanded_; }

public:
    // cli 层滚屏钩子调用；彼时 stdout 锁已在外层拿住。
    void OnScreenScrolledLocked(int rows) { painter.OnScreenScrolledLocked(rows); }

    // TurnInputListener 的 Ctrl+O 回调。调用方已持有 stdout 锁、footer 已
    // 擦掉：先收状态块、废掉旧锚点，再从线程安全快照生成真正的详细/紧凑
    // 转录。这里只返回文本，落笔仍由 console_input 统一完成。
    std::string FormatSnapshotForToggleLocked(bool expanded) {
        painter.ForgetAnchorsLocked();

        std::vector<lubancode::cli::TranscriptItem> snapshot;
        {
            std::lock_guard<std::mutex> lock(transcript_snapshot_mutex_);
            snapshot = transcript_snapshot_;
        }
        if (snapshot.empty()) {
            return tr("ui.no_items") + "\n";
        }

        const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
        return lubancode::cli::FormatTranscriptItems(snapshot, theme, width, expanded);
    }

private:

    void UpdateSnapshotItem(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= transcript.size()) {
            return;
        }
        std::lock_guard<std::mutex> lock(transcript_snapshot_mutex_);
        if (transcript_snapshot_.size() < transcript.size()) {
            transcript_snapshot_.resize(transcript.size());
        }
        transcript_snapshot_[static_cast<std::size_t>(index)] = transcript[static_cast<std::size_t>(index)];
    }

    int NewItem(lubancode::cli::TranscriptKind kind, const std::string& name, const nlohmann::json& input) {
        lubancode::cli::TranscriptItem item;
        item.id = static_cast<int>(transcript.size()) + 1;
        item.kind = kind;
        item.tool_name = name;
        item.title = lubancode::cli::BuildToolTitle(name, input);
        // UI-D:完整入参存档,展开版("参数: {...}")和 Ctrl+E 聚焦查看用。
        item.input_json = input.is_null() ? std::string() : input.dump();
        item.status = lubancode::cli::TranscriptStatus::Running;
        item.summary_lines = {"Running..."};
        item.start_time = std::chrono::steady_clock::now();
        transcript.push_back(std::move(item));
        const int index = static_cast<int>(transcript.size()) - 1;
        UpdateSnapshotItem(index);
        return index;
    }

    // 终态归档:状态 + 摘要 + full_output + 计时,一处算完。摘要规则:
    // run_command 退出码+耗时;read_file 行数;write/edit +N -M;search
    // 命中数;agent 子代理轮数/子工具次数;todo_write 接现成清单渲染;
    // MCP 和其余工具取结果第一行。失败态固定 "Error: ..." 开头,拒绝态
    // (确认回调里已定格)不再覆盖,ESC 打断标成 Interrupted。
    void FinalizeItem(lubancode::cli::TranscriptItem& item, const std::string& name, const nlohmann::json& input,
                       const lubancode::tools::Tool::Result& result, std::optional<int> write_old_lines,
                       int step_count, int sub_tools, const std::string& diff_full = std::string(),
                       const std::vector<std::string>& diff_final = {}) {
        namespace cli = lubancode::cli;
        item.end_time = std::chrono::steady_clock::now();
        // UI-C:有 diff 预览的(edit_file/write_file),完整 plain diff 跟着
        // 工具结果一起进 full_output——屏上的预览是要被擦掉的,Ctrl+E
        // 聚焦查看从这儿看全。
        item.full_output = cli::TruncateUtf8Bytes(
            diff_full.empty() ? result.content : result.content + "\n\n" + diff_full, cli::kFullOutputCapBytes);
        const double seconds = std::chrono::duration<double>(item.end_time - item.start_time).count();

        if (item.status == cli::TranscriptStatus::Cancelled) {
            return;  // 确认回调里已经定格成拒绝态,别拿 "用户拒绝执行该工具" 再盖一遍
        }
        if (item.status == cli::TranscriptStatus::Blocked) {
            // hooks:被钩子拦下的条目已定格成"拦下(未执行)"——full_output
            // 补上拦截理由(钩子写进 tool_result 的那段),摘要保持"拦下",
            // 不冒充"运行过又失败"。
            const std::string reason = cli::TruncateUtf8Bytes(result.content, cli::kFullOutputCapBytes);
            item.full_output = cli::TruncateUtf8Bytes(
                diff_full.empty() ? reason : reason + "\n\n" + diff_full, cli::kFullOutputCapBytes);
            return;
        }
        if (result.is_error) {
            if (result.content == "用户拒绝执行该工具") {
                item.status = cli::TranscriptStatus::Cancelled;
                item.summary_lines = {"Cancelled"};
                return;
            }
            item.status = cli::TranscriptStatus::Error;
            item.summary_lines = cli::ErrorSummaryLines(name, result.content);
            return;
        }
        if (cancel_flag != nullptr && cancel_flag->load()) {
            item.status = cli::TranscriptStatus::Interrupted;
            item.summary_lines = {"Interrupted"};
            return;
        }

        item.status = cli::TranscriptStatus::Ok;
        if (name == "run_command") {
            item.summary_lines = {cli::RunCommandDoneSummary(result.content, seconds)};
        } else if (name == "read_file") {
            item.summary_lines = {cli::ReadFileDoneSummary(result.content)};
        } else if (name == "write_file") {
            item.summary_lines = {
                cli::WriteDiffSummary(cli::CountLines(input.value("content", std::string())), write_old_lines)};
            // UI-C:终态把 diff 留在条目里(git 那种效果)——首行
            // "新增 N 行,删除 M 行",底下接 diff 正文(建预览时已按宽截
            // 好、行数收紧,超长有 Ctrl+E 标注)。管道模式没建预览,
            // diff_final 是空的,摘要保持一行文字不变。
            item.summary_lines.insert(item.summary_lines.end(), diff_final.begin(), diff_final.end());
        } else if (name == "edit_file") {
            item.summary_lines = {
                cli::WriteDiffSummary(cli::CountLines(input.value("new_string", std::string())),
                                       cli::CountLines(input.value("old_string", std::string())))};
            item.summary_lines.insert(item.summary_lines.end(), diff_final.begin(), diff_final.end());
        } else if (name == "search") {
            item.summary_lines = {cli::SearchDoneSummary(result.content)};
        } else if (name == "agent") {
            // 后台派出(规格"现场六"):启动卡只写不过期的事实——"后台子代理
            // #N 已启动"。绝不把派出那一刻的 0/0 冒充任务摘要(计数随后台
            // 线程走,主区条目拿不到,两本账当面打架);前台完成的照旧报
            // 步数与工具数。后台启动的 Result 以"后台子代理 #"开头,是
            // agent_tool 定下的回话,这里认这个前缀。
            if (result.content.rfind("后台子代理 #", 0) == 0) {
                item.summary_lines = {result.content.substr(0, result.content.find('\n'))};
            } else {
                item.summary_lines = {cli::AgentDoneSummary(step_count, sub_tools)};
            }
        } else if (name == "todo_write" && todo_state) {
            // 沿用现有清单渲染,清单接在 ⎿ 之后(FormatTodoList 每行自带的
            // 两空格缩进剥掉,条目渲染自己管缩进)。
            item.summary_lines.clear();
            const std::vector<std::size_t> highlights =
                todo_state->last_write_kind == lubancode::tools::TodoWriteKind::Updated
                    ? todo_state->last_changed_indices
                    : std::vector<std::size_t>{};
            const std::string rendered = cli::FormatTodoList(todo_state->items, theme, highlights);
            std::size_t pos = 0;
            while (pos < rendered.size()) {
                std::size_t nl = rendered.find('\n', pos);
                if (nl == std::string::npos) {
                    nl = rendered.size();
                }
                std::string line = rendered.substr(pos, nl - pos);
                if (line.compare(0, 2, "  ") == 0) {
                    line.erase(0, 2);
                }
                if (!line.empty()) {
                    item.summary_lines.push_back(std::move(line));
                }
                pos = nl + 1;
            }
        } else {
            // MCP(mcp__server__tool)和其余工具:结果前一行当摘要。
            std::string first_line = result.content.substr(0, result.content.find('\n'));
            if (first_line.empty()) {
                first_line = "Done";
            }
            item.summary_lines = {std::move(first_line)};
        }
    }

    // 管道模式那行 "[工具完成] name: 摘要" 的摘要——不回写、不夹 ANSI,
    // 取条目摘要第一行;todo_write 的摘要是清单本身,换成一句人话。
    std::string PipeSummary(const lubancode::cli::TranscriptItem& item, const std::string& name) const {
        if (name == "todo_write" && item.status == lubancode::cli::TranscriptStatus::Ok && todo_state) {
            return trf("pipe.todo_updated", todo_state->items.size());
        }
        if (item.summary_lines.empty()) {
            return "Done";
        }
        return item.summary_lines.front();
    }
};

}  // namespace lubancode::cli

