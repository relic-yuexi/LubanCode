#include "tools/worktree_tool.hpp"

#include <utility>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

WorktreeTool::WorktreeTool(lubancode::cli::WorktreeSession& session, ConfirmHandler confirm,
                           std::function<void()> on_session_moved)
    : session_(session), confirm_(std::move(confirm)), on_session_moved_(std::move(on_session_moved)) {}

std::string WorktreeTool::name() const {
    return "worktree";
}

std::string WorktreeTool::description() const {
    // 文案在 src/prompts/tools/<语言>/worktree.md,兜底是迁移前的原文。
    return ToolText("worktree", "description",
                    "住进隔离的 git worktree 里干活,不碰主 checkout。大改动先 worktree enter(缺省名字自动生成,"
                    "基准 fresh=远端默认分支或 head=当前 HEAD),整场会话搬进房里:读写、命令都在房内,"
                    "状态行会亮房名;干完 worktree exit keep(留房)或 exit remove(干净才删,脏了要用户确认)。"
                    "worktree status 看在不在房里、脏没脏;worktree list 列全部工作树。"
                    "别把构建产物提交进房里;房里的改动最终仍要合回主分支。");
}

nlohmann::json WorktreeTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json action_prop = nlohmann::json::object();
    action_prop["type"] = "string";
    action_prop["enum"] = nlohmann::json::array({"enter", "status", "list", "exit"});
    action_prop["description"] =
        ToolText("worktree", "param.action",
                 "enter=建房或进已有房(整场会话搬进去);status=房内状态;list=列工作树;"
                 "exit=搬回原处(配 mode)");
    properties["action"] = action_prop;

    nlohmann::json name_prop = nlohmann::json::object();
    name_prop["type"] = "string";
    name_prop["description"] =
        ToolText("worktree", "param.name",
                 "enter 时的房名(字母数字-_),不填自动生成;也可传已有 worktree 的名字或路径,"
                 "园子(.lubancode/worktrees)之外的房要先经用户确认");
    properties["name"] = name_prop;

    nlohmann::json base_prop = nlohmann::json::object();
    base_prop["type"] = "string";
    base_prop["enum"] = nlohmann::json::array({"fresh", "head"});
    base_prop["description"] =
        ToolText("worktree", "param.base",
                 "enter 建新房的基准:fresh=远端默认分支(缺省,fetch 5 秒封顶失败回落本地);"
                 "head=当前 HEAD");
    properties["base"] = base_prop;

    nlohmann::json mode_prop = nlohmann::json::object();
    mode_prop["type"] = "string";
    mode_prop["enum"] = nlohmann::json::array({"keep", "remove"});
    mode_prop["description"] =
        ToolText("worktree", "param.mode",
                 "exit 的方式:keep=房留在盘上;remove=干净才删(脏了必须用户确认,别替用户点头)");
    properties["mode"] = mode_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"action"});

    return schema;
}

Tool::Result WorktreeTool::execute(const nlohmann::json& input) {
    if (!input.contains("action") || !input.at("action").is_string()) {
        return {"缺少必填参数 action(enter/status/list/exit)", true};
    }
    const std::string action = input.at("action").get<std::string>();
    if (action == "enter") {
        return HandleEnter(input);
    }
    if (action == "exit") {
        return HandleExit(input);
    }
    if (action == "status") {
        const lubancode::cli::WorktreeResult result = session_.Status();
        if (result.code == lubancode::cli::WorktreeResultCode::NoActiveWorktree) {
            return {"当前不在任何 worktree 房里(cwd 就是主工作目录)。", false};
        }
        std::string text = "住在 worktree 房里:\n路径: " + PathToUtf8(result.path) + "\n分支: " + result.branch +
                           "\n状态: " + (result.detail == "clean" ? "干净" : "有未提交改动");
        return {std::move(text), false};
    }
    if (action == "list") {
        const lubancode::cli::WorktreeResult result = session_.List();
        if (result.code == lubancode::cli::WorktreeResultCode::NotRepository) {
            return {"这里不在 git 仓库里。", true};
        }
        if (result.code != lubancode::cli::WorktreeResultCode::Listed) {
            return {"列工作树失败: " + result.detail, true};
        }
        std::string text = "工作树清单:";
        for (const auto& entry : result.entries) {
            text += "\n- " + PathToUtf8(entry.path);
            text += entry.branch.empty() ? (entry.detached ? "(游离 HEAD)" : "") : (" 分支 " + entry.branch);
            if (entry.locked) {
                text += "(已锁)";
            }
        }
        if (result.entries.size() > 1) {
            text += "\n陈房可用 git worktree remove 收拾;带活(改动/未跟踪)的房先处理改动。";
        }
        return {std::move(text), false};
    }
    return {"action 只认 enter/status/list/exit", true};
}

