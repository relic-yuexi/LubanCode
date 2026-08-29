// 自然语言编译器实现(自然语言编排单第 5 批)。

#include "workflow/compiler.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "platform/paths.hpp"
#include "workflow/graph_view.hpp"
#include "workflow/parser.hpp"

namespace lubancode::workflow {

namespace {

bool WriteFileAtomically(const std::filesystem::path& path, const std::string& content, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "目录建不成: " + lubancode::platform::PathToUtf8(path.parent_path()) + ": " + ec.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "写不开: " + lubancode::platform::PathToUtf8(path);
        return false;
    }
    file << content;
    if (!file) {
        error = "写半截: " + lubancode::platform::PathToUtf8(path);
        return false;
    }
    return true;
}

}  // namespace

std::expected<WorkflowDraft, std::vector<ValidationIssue>> ParseDraftJson(
    const nlohmann::json& candidate, const CapabilityTable& capabilities) {
    std::vector<ValidationIssue> issues;
    WorkflowDraft draft;

    if (!candidate.is_object()) {
        issues.push_back(ValidationIssue{"bad_draft", "draft", "候选 AST 不是对象"});
        return std::unexpected(std::move(issues));
    }

    // 定义本体:模型输出就是归一化 JSON 形状(与 FromJson 同构)。
    try {
        draft.definition = WorkflowDefinition::FromJson(candidate);
    } catch (const std::exception& e) {
        issues.push_back(ValidationIssue{"bad_draft", "draft", std::string("候选 AST 还原失败: ") + e.what()});
        return std::unexpected(std::move(issues));
    }

    // unknowns / warnings(模型自己列的;宿主信一半,缺的关键字段再补)。
    if (const auto it = candidate.find("unknowns"); it != candidate.end() && it->is_array()) {
        for (const auto& u : *it) {
            if (!u.is_object()) continue;
            DraftUnknown unknown;
            if (const auto f = u.find("field"); f != u.end() && f->is_string()) unknown.field = f->get<std::string>();
            if (const auto q = u.find("question"); q != u.end() && q->is_string()) unknown.question = q->get<std::string>();
            draft.unknowns.push_back(std::move(unknown));
        }
    }
    if (const auto it = candidate.find("warnings"); it != candidate.end() && it->is_array()) {
        for (const auto& w : *it) {
            if (w.is_string()) draft.warnings.push_back(w.get<std::string>());
        }
    }

    // 宿主侧第一道:id/alias 合法性。
    if (!IsValidWorkflowId(draft.definition.id)) {
        issues.push_back(ValidationIssue{"bad_id", "id", "id 只认小写字母开头的小写字母/数字/-"});
    }
    if (!draft.definition.alias.empty() && !IsValidAlias(draft.definition.alias)) {
        issues.push_back(ValidationIssue{"bad_alias", "alias", "alias 含空白/斜杠/控制符"});
    }

    // 能力对账:模型杜撰的工具名在这里抓(validator 的能力检查同源)。
    const auto structure = ValidateDefinition(draft.definition, capabilities);
    for (const auto& issue : structure.issues) {
        // 图不完整的部分(编辑中)记 unknowns,不拦预览;能力类错误直接拦。
        if (issue.code == "unknown_tool" || issue.code == "unknown_skill" || issue.code == "unknown_transform" ||
            issue.code == "unknown_subflow" || issue.code == "unknown_agent" ||
            issue.code == "plaintext_secret" || issue.code == "path_escape") {
            issues.push_back(issue);
        }
    }

    if (!issues.empty()) {
        return std::unexpected(std::move(issues));
    }
    draft.complete = draft.unknowns.empty() && structure.ok();
    return draft;
}

std::vector<DraftUnknown> ComputeClarifications(const WorkflowDraft& draft) {
    // 只列 blocking 的缺口;已有答案的(unknowns.field 在 definition 里已
    // 有值)跳过。会改变图的岔路优先:输入 > 产物 > 失败规矩 > 并行度。
    std::vector<DraftUnknown> out;
    std::set<std::string> known_fields;
    if (!draft.definition.inputs.empty()) known_fields.insert("inputs");
    if (!draft.definition.outputs.empty()) known_fields.insert("outputs");
    if (!draft.definition.nodes.empty()) known_fields.insert("graph");
    if (!draft.definition.alias.empty()) known_fields.insert("alias");

    const char* kPriority[] = {"inputs", "outputs", "failure_policy", "parallelism", "sources", "graph",
                               "approval", "alias"};
    for (const char* field : kPriority) {
        for (const auto& unknown : draft.unknowns) {
            if (unknown.field != field || !unknown.blocking) continue;
            if (known_fields.count(field) > 0 && field != std::string("failure_policy")) continue;
            out.push_back(unknown);
        }
    }
    return out;
}

