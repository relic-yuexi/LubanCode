// subagent_isolation.hpp 的实现:0.27.x"模型侧 worktree 工具与子代理隔离"
// 的房务三件,自 agent_tool.cpp 原样搬来(病十四拆分,行为一字不改)。
#include "tools/subagent_isolation.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "tools/path_utils.hpp"

namespace lubancode::tools {

namespace {

// base_dir 包装层:路径入参按房解析成绝对路径,run_command 注入房作为
// 工作目录。
class BaseDirTool : public Tool {
public:
    BaseDirTool(Tool& inner, IsolationScope scope) : inner_(inner), scope_(std::move(scope)) {}

    std::string name() const override { return inner_.name(); }
    std::string description() const override { return inner_.description(); }
    nlohmann::json input_schema() const override { return inner_.input_schema(); }
    bool needs_confirm() const override { return inner_.needs_confirm(); }
    bool deferred() const override { return inner_.deferred(); }

    Result execute(const nlohmann::json& input) override { return inner_.execute(PatchPaths(input)); }

    // 取消旗透传(子代理 x 停止失效单):隔离房里的 run_command 就是靠这层
    // 壳挂进子代理工具表的——壳不透传 context,房内命令的进程树就收不到
    // 停止信号。改写后的入参与旧口同一份(PatchPaths),只多递一根旗。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override {
        return inner_.execute(PatchPaths(input), context);
    }

private:
    // 相对路径按房解析、run_command 缺 cwd 注入房目录:两个执行口共用的
    // 改写半边(原先内联在 execute 里)。
    nlohmann::json PatchPaths(const nlohmann::json& input) const {
        nlohmann::json patched = input;
        const std::string inner_name = inner_.name();
        if (inner_name == "read_file" || inner_name == "write_file" || inner_name == "edit_file" ||
            inner_name == "search") {
            const auto it = patched.find("path");
            if (it != patched.end() && it->is_string()) {
                const std::string path = it->get<std::string>();
                if (!path.empty() && !Utf8ToPath(path).is_absolute()) {
                    patched["path"] = scope_.base_dir + "/" + path;
                }
            }
        } else if (inner_name == "run_command") {
            if (patched.find("cwd") == patched.end()) {
                patched["cwd"] = scope_.base_dir;
            }
        }
        return patched;
    }

    Tool& inner_;
    IsolationScope scope_;
};

}  // namespace

std::optional<lubancode::cli::AgentWorktree> SetupIsolationRoom(const std::string& cwd,
                                                                const lubancode::cli::GitRunner& runner,
                                                                Tool::Result& error_out) {
    const std::filesystem::path cwd_path = Utf8ToPath(cwd);
    const auto repo_root = lubancode::cli::FindRepositoryRoot(cwd_path, runner);
    if (!repo_root.has_value()) {
        error_out = {"isolation=worktree 需要在 git 仓库里给子代理建房,当前目录不是仓库: " + cwd, true};
        return std::nullopt;
    }
    // 基线冻结(派工单 §三):派工瞬间的调用者 HEAD,先冻结再建房——本地
    // 分支领先远端时子代理照样在调用者的代码上开工,不再解析 origin/main。
    const lubancode::cli::FrozenWorktreeBase base = lubancode::cli::FreezeWorktreeBase(cwd_path, runner);
    if (base.commit.empty()) {
        error_out = {"isolation=worktree 冻结调用者 HEAD 失败(不在可用 git 仓库里?): " + cwd, true};
        return std::nullopt;
    }
    // 未提交改动(派工单 §3.4):产品不接未提交改动(房从提交起树),但必须
    // 明说,不能悄悄丢——给调用方的附言在这里攒好,随结果带回。
    std::string dirty_note;
    if (!lubancode::cli::WorktreeClean(cwd_path, runner)) {
        dirty_note = "调用者工作树有未提交改动,未提交改动不在房内(如需带上,先提交或写进任务说明)。";
    }
    lubancode::cli::AgentWorktree room =
        lubancode::cli::CreateAgentWorktree(*repo_root, base.commit, base.ref, runner);
    if (!room.ok) {
        error_out = {"给隔离子代理建 worktree 失败: " + room.error, true};
        // 半拉子房收拾掉,不留垃圾。
        if (!room.room_path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(room.room_path, ec);
        }
        return std::nullopt;
    }
    room.caller_note = "\n\n[隔离基线] base=" + room.base_ref + "@" + room.base_commit + "。" + dirty_note;
    return room;
}

lubancode::cli::AgentWorktreeFinish FinishIsolationRoom(const lubancode::cli::AgentWorktree& room,
                                                        const lubancode::cli::GitRunner& runner) {
    return lubancode::cli::FinishAgentWorktree(room.repo_root, room.room_path, room.branch, room.base_commit,
                                               runner);
}

std::unique_ptr<ToolRegistry> BuildIsolatedRegistry(ToolRegistry& source, const IsolationScope& scope) {
    auto out = std::make_unique<ToolRegistry>();
    for (const auto& tool : source.All()) {
        out->Register(std::make_unique<BaseDirTool>(*tool, scope));
    }
    return out;
}

}  // namespace lubancode::tools