Tool::Result WorktreeTool::HandleEnter(const nlohmann::json& input) {
    std::string name;
    if (const auto it = input.find("name"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"name 得是字符串", true};
        }
        name = it->get<std::string>();
    }
    std::string base = "fresh";
    if (const auto it = input.find("base"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"base 得是 fresh 或 head", true};
        }
        base = it->get<std::string>();
    }

    lubancode::cli::WorktreeResult result = session_.Enter(name, base);
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsUserConfirmation) {
        // 硬安全线:进园子外的房,确认档压不住,只有真用户点头才行。
        bool allowed = false;
        if (confirm_) {
            const auto answer = confirm_("要进 LubanCode 自家 worktree 目录之外的房吗?\n  " + result.detail +
                                         "\n这会把会话工作目录、写权限和项目配置都搬过去 [y/N]: ");
            allowed = answer.has_value() && *answer;
        }
        if (!allowed) {
            return {"进 " + result.detail + " 需要用户确认,已被拒绝。可让用户敲 /worktree 或换 .lubancode/"
                                             "worktrees 之内的房名。",
                    true};
        }
        result = session_.Enter(name, base, /*confirmed_outside=*/true);
    }
    switch (result.code) {
        case lubancode::cli::WorktreeResultCode::Created: {
            std::string text = "已住进 worktree 房:\n路径: " + PathToUtf8(result.path) + "\n分支: " + result.branch +
                               "\n整场会话已搬进去:读写、命令都在房内;干完用 worktree exit keep|remove 出房。";
            if (on_session_moved_) {
                on_session_moved_();
            }
            return {std::move(text), false};
        }
        case lubancode::cli::WorktreeResultCode::VerificationFailed:
            return {"拒绝进房,验明正身没过: " + result.detail, true};
        case lubancode::cli::WorktreeResultCode::AlreadyActive:
            return {"会话已住在房里(" + PathToUtf8(result.path) + "),先 worktree exit。", true};
        case lubancode::cli::WorktreeResultCode::InvalidName:
            return {"房名不合法(字母数字-_,64 字符内): " + result.detail, true};
        case lubancode::cli::WorktreeResultCode::NotRepository:
            return {"这里不在 git 仓库里,建不了 worktree。", true};
        case lubancode::cli::WorktreeResultCode::GitError:
        case lubancode::cli::WorktreeResultCode::FilesystemError:
            return {"进房失败: " + result.detail, true};
        default:
            return {"进房失败: " + result.detail, true};
    }
}

Tool::Result WorktreeTool::HandleExit(const nlohmann::json& input) {
    std::string mode = "keep";
    if (const auto it = input.find("mode"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"mode 得是 keep 或 remove", true};
        }
        mode = it->get<std::string>();
    }
    lubancode::cli::WorktreeResult result = session_.Exit(mode);
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsRemoveConfirmation) {
        // 脏房强删,用户点头才动,yolo 也不豁免。
        bool allowed = false;
        if (confirm_) {
            const auto answer = confirm_("worktree 房里有未提交改动:\n  " + PathToUtf8(result.path) +
                                         "\n仍要强删这间房和它的分支?改动将丢失 [y/N]: ");
            allowed = answer.has_value() && *answer;
        }
        if (!allowed) {
            return {"房里有未提交改动(" + PathToUtf8(result.path) +
                        "),删房需用户确认,已被拒绝。房原样保留,可先提交或让用户处理。",
                    true};
        }
        result = session_.ConfirmRemove();
    }
    switch (result.code) {
        case lubancode::cli::WorktreeResultCode::Kept:
            if (on_session_moved_) {
                on_session_moved_();
            }
            return {"已搬回原目录,房留着: " + PathToUtf8(result.path), false};
        case lubancode::cli::WorktreeResultCode::Removed:
            if (on_session_moved_) {
                on_session_moved_();
            }
            return {"已搬回原目录,房与分支已删: " + PathToUtf8(result.path), false};
        case lubancode::cli::WorktreeResultCode::NoActiveWorktree:
            return {"会话不在任何 worktree 房里,无可退出。", true};
        case lubancode::cli::WorktreeResultCode::InvalidArgument:
            return {"mode 只认 keep 或 remove", true};
        default:
            return {"出房失败: " + result.detail, true};
    }
}

}  // namespace lubancode::tools
