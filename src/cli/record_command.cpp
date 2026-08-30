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
    TermOut() << trf("record.install.files", PathToUtf8(skills_root) + "/<"
                                               + tr("record.skill_name_placeholder") + ">")
              << "\n";
    for (const std::string& file : files) {
        TermOut() << "  " << file << "\n";
    }
    const auto answer = ReadLine(theme.confirm + tr("record.install.confirm") + theme.reset, theme,
                                 /*esc_rejects=*/true);
    const bool confirmed = answer.has_value() && (*answer == "y" || *answer == "Y");
    if (!confirmed) {
        TermOut() << tr("record.install.cancelled") << "\n";
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
    TermOut() << trf("record.install.done", installed->installed_names.empty()
                                                ? std::string("?")
                                                : installed->installed_names.front(),
                     PathToUtf8(skills_root))
              << "\n";
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
        TermOut() << tr("record.install.cancelled") << "\n";
        return;
    }
    const std::string choice = *answer;
    const fs::path skills_root = (choice == "h" || choice == "H") ? ctx.home_skills_root : (
                                 (choice == "p" || choice == "P" || choice.empty()) ? ctx.project_skills_root
                                                                                    : fs::path());
    if (skills_root.empty()) {
        TermOut() << tr("record.install.cancelled") << "\n";
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
                TermOut() << trf("record.status.recording",
                                 selection.paused() ? tr("record.status.paused_word")
                                                    : tr("record.status.recording_word"),
                                 selection.record_id(), std::string("trajectory"))
                          << "\n";
            } else {
                TermOut() << tr("record.status.idle") << "\n";
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
            TermOut() << trf("record.started", selection.record_id(), std::string("trajectory selection"))
                      << "\n";
            return;
        }
        case RecordCommandAction::Note: {
            const std::string error = selection.Note(command.text);
            if (!error.empty()) {
                fail(error);
                return;
            }
            TermOut() << tr("record.note_saved") << "\n";
            return;
        }
        case RecordCommandAction::Pause: {
            const std::string error = selection.Pause();
            if (!error.empty()) {
                fail(error);
                return;
            }
            TermOut() << tr("record.paused_msg") << "\n";
            return;
        }
        case RecordCommandAction::Resume: {
            const std::string error = selection.Resume();
            if (!error.empty()) {
                fail(error);
                return;
            }
            TermOut() << tr("record.resumed_msg") << "\n";
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
            TermOut() << trf("record.stop_done", id, std::string("trajectory selection")) << "\n";
            TermOut() << theme.stats
                      << "选段已封口(canonical 事件段与末 hash 已落 Journal);"
                         "技能草稿由轨迹导出(P0-5)从同一 selection 确定性重编。"
                      << theme.reset << "\n";
            return;
        }
        case RecordCommandAction::Cancel: {
            const std::string error = selection.Cancel();
            if (!error.empty()) {
                fail(error);
                return;
            }
            TermOut() << tr("record.cancel_done") << "\n";
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
                TermOut() << trf("record.status.recording",
                                 ctx.recorder->state() == skills::RecorderState::Paused
                                     ? tr("record.status.paused_word")
                                     : tr("record.status.recording_word"),
                                 ctx.recorder->name(), PathToUtf8(ctx.recorder->dir()))
                          << "\n";
            } else {
                TermOut() << tr("record.status.idle") << "\n";
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
            TermOut() << trf("record.started", started->id(), PathToUtf8(started->dir())) << "\n";
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
            TermOut() << tr("record.note_saved") << "\n";
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
            TermOut() << tr("record.paused_msg") << "\n";
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
            TermOut() << tr("record.resumed_msg") << "\n";
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
            TermOut() << trf("record.stop_done", id, PathToUtf8(dir)) << "\n";
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
            TermOut() << tr("record.cancel_done") << "\n";
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
            TermOut() << tr("record.list.header") << "\n";
            for (const auto& status : recordings) {
                TermOut() << trf("record.list.entry", status.id, status.name, status.started_at,
                                 status.finished ? tr("record.list.finished") : tr("record.list.unfinished"),
                                 status.has_draft ? tr("record.list.has_draft") : tr("record.list.no_draft"))
                          << "\n";
            }
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
            TermOut() << trf("record.discard_done", command.name) << "\n";
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
