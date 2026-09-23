// Kanban 看板单:`lubancode kanban` 执行体。合同见 kanban_command.hpp。
#include "cli/kanban_command.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>  // std::cerr:错误行照旧走 stderr(批 7 不动错误流)
#include <string>

#include <nlohmann/json.hpp>

#include "app/version.hpp"
#include "cli/frame_notice.hpp"  // 批 7:产物提示走 frame 键值对框
#include "config/config.hpp"
#include "kanban/kanban_html.hpp"
#include "platform/process.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/index.hpp"

namespace lubancode::cli {

namespace {

// 浏览器打开(与 assistant_host 同款:失败不拦,路径已打印可复制)。
void OpenBrowserFor(const std::string& url) {
#ifdef _WIN32
    // cmd start 的空标题参数挡住"路径被当窗口标题"的老坑。
    const platform::ProcessResult result =
        platform::RunProcess({"cmd.exe", "/d", "/c", "start", "", url}, 8000);
    if (result.spawn_failed || result.exit_code != 0) {
        std::fprintf(stderr, "[kanban] 打开浏览器失败,请手动复制上面的地址(%s)\n",
                     result.spawn_failed ? result.spawn_error.c_str() : "start 退出码非零");
    }
#else
    for (const char* opener : {"xdg-open", "open"}) {
        const platform::ProcessResult result = platform::RunProcess({opener, url}, 8000);
        if (!result.spawn_failed && result.exit_code == 0) {
            return;
        }
    }
    std::fprintf(stderr, "[kanban] 打开浏览器失败(xdg-open/open 都不行),请手动复制上面的地址\n");
#endif
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json BuildBoardData(const std::filesystem::path& workspaces_root) {
    namespace trajectory = lubancode::trajectory;

    nlohmann::json projects = nlohmann::json::array();
    for (const auto& room : workspace::index::ScanRooms(workspaces_root)) {
        const auto& m = room.manifest;
        nlohmann::json checkouts = nlohmann::json::array();
        for (const auto& c : m.checkouts) {
            checkouts.push_back({{"root", c.root},
                                 {"first_seen_at_ms", c.first_seen_at_ms},
                                 {"last_seen_at_ms", c.last_seen_at_ms}});
        }
        projects.push_back({{"key", m.workspace_key},
                            {"name", m.display_name},
                            {"identity_kind", m.identity_kind},
                            {"identity_root", m.identity_root},
                            {"created_at_ms", m.created_at_ms},
                            {"last_opened_at_ms", m.last_opened_at_ms},
                            {"checkouts", std::move(checkouts)}});
    }

    trajectory::SessionIndexQuery query;
    query.all_workspaces = true;
    query.include_archived = true;
    const trajectory::SessionIndexPage page =
        trajectory::QueryWorkspaceSessions(workspaces_root, query);

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& s : page.entries) {
        sessions.push_back({{"id", s.session_id},
                            {"workspace_key", s.workspace_key},
                            {"status", s.status},
                            {"archived", s.archived},
                            {"run_kind", s.run_kind},
                            {"run_kind_unknown", s.run_kind_unknown},
                            {"title", s.title},
                            {"first_user_text", s.first_user_text},
                            {"cwd", s.cwd},
                            {"model", s.model},
                            {"created_at_ms", s.created_at_ms},
                            {"updated_at_ms", s.updated_at_ms},
                            {"message_count", s.message_count},
                            {"event_count", s.event_count},
                            {"damaged", s.damaged},
                            {"session_dir", s.session_dir}});
    }

    return {{"generated_at_ms", NowMs()},
            {"version", std::string(app::kVersion)},
            {"diagnostic", page.diagnostic},
            {"projects", std::move(projects)},
            {"sessions", std::move(sessions)}};
}

}  // namespace

std::vector<std::string> RenderKanbanNotice(const std::string& output_path,
                                            std::size_t project_count, std::size_t session_count,
                                            const Theme& theme, int width) {
    // 两句既有文案原样进框:第一句按句内冒号拆两列,第二句(无冒号)整句
    // 进 value。标题 kanban 是命令名(schema 标识符),不添文案。
    const std::vector<frame::Field> fields = {
        SentenceField("看板已生成: " + output_path),
        SentenceField(std::to_string(project_count) + " 个项目," + std::to_string(session_count) +
                      " 场会话"),
    };
    return frame::RenderKeyValues("kanban", fields, theme, frame::Light(), width);
}

int RunKanbanCommand(const KanbanCommandArgs& args) {
    const auto state_root = config::StateRootDir();
    if (!state_root.has_value() || state_root->empty()) {
        std::cerr << "kanban: 取不到状态根目录(用户主目录不可用)\n";
        return 1;
    }
    const std::filesystem::path state_dir = tools::Utf8ToPath(*state_root);
    const std::filesystem::path workspaces_root = state_dir / "workspaces";

    std::error_code ec;
    if (!std::filesystem::is_directory(workspaces_root, ec)) {
        std::cerr << "kanban: 还没有任何会话记录(" << tools::PathToUtf8(workspaces_root)
                  << " 不存在)\n";
        return 1;
    }

    const nlohmann::json data = BuildBoardData(workspaces_root);
    const std::string html = kanban::RenderKanbanHtml(data);

    const std::filesystem::path output =
        args.output.empty() ? state_dir / "kanban" / "kanban.html"
                            : tools::Utf8ToPath(args.output);
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path(), ec);
        if (ec) {
            std::cerr << "kanban: 建不了输出目录 " << tools::PathToUtf8(output.parent_path())
                      << ":" << ec.message() << "\n";
            return 1;
        }
    }
    {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "kanban: 写不进 " << tools::PathToUtf8(output) << "\n";
            return 1;
        }
        out << html;
        if (!out) {
            std::cerr << "kanban: 写入 " << tools::PathToUtf8(output) << " 中途失败\n";
            return 1;
        }
    }

    const auto session_count = data["sessions"].size();
    const auto project_count = data["projects"].size();
    // 批 7:产物提示走 frame 键值对框(RenderKanbanNotice 纯函数渲染,
    // 形状册直调钉形状);输出端口收口 TermOut(默认 stdout,与 std::cout
    // 同一目的地)。错误行(上面的 std::cerr)照旧,一字不动。
    EmitFrameLines(RenderKanbanNotice(tools::PathToUtf8(output), project_count, session_count,
                                      CliTheme(), CliFrameWidth()));
    TermOut().flush();
    if (!args.no_open) {
        OpenBrowserFor(tools::PathToUtf8(std::filesystem::absolute(output, ec)));
    }
    return 0;
}

}  // namespace lubancode::cli