std::string BuildPreviewText(const WorkflowDraft& draft, const PreviewOptions& options) {
    const WorkflowDefinition& def = draft.definition;
    std::ostringstream out;
    out << "── Workflow 预览 ──\n";
    out << "  名字: " << def.name << " [" << def.id << " v" << def.version << "]\n";
    out << "  说明: " << def.description << "\n";
    if (!def.alias.empty()) {
        out << "  直呼: /" << def.alias << " <参数>\n";
    }
    out << "  安装: " << (options.scope == WorkflowScope::Project ? "项目级" : "用户级") << " -> "
        << lubancode::platform::PathToUtf8(options.install_dir) << "\n";

    out << "\n  输入: " << (def.inputs.empty() ? std::string("(未定)") : def.inputs.dump()) << "\n";
    out << "  产物: " << (def.outputs.empty() ? std::string("(未定)") : def.outputs.dump()) << "\n";
    out << "  预算: 并发 " << def.limits.max_concurrency << " · 节点 " << def.nodes.size() << "/"
        << def.limits.max_nodes << " · 步数 " << def.limits.max_steps << " · 时限 " << def.limits.timeout_secs
        << "s · 工具调用 " << def.limits.tool_calls << "\n";

    out << "\n  工具与节点:\n";
    for (const auto& node : def.nodes) {
        out << "    " << NodeSummaryLine(node) << "\n";
    }

    out << "\n  失败规矩:\n";
    bool any_policy = false;
    for (const auto& node : def.nodes) {
        if (node.on_unavailable != OnUnavailable::Fail) {
            out << "    " << node.id << ": 缺失时 " << ToString(node.on_unavailable) << "\n";
            any_policy = true;
        }
        if (node.retry.has_value() && node.retry->attempts > 1) {
            out << "    " << node.id << ": 重试 " << node.retry->attempts << " 次\n";
            any_policy = true;
        }
    }
    if (!any_policy) out << "    (默认:出错即停)\n";

    out << "\n  图:\n" << RenderAsciiGraph(def);

    if (!draft.warnings.empty()) {
        out << "\n  警告:\n";
        for (const auto& warning : draft.warnings) out << "    - " << warning << "\n";
    }
    if (!draft.unknowns.empty()) {
        out << "\n  待补缺口:\n";
        for (const auto& unknown : draft.unknowns) {
            out << "    - " << (unknown.question.empty() ? unknown.field : unknown.question) << "\n";
        }
    }

    out << "\n  将写文件:\n";
    out << "    " << lubancode::platform::PathToUtf8(options.install_dir / "workflow.yaml") << "\n";
    for (const auto& node : def.nodes) {
        for (const std::string* ref : {&node.prompt, &node.task, &node.template_path}) {
            if (!ref->empty()) {
                out << "    " << lubancode::platform::PathToUtf8(options.install_dir / *ref) << "\n";
            }
        }
    }
    return out.str();
}

