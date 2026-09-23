#include "cli/record_command.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "skills/skill_drafter.hpp"
#include "cli/console_input.hpp"
#include "cli/frame_notice.hpp"  // 批 7:清单/回执走 frame 三助手
#include "cli/i18n.hpp"
#include "cli/slash_commands.hpp"
#include "config/skill_store.hpp"

namespace lubancode::cli {

namespace {

namespace fs = std::filesystem;

std::string PathToUtf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::optional<std::string> Ask(const std::string& question, const Theme& theme) {
    // 管道/重定向模式退回 getline(ReadLine 内部自理);读尽(EOF)给
    // nullopt,调用方按"跳过"办——自动化脚本喂不齐三问也不卡死。
    return ReadLine(theme.stats + question + theme.reset + " ", theme, /*esc_rejects=*/true);
}

void PrintUsage() { TermOut() << tr("record.usage") << "\n"; }

// 草稿目录里的全部常规文件(相对路径,正斜杠),安装前列给用户看。
std::vector<std::string> DraftFiles(const fs::path& draft_dir) {
    std::vector<std::string> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(draft_dir, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        const std::u8string relative = it->path().lexically_relative(draft_dir).generic_u8string();
        files.push_back(std::string(reinterpret_cast<const char*>(relative.data()), relative.size()));
    }
    return files;
}

// 列文件 + y/N 确认 + 原子安装 + 刷新清单。返回是否真装上了。
bool InstallDraftWithConfirm(const fs::path& draft_dir, const fs::path& skills_root,
                             RecordCommandContext& ctx, const Theme& theme) {
    const std::vector<std::string> files = DraftFiles(draft_dir);
    // 批 7:引导句(尾冒号剥掉)做 frame 标题,文件清单走列表;空清单时
    // 句子照旧平铺(空态文案调用方管,框内无行可装)。
    const std::string files_sentence = trf("record.install.files",
                                           PathToUtf8(skills_root) + "/<" +
                                               tr("record.skill_name_placeholder") + ">");
    if (files.empty()) {
        TermOut() << files_sentence << "\n";
    } else {
        std::vector<frame::ListRow> rows;
        for (const std::string& file : files) {
            rows.push_back(frame::ListRow{file, {}, {}, frame::Bullet::None});
        }
        EmitFrameLines(frame::RenderList(StripTrailingColon(files_sentence), rows, theme,
                                         frame::Light(), CliFrameWidth()));
    }
    const auto answer = ReadLine(theme.confirm + tr("record.install.confirm") + theme.reset, theme,
                                 /*esc_rejects=*/true);
    const bool confirmed = answer.has_value() && (*answer == "y" || *answer == "Y");
    if (!confirmed) {
        PrintNotice(theme, {tr("record.install.cancelled")});
        return false;
    }
    const auto installed = config::InstallDraftSkill(
        skills_root, draft_dir, [](const std::string& content) {
            return skills::ValidateSkillMarkdownForInstall(content);
        });
    if (!installed.has_value()) {
        TermOut() << theme.error << trf("record.install.failed", installed.error()) << theme.reset << "\n";
        return false;
    }
    PrintNotice(theme, {trf("record.install.done", installed->installed_names.empty()
                                                        ? std::string("?")
                                                        : installed->installed_names.front(),
                             PathToUtf8(skills_root))});
    if (ctx.refresh_skills) {
        ctx.refresh_skills();
    }
    return true;
}

// /record stop 之后的那一串:落草稿、打预览、问装不装。
void FinishRecording(RecordCommandContext& ctx, const Theme& theme, const fs::path& recording_dir) {
    const std::vector<skills::RecordEvent> events = skills::ReadRecordingEvents(recording_dir);
    const auto draft = skills::WriteSkillDraft(recording_dir, events);
    if (!draft.has_value()) {
        TermOut() << theme.error << trf("record.stop.draft_failed", draft.error()) << theme.reset << "\n";
        return;
    }
    std::string draft_text;
    {
        std::ifstream file(draft->draft_dir / "SKILL.md", std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        draft_text = buffer.str();
    }
    TermOut() << trf("record.draft.header", draft->files.size()) << "\n\n" << draft_text << "\n";

    const auto answer = ReadLine(theme.confirm + tr("record.install.prompt") + theme.reset, theme,
                                 /*esc_rejects=*/true);
    if (!answer.has_value()) {
        PrintNotice(theme, {tr("record.install.cancelled")});
        return;
    }
    const std::string choice = *answer;
    const fs::path skills_root = (choice == "h" || choice == "H") ? ctx.home_skills_root : (
                                 (choice == "p" || choice == "P" || choice.empty()) ? ctx.project_skills_root
                                                                                    : fs::path());
    if (skills_root.empty()) {
        PrintNotice(theme, {tr("record.install.cancelled")});
        return;
    }
    InstallDraftWithConfirm(draft->draft_dir, skills_root, ctx, theme);
}

}  // namespace

std::string RecorderStatusMarker(const std::optional<skills::WorkflowRecorder>& recorder) {
    if (!recorder.has_value()) {
        return std::string();
    }
    if (recorder->state() == skills::RecorderState::Paused) {
        return tr("record.status.paused_marker");
    }
    return "REC · " + recorder->name();
}

// P0-2 轨迹选段器(§14.3):flag 开的会话里 /record 的全套动作。选段只圈
// canonical refs(record.selection.* 事件进 main Journal),不旁听、不复制
// 事实;草稿由 P0-5 的 SkillDraftCompiler 从同一 selection 确定性重编,
// 这里 stop 后只报段位,不起草。
void HandleRecordSelection(const ParsedRecordCommand& command, const std::string& args,
                           RecordCommandContext& ctx, const Theme& theme) {
    runtime::RecordSelectionController& selection = *ctx.selection;
    const auto fail = [&theme](const std::string& code) {
        TermOut() << theme.error << trf("record.op_failed", code) << theme.reset << "\n";
    };
    switch (command.action) {
        case RecordCommandAction::Status:
            if (selection.active()) {
                PrintNotice(theme, {trf("record.status.recording",
                                        selection.paused() ? tr("record.status.paused_word")
                                                           : tr("record.status.recording_word"),
                                        selection.record_id(), std::string("trajectory"))});
            } else {
                PrintNotice(theme, {tr("record.status.idle")});
            }
            return;
        case RecordCommandAction::Start: {
            if (selection.active()) {
                TermOut() << theme.error << trf("record.already_active", selection.record_id()) << theme.reset
                          << "\n";
                return;
            }
            std::string goal;
            std::vector<std::string> variables;
            std::string acceptance;
            if (const auto asked = Ask(tr("record.ask.goal"), theme); asked.has_value()) {
                goal = *asked;
            }
            if (const auto asked = Ask(tr("record.ask.variables"), theme); asked.has_value() && !asked->empty()) {
                variables.push_back(*asked);
            }
            if (const auto asked = Ask(tr("record.ask.acceptance"), theme); asked.has_value()) {
                acceptance = *asked;
            }
            const std::string error = selection.Start(command.name, goal, variables, acceptance);
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {trf("record.started", selection.record_id(),
                                    std::string("trajectory selection"))});
            return;
        }
        case RecordCommandAction::Note: {
            const std::string error = selection.Note(command.text);
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {tr("record.note_saved")});
            return;
        }
        case RecordCommandAction::Pause: {
            const std::string error = selection.Pause();
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {tr("record.paused_msg")});
            return;
        }
        case RecordCommandAction::Resume: {
            const std::string error = selection.Resume();
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {tr("record.resumed_msg")});
            return;
        }
        case RecordCommandAction::Stop: {
            std::string verification;
            if (const auto asked = Ask(tr("record.ask.verification"), theme); asked.has_value()) {
                verification = *asked;
            }
            const std::string id = selection.record_id();
            const std::string error = selection.Stop(verification);
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {trf("record.stop_done", id, std::string("trajectory selection")),
                                // 硬编码老文案(不走 i18n)原样进框,一字不改
                                //(批 2 裁量 1);stats 色由键值对助手降为默认档。
                                std::string("选段已封口(canonical 事件段与末 hash 已落 Journal);"
                                            "技能草稿由轨迹导出(P0-5)从同一 selection 确定性重编。")});
            return;
        }
        case RecordCommandAction::Cancel: {
            const std::string error = selection.Cancel();
            if (!error.empty()) {
                fail(error);
                return;
            }
            PrintNotice(theme, {tr("record.cancel_done")});
            return;
        }
        default:
            // list/discard/install 仍是 recordings 目录的旧管理面,照旧路走。
            break;
    }
    // 落到旧路(list/discard/install)。
    RecordCommandContext legacy = ctx;
    legacy.selection = nullptr;
    HandleRecordCommand(args, legacy, theme);
}

