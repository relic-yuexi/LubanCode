// TerminalTurnRenderer 的实现(终端回合视觉收束单第 4 步)。
//
// TurnView -> 行组:条目措辞全走 cli::FormatTranscriptItem(不抄第二遍),
// 这里只管"轮"的骨架——user 条目、step 分组的轻间隔、turn footer。

#include "cli/turn_renderer.hpp"

#include <chrono>
#include <string>
#include <vector>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/transcript.hpp"

#include <string>

namespace lubancode::cli {

namespace {

// TurnItemViewState -> TranscriptStatus(终态词同一套:cli 的 Interrupted
// 对应 collector 的 Interrupted;Skipped 在 cli 侧折成 Cancelled——"没跑"
// 与"拒了"在紧凑画面上同一副灰灯,完整区分留在 TurnView 里)。
lubancode::cli::TranscriptStatus ProjectStatus(lubancode::runtime::TurnItemViewState status) {
    using S = lubancode::runtime::TurnItemViewState;
    using T = lubancode::cli::TranscriptStatus;
    switch (status) {
        case S::Pending: return T::Pending;
        case S::Running: return T::Running;
        case S::Succeeded: return T::Ok;
        case S::Failed: return T::Error;
        case S::Declined: return T::Cancelled;
        case S::Cancelled: return T::Cancelled;
        case S::Interrupted: return T::Interrupted;
        case S::Skipped: return T::Cancelled;
    }
    return T::Running;
}

// 摘要行:工具终态的 ⎿ 行。这里只做"事实底子"的投影——run_command 的
// 退出码/耗时、read_file 的行数这些摘要文案在 cli::transcript 有一整套
// (RunCommandDoneSummary 一族);TurnView 存的是领域真值(结果原文、
// 起止毫秒),折成同样的入参喂它们,措辞一处定。
std::vector<std::string> ProjectSummaryLines(const lubancode::runtime::TurnItemView& item) {
    using lubancode::runtime::TurnItemViewKind;
    if (item.kind == TurnItemViewKind::User) {
        return {};
    }
    if (item.kind == TurnItemViewKind::Thinking) {
        const double seconds =
            item.ended_at_ms > item.started_at_ms
                ? static_cast<double>(item.ended_at_ms - item.started_at_ms) / 1000.0
                : 0.0;
        return {};
    }
    if (item.kind == TurnItemViewKind::Text) {
        return {};  // 正文走 markdown 正文流,不折成条目摘要
    }
    if (!item.result_text.empty()) {
        // 工具:结果首行当摘要(与 ToolDisplay 的兜底分支同款);run_command
        // 的完整摘要文案(退出码/耗时)由实时路负责,重放路用首行。
        std::string first = item.result_text.substr(0, item.result_text.find('\n'));
        if (first.empty()) {
            first = "Done";
        }
        return {first};
    }
    if (item.status == lubancode::runtime::TurnItemViewState::Pending) {
        return {lubancode::cli::tr("transcript.batch_pending")};
    }
    if (item.status == lubancode::runtime::TurnItemViewState::Skipped) {
        return {lubancode::cli::tr("transcript.batch_skipped")};
    }
    return {};
}

}  // namespace

lubancode::cli::TranscriptItem ProjectTurnItem(const lubancode::runtime::TurnItemView& item) {
    lubancode::cli::TranscriptItem out;
    out.kind = item.parent_item_id.empty() ? lubancode::cli::TranscriptKind::Tool
                                           : lubancode::cli::TranscriptKind::SubTool;
    out.tool_name = item.tool_name;
    if (item.kind == lubancode::runtime::TurnItemViewKind::User) {
        out.title = item.result_text;
    } else if (item.kind == lubancode::runtime::TurnItemViewKind::Thinking) {
        out.tool_name = "thinking";
        const double seconds =
            item.ended_at_ms > item.started_at_ms
                ? static_cast<double>(item.ended_at_ms - item.started_at_ms) / 1000.0
                : 0.0;
        out.title = lubancode::cli::trf("transcript.thinking_done",
                                        lubancode::cli::FormatSeconds(seconds));
    } else if (item.kind == lubancode::runtime::TurnItemViewKind::Text) {
        out.tool_name = "assistant";
        out.title = item.result_text;
    } else {
        out.title = lubancode::cli::BuildToolTitle(item.tool_name, item.input);
    }
    out.input_json = item.input.is_null() || item.input.empty() ? std::string() : item.input.dump();
    out.full_output = item.result_text;
    out.summary_lines = ProjectSummaryLines(item);
    out.status = ProjectStatus(item.status);
    // 起止:TurnView 是 epoch 毫秒,TranscriptItem 是 steady 时点——重放路
    // 没有当年的 steady 钟,给相对值(耗时正确,绝对时点无意义)。
    out.start_time = std::chrono::steady_clock::time_point{} +
                     std::chrono::milliseconds(item.started_at_ms % 1'000'000);
    out.end_time = std::chrono::steady_clock::time_point{} +
                   std::chrono::milliseconds(item.ended_at_ms % 1'000'000);
    return out;
}

std::vector<std::string> RenderTurnView(const lubancode::runtime::TurnView& view, const Theme& theme,
                                        const TurnRenderOptions& options) {
    std::vector<std::string> lines;
    const int width = options.width > 0 ? options.width : 80;

    // 条目按 step 归组:step_id 为空的(user)排最前;每个 step 的条目按
    // 登记次序;换拍(第二个 step 起)垫一空行——轻间隔,不画满宽横线。
    // parent 非空的(子代理内层)跳过:它们沿 SubTool 规则折在父条目下,
    // 紧凑态不单独铺(与 FormatTranscriptItems 的紧凑规矩一致)。
    std::vector<std::string> ordered;
    for (const auto& step : view.steps) {
        for (const std::string& item_id : step.item_ids) {
            ordered.push_back(item_id);
        }
    }
    std::string previous_step;
    bool first_printed = false;
    for (const auto& item : view.items) {
        if (!item.parent_item_id.empty()) {
            continue;
        }
        if (item.kind == lubancode::runtime::TurnItemViewKind::User && !options.include_user) {
            continue;
        }
        if (item.kind == lubancode::runtime::TurnItemViewKind::Text) {
            continue;  // 正文走 markdown 正文流(实时 painter);重放另拼
        }
        // 用户条目:背景块(与 live 提交、resume 重放同一颗 formatter),不再
        // 折成工具条目样。块前按间距表垫一口气,块后再垫一口(UserPrompt ->
        // 任意 = 1);turn_divider 开着时,用户块之前再画一道满宽横线(轮界,
        // 克制样式:stats 淡色,与 PrintDivider 同一根线)。
        if (item.kind == lubancode::runtime::TurnItemViewKind::User) {
            const std::string block =
                lubancode::cli::FormatUserPromptBlock(item.result_text, theme, width);
            if (!block.empty()) {
                // 轮界横线:caller(多轮重放循环)从第二轮起置
                // leading_turn_divider,这里只照办——"上面有没有前一轮"是
                // 调用方的账,本函数的局部状态带不过轮。
                if (options.leading_turn_divider) {
                    for (int g = 0; g < lubancode::cli::GapBetween(lubancode::cli::BlockRole::TurnFooter,
                                                                   lubancode::cli::BlockRole::UserPrompt);
                         ++g) {
                        lines.push_back(std::string());
                    }
                    lines.push_back(theme.stats +
                                    lubancode::cli::BuildDividerLine(width, options.plain, width) +
                                    theme.reset);
                }
                std::size_t pos = 0;
                while (pos < block.size()) {
                    const std::size_t nl = block.find('\n', pos);
                    const std::size_t end = nl == std::string::npos ? block.size() : nl;
                    lines.push_back(block.substr(pos, end - pos));
                    if (nl == std::string::npos) {
                        break;
                    }
                    pos = nl + 1;
                }
                for (int g = 0; g < lubancode::cli::GapBetween(lubancode::cli::BlockRole::UserPrompt,
                                                               lubancode::cli::BlockRole::Thinking);
                     ++g) {
                    lines.push_back(std::string());
                }
                first_printed = true;
            }
            continue;
        }
        // step 换拍:垫轻间隔(首条之前不垫)。
        if (first_printed && !item.step_id.empty() && item.step_id != previous_step) {
            lines.push_back(std::string());
        }
        previous_step = item.step_id;
        first_printed = true;
        const lubancode::cli::TranscriptItem projected = ProjectTurnItem(item);
        const std::string text = lubancode::cli::FormatTranscriptItem(projected, theme, width,
                                                                     options.expanded);
        // FormatTranscriptItem 每行以 \n 收尾:拆进行组。
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t end = nl == std::string::npos ? text.size() : nl;
            lines.push_back(text.substr(pos, end - pos));
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
    }
    (void)ordered;

    // turn footer:恰一枚。终态词按 view.status 挑;墙钟走
    // metrics.wall_duration_ms(与 Working 活动条同一只钟的账)。footer 与
    // 末枚条目之间按间距表垫一口气(任意正文 -> TurnFooter = 1)。
    if (options.include_footer && view.finished) {
        using lubancode::runtime::TurnItemViewState;
        lubancode::cli::TurnFooterTone tone = lubancode::cli::TurnFooterTone::Worked;
        if (view.status == TurnItemViewState::Cancelled || view.status == TurnItemViewState::Interrupted) {
            tone = lubancode::cli::TurnFooterTone::Stopped;
        } else if (view.status == TurnItemViewState::Failed) {
            tone = lubancode::cli::TurnFooterTone::Failed;
        }
        if (first_printed) {
            for (int g = 0; g < lubancode::cli::GapBetween(lubancode::cli::BlockRole::Tool,
                                                           lubancode::cli::BlockRole::TurnFooter);
                 ++g) {
                lines.push_back(std::string());
            }
        }
        const std::string text =
            lubancode::cli::FormatTurnFooterText(view.metrics.wall_duration_ms, tone) +
            lubancode::cli::FormatApprovalWaitNote(view.metrics.approval_wait_ms);
        lines.push_back(lubancode::cli::BuildTurnFooterLine(text, width, options.plain));
    }
    return lines;
}

}  // namespace lubancode::cli