std::expected<InstallResult, std::string> InstallWorkflow(
    const WorkflowDefinition& definition, const std::optional<std::filesystem::path>& project_root,
    const std::optional<std::filesystem::path>& user_root, const InstallOptions& options,
    const std::map<std::string, std::string>& prompt_files) {
    // 落点。
    const std::filesystem::path& root =
        options.scope == WorkflowScope::Project ? *project_root : *user_root;
    std::error_code ec;
    const std::filesystem::path workflows_root = root / ".lubancode" / "workflows";
    const std::filesystem::path target = workflows_root / definition.id;
    if (std::filesystem::exists(target, ec)) {
        if (!options.overwrite) {
            return std::unexpected("已存在同 id workflow: " + lubancode::platform::PathToUtf8(target) +
                                   "(要换用 /workflow edit)");
        }
    }

    // 包内引用守门 + prompt 文件齐不齐。
    std::vector<std::pair<std::string, std::string>> files;  // (相对路径, 正文)
    files.emplace_back("workflow.yaml", EmitWorkflowYaml(definition));
    const auto collect_ref = [&](const std::string& ref) {
        if (ref.empty() || !IsSafePackageRelative(ref)) return;
        const auto it = prompt_files.find(ref);
        if (it != prompt_files.end()) {
            files.emplace_back(ref, it->second);
        }
    };
    for (const auto& node : definition.nodes) {
        collect_ref(node.prompt);
        collect_ref(node.task);
        collect_ref(node.template_path);
    }

    // staging:先写临时目录,全齐了 rename 进位——半截包不进可用清单。
    const std::filesystem::path staging = workflows_root / (".staging-" + definition.id);
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        return std::unexpected("staging 建不成: " + ec.message());
    }
    InstallResult result;
    for (const auto& [relative, content] : files) {
        std::string error;
        if (!WriteFileAtomically(staging / relative, content, error)) {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected(error);
        }
        result.written_files.push_back(relative);
    }
    // 原子换入:目标在就先挪走,再 rename,最后清旧的。
    std::filesystem::path backup;
    if (std::filesystem::exists(target, ec)) {
        backup = workflows_root / (".bak-" + definition.id);
        std::filesystem::remove_all(backup, ec);
        std::filesystem::rename(target, backup, ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected("旧定义挪不开: " + ec.message());
        }
    }
    std::filesystem::rename(staging, target, ec);
    if (ec) {
        if (!backup.empty()) std::filesystem::rename(backup, target, ec);
        std::filesystem::remove_all(staging, ec);
        return std::unexpected("安装换名不成: " + ec.message());
    }
    if (!backup.empty()) {
        std::filesystem::remove_all(backup, ec);
    }
    result.dir = target;
    result.content_hash = ContentHash(definition);
    return result;
}

std::expected<InstallResult, std::string> UpdateWorkflow(
    const WorkflowDefinition& definition, const std::filesystem::path& existing_dir,
    const std::map<std::string, std::string>& prompt_files) {
    std::error_code ec;
    if (!std::filesystem::exists(existing_dir / "workflow.yaml", ec)) {
        return std::unexpected("目标目录没有 workflow.yaml: " + lubancode::platform::PathToUtf8(existing_dir));
    }
    // 回滚件:.bak 里留旧定义全文(单子:留一份可回滚旧定义)。
    const std::filesystem::path bak = existing_dir / "workflow.yaml.bak";
    std::error_code copy_ec;
    std::filesystem::copy_file(existing_dir / "workflow.yaml", bak,
                               std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
        return std::unexpected("回滚件写不成: " + copy_ec.message());
    }
    InstallResult result;
    std::string error;
    if (!WriteFileAtomically(existing_dir / "workflow.yaml", EmitWorkflowYaml(definition), error)) {
        return std::unexpected(error);
    }
    result.written_files.push_back("workflow.yaml");
    for (const auto& [relative, content] : prompt_files) {
        if (!IsSafePackageRelative(relative)) continue;
        if (!WriteFileAtomically(existing_dir / relative, content, error)) {
            return std::unexpected(error);
        }
        result.written_files.push_back(relative);
    }
    result.dir = existing_dir;
    result.content_hash = ContentHash(definition);
    return result;
}

std::expected<void, std::string> SetWorkflowEnabled(const std::filesystem::path& workflow_dir, bool enabled) {
    auto parsed = LoadWorkflowDefinition(workflow_dir / "workflow.yaml");
    if (!parsed.has_value()) {
        return std::unexpected("定义读不动: " +
                               (parsed.error().empty() ? std::string("?") : parsed.error()[0].message));
    }
    WorkflowDefinition def = std::move(*parsed);
    def.enabled = enabled;
    def.normalized = BuildNormalizedJson(def);
    std::string error;
    if (!WriteFileAtomically(workflow_dir / "workflow.yaml", EmitWorkflowYaml(def), error)) {
        return std::unexpected(error);
    }
    return {};
}

std::expected<void, std::string> RemoveWorkflow(const std::filesystem::path& workflow_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(workflow_dir, ec)) {
        return std::unexpected("目录不存在: " + lubancode::platform::PathToUtf8(workflow_dir));
    }
    std::filesystem::remove_all(workflow_dir, ec);
    if (ec) {
        return std::unexpected("删不成: " + ec.message());
    }
    return {};
}

}  // namespace lubancode::workflow