void HandleRecordCommand(const std::string& args, RecordCommandContext& ctx, const Theme& theme) {
    const ParsedRecordCommand command = ParseRecordCommand(args);

    // P0-2 轨迹:选段器在位时,record 生命周期动作全走 selection;list/
    // discard/install(旧录制的管理面)照旧路。
    if (ctx.selection != nullptr &&
        (command.action == RecordCommandAction::Start || command.action == RecordCommandAction::Status ||
         command.action == RecordCommandAction::Note || command.action == RecordCommandAction::Pause ||
         command.action == RecordCommandAction::Resume || command.action == RecordCommandAction::Stop ||
         command.action == RecordCommandAction::Cancel)) {
        HandleRecordSelection(command, args, ctx, theme);
        return;
    }

    switch (command.action) {
        case RecordCommandAction::Invalid:
            PrintUsage();
            return;
        case RecordCommandAction::Status:
            if (ctx.recorder.has_value()) {
                PrintNotice(theme, {trf("record.status.recording",
                                        ctx.recorder->state() == skills::RecorderState::Paused
                                            ? tr("record.status.paused_word")
                                            : tr("record.status.recording_word"),
                                        ctx.recorder->name(), PathToUtf8(ctx.recorder->dir()))});
            } else {
                PrintNotice(theme, {tr("record.status.idle")});
            }
            return;
        case RecordCommandAction::Start: {
            if (ctx.recorder.has_value()) {
                TermOut() << theme.error << trf("record.already_active", ctx.recorder->name()) << theme.reset
                          << "\n";
                return;
            }
            if (ctx.recordings_root.empty()) {
                TermOut() << theme.error << tr("record.unavailable") << theme.reset << "\n";
                return;
            }
            // 开录先问三句:目标、可变输入、成事标准。管道里喂不齐就空着,
            // 草稿里如实落"(未口述)"。
            skills::RecordingStartInfo info;
            info.name = command.name;
            if (const auto goal = Ask(tr("record.ask.goal"), theme); goal.has_value()) {
                info.goal = *goal;
            }
            if (const auto variables = Ask(tr("record.ask.variables"), theme); variables.has_value()) {
                if (!variables->empty()) {
                    info.variables.push_back(*variables);
                }
            }
            if (const auto acceptance = Ask(tr("record.ask.acceptance"), theme); acceptance.has_value()) {
                info.acceptance = *acceptance;
            }
            auto started = skills::WorkflowRecorder::Start(ctx.recordings_root, info);
            if (!started.has_value()) {
                TermOut() << theme.error << trf("record.start.failed", started.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {trf("record.started", started->id(), PathToUtf8(started->dir()))});
            ctx.recorder.emplace(std::move(*started));
            return;
        }
        case RecordCommandAction::Note:
            if (!ctx.recorder.has_value()) {
                TermOut() << theme.error << tr("record.not_active") << theme.reset << "\n";
                return;
            }
            if (const auto noted = ctx.recorder->Note(command.text); !noted.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", noted.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {tr("record.note_saved")});
            return;
        case RecordCommandAction::Pause:
            if (!ctx.recorder.has_value()) {
                TermOut() << theme.error << tr("record.not_active") << theme.reset << "\n";
                return;
            }
            if (const auto paused = ctx.recorder->Pause(); !paused.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", paused.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {tr("record.paused_msg")});
            return;
        case RecordCommandAction::Resume:
            if (!ctx.recorder.has_value()) {
                TermOut() << theme.error << tr("record.not_active") << theme.reset << "\n";
                return;
            }
            if (const auto resumed = ctx.recorder->Resume(); !resumed.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", resumed.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {tr("record.resumed_msg")});
            return;
        case RecordCommandAction::Stop: {
            if (!ctx.recorder.has_value()) {
                TermOut() << theme.error << tr("record.not_active") << theme.reset << "\n";
                return;
            }
            std::string verification;
            if (const auto asked = Ask(tr("record.ask.verification"), theme); asked.has_value()) {
                verification = *asked;
            }
            const std::string id = ctx.recorder->id();
            const auto stopped = ctx.recorder->Stop(verification);
            const fs::path dir = ctx.recorder->dir();
            ctx.recorder.reset();
            if (!stopped.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", stopped.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {trf("record.stop_done", id, PathToUtf8(dir))});
            FinishRecording(ctx, theme, dir);
            return;
        }
        case RecordCommandAction::Cancel: {
            if (!ctx.recorder.has_value()) {
                TermOut() << theme.error << tr("record.not_active") << theme.reset << "\n";
                return;
            }
            const auto cancelled = ctx.recorder->Cancel();
            ctx.recorder.reset();
            if (!cancelled.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", cancelled.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {tr("record.cancel_done")});
            return;
        }
        case RecordCommandAction::List: {
            if (ctx.recordings_root.empty()) {
                TermOut() << theme.error << tr("record.unavailable") << theme.reset << "\n";
                return;
            }
            const auto recordings = skills::ListRecordings(ctx.recordings_root);
            if (recordings.empty()) {
                TermOut() << tr("record.list.empty") << "\n";
                return;
            }
            // 批 7:清单走表格。列头用数据 schema 名(批 1 裁量 1);state/
            // draft 两列的值是既有 i18n 文案,一字不改;record.list.entry
            // 的五段信息全数入列(不再拼句)。语义色:已停止=Pass、未完成=
            // Fail(error 色)、有草稿=Pass。
            std::vector<frame::TableColumn> columns;
            columns.push_back({"id"});
            columns.push_back({"name"});
            columns.push_back({"started"});
            columns.push_back({"state"});
            columns.push_back({"draft"});
            std::vector<frame::TableRow> rows;
            for (const auto& status : recordings) {
                rows.push_back(frame::TableRow{
                    {status.id, status.name, status.started_at,
                     status.finished ? tr("record.list.finished") : tr("record.list.unfinished"),
                     status.has_draft ? tr("record.list.has_draft") : tr("record.list.no_draft")},
                    {frame::CellTone::Normal, frame::CellTone::Normal, frame::CellTone::Normal,
                     status.finished ? frame::CellTone::Pass : frame::CellTone::Fail,
                     status.has_draft ? frame::CellTone::Pass : frame::CellTone::Normal}});
            }
            EmitFrameLines(frame::RenderTable(StripTrailingColon(tr("record.list.header")), columns,
                                              rows, theme, frame::Light(), CliFrameWidth()));
            return;
        }
        case RecordCommandAction::Discard: {
            if (ctx.recordings_root.empty()) {
                TermOut() << theme.error << tr("record.unavailable") << theme.reset << "\n";
                return;
            }
            const auto discarded = skills::DiscardRecording(ctx.recordings_root, command.name);
            if (!discarded.has_value()) {
                TermOut() << theme.error << trf("record.op_failed", discarded.error()) << theme.reset << "\n";
                return;
            }
            PrintNotice(theme, {trf("record.discard_done", command.name)});
            return;
        }
        case RecordCommandAction::Install: {
            if (ctx.recordings_root.empty()) {
                TermOut() << theme.error << tr("record.unavailable") << theme.reset << "\n";
                return;
            }
            const auto recordings = skills::ListRecordings(ctx.recordings_root);
            const auto found = std::find_if(recordings.begin(), recordings.end(),
                                            [&](const skills::RecordingStatus& status) {
                                                return status.id == command.name;
                                            });
            if (found == recordings.end()) {
                TermOut() << theme.error << trf("record.install.not_found", command.name) << theme.reset << "\n";
                return;
            }
            if (!found->has_draft) {
                TermOut() << theme.error << trf("record.install.no_draft", command.name) << theme.reset << "\n";
                return;
            }
            const fs::path skills_root = command.to_project ? ctx.project_skills_root : ctx.home_skills_root;
            InstallDraftWithConfirm(found->dir / "draft", skills_root, ctx, theme);
            return;
        }
    }
}

}  // namespace lubancode::cli
