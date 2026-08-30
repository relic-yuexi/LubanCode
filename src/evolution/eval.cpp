// 评测与基线的实现(自进化闭环阶段 3)。全文只产结果与账面,不落盘:
// eval-results.jsonl 的追加与状态迁移唯一写口在 EvolutionCoordinator。
//
// 确定性口径(取舍记在案):
//   - 回放/留出不 起 真 模 型。任务的"执行"由夹具携带产物、检查器逐项验
//     收代跑;model-in-the-loop 与真实 agent 开销没测到,行行写 unverified。
//   - tool_calls 只数确定性检查里真起了的进程(command 项),tokens 恒 0
//     ——这是"确定性代跑"的账,不是模型回合的账,不冒充。
//   - 墙钟为检查执行的实测毫秒;workspace_writes 为检查前后快照对比出的
//     新增/改动文件数。
#include "evolution/eval.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#include "package/catalog.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::evolution {

namespace {

namespace fs = std::filesystem;

std::string IsoNowUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::optional<std::string> ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string GetString(const nlohmann::json& parent, const char* key) {
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

std::int64_t GetInt(const nlohmann::json& parent, const char* key, std::int64_t fallback = 0) {
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<std::int64_t>();
}

double GetDouble(const nlohmann::json& parent, const char* key, double fallback = 0.0) {
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<double>();
}

bool IsWordChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

// 按空白拆 argv(command 检查不经 shell 拼串,安全铁律;引号语法首版不认,
// 复杂命令自己包一层脚本)。
std::vector<std::string> SplitArgv(const std::string& command) {
    std::vector<std::string> argv;
    std::istringstream stream(command);
    std::string word;
    while (stream >> word) {
        argv.push_back(word);
    }
    return argv;
}

// workspace 快照:相对路径 -> (大小, mtime)。检查前后各拍一张,对出写入数。
struct FileStamp {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
};
std::map<std::string, FileStamp> SnapshotWorkspace(const fs::path& workspace) {
    std::map<std::string, FileStamp> out;
    if (workspace.empty()) {
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(workspace, ec) || ec) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(workspace, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        std::string rel = lubancode::platform::PathToUtf8(it->path().lexically_relative(workspace));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        FileStamp stamp;
        stamp.size = it->file_size(ec);
        stamp.mtime = static_cast<std::int64_t>(it->last_write_time(ec).time_since_epoch().count());
        out.emplace(std::move(rel), stamp);
    }
    return out;
}

void FillDelta(MetricDelta& delta, double candidate, double baseline, bool has_baseline) {
    delta.has_baseline = has_baseline;
    delta.candidate = candidate;
    delta.baseline = baseline;
    delta.delta = has_baseline ? candidate - baseline : 0.0;
    if (has_baseline && baseline > 0.0) {
        delta.delta_pct = static_cast<int>(std::lround((candidate - baseline) / baseline * 100.0));
    }
}

std::string FormatCount(std::int64_t value) {
    return std::to_string(value);
}

std::string FormatRate(double value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string FormatCost(const MetricDelta& delta, const char* name) {
    if (!delta.has_baseline) {
        return std::string(name) + " " + FormatCount(static_cast<std::int64_t>(delta.candidate)) + "(基线侧无账)";
    }
    std::string out = std::string(name) + " " + FormatCount(static_cast<std::int64_t>(delta.candidate)) +
                      " 对 " + FormatCount(static_cast<std::int64_t>(delta.baseline));
    if (delta.delta == 0) {
        out += "(持平)";
    } else {
        out += "(" + (delta.delta > 0 ? std::string("+") : std::string()) +
               FormatCount(static_cast<std::int64_t>(delta.delta));
        if (delta.baseline > 0) {
            out += "," + (delta.delta_pct > 0 ? std::string("+") : std::string()) +
                   std::to_string(delta.delta_pct) + "%";
        }
        out += ")";
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 验收检查项
// ---------------------------------------------------------------------------

std::string ToString(AcceptanceCheckKind kind) {
    switch (kind) {
        case AcceptanceCheckKind::Manual: return "manual";
        case AcceptanceCheckKind::FileExists: return "file_exists";
        case AcceptanceCheckKind::JsonParses: return "json_parses";
        case AcceptanceCheckKind::FileContains: return "file_contains";
        case AcceptanceCheckKind::Command: return "command";
    }
    return "manual";
}

std::optional<AcceptanceCheckKind> ParseAcceptanceCheckKind(const std::string& text) {
    static const std::pair<const char*, AcceptanceCheckKind> kTable[] = {
        {"manual", AcceptanceCheckKind::Manual},
        {"file_exists", AcceptanceCheckKind::FileExists},
        {"json_parses", AcceptanceCheckKind::JsonParses},
        {"file_contains", AcceptanceCheckKind::FileContains},
        {"command", AcceptanceCheckKind::Command},
    };
    for (const auto& [name, kind] : kTable) {
        if (text == name) {
            return kind;
        }
    }
    return std::nullopt;
}

namespace {

// 一个 acceptance 元素 -> 检查项。字符串 = 人工描述;对象 = 可执行检查。
AcceptanceCheck ParseAcceptanceItem(const nlohmann::json& item, std::string* error) {
    AcceptanceCheck check;
    if (item.is_string()) {
        check.kind = AcceptanceCheckKind::Manual;
        check.raw = item.get<std::string>();
        return check;
    }
    if (!item.is_object()) {
        if (error != nullptr && error->empty()) {
            *error = "acceptance 元素既不是字符串也不是对象";
        }
        return check;
    }
    const std::string kind_text = GetString(item, "kind");
    const auto kind = ParseAcceptanceCheckKind(kind_text);
    if (!kind.has_value() || *kind == AcceptanceCheckKind::Manual) {
        if (error != nullptr && error->empty()) {
            *error = "认不得的验收 kind: \"" + kind_text + "\"";
        }
        return check;
    }
    check.kind = *kind;
    check.path = GetString(item, "path");
    check.text = GetString(item, "text");
    check.command = GetString(item, "command");
    if (check.kind == AcceptanceCheckKind::Command) {
        if (check.command.empty()) {
            if (error != nullptr && error->empty()) {
                *error = "command 检查缺 command 字段";
            }
            return AcceptanceCheck{};  // 坏项按 manual 空
        }
        check.raw = check.command;
    } else {
        if (check.path.empty()) {
            if (error != nullptr && error->empty()) {
                *error = ToString(check.kind) + " 检查缺 path 字段";
            }
            return AcceptanceCheck{};
        }
        check.raw = check.path + (check.text.empty() ? "" : " 含 \"" + check.text + "\"");
    }
    const std::string note = GetString(item, "note");
    if (!note.empty()) {
        check.raw = note + "(" + check.raw + ")";
    }
    return check;
}

EvalTask ParseEvalTask(const nlohmann::json& obj, const char* id_key, std::string* error) {
    EvalTask task;
    task.task_id = GetString(obj, id_key);
    task.task = GetString(obj, "task");
    task.workspace = GetString(obj, "workspace");
    if (task.task_id.empty()) {
        if (error != nullptr && error->empty()) {
            *error = std::string("任务缺 ") + id_key + " 字段";
        }
    }
    const auto acceptance = obj.find("acceptance");
    if (acceptance != obj.end() && acceptance->is_array()) {
        for (const auto& item : *acceptance) {
            task.acceptance.push_back(ParseAcceptanceItem(item, error));
        }
    }
    return task;
}

}  // namespace

std::expected<EvalPlan, std::string> ParseEvalPlan(const std::string& text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(std::string("eval-plan.json 不是合法 JSON: ") + e.what());
    }
    if (!root.is_object()) {
        return std::unexpected("eval-plan.json 不是 JSON 对象");
    }
    if (!root.contains("schema") || !root["schema"].is_number_integer() ||
        root["schema"].get<int>() != 1) {
        return std::unexpected("eval-plan.json 的 schema 只认 1");
    }
    EvalPlan plan;
    plan.candidate_id = GetString(root, "candidate_id");
    plan.content_hash = GetString(root, "content_hash");
    if (plan.candidate_id.empty() || plan.content_hash.empty()) {
        return std::unexpected("eval-plan.json 缺 candidate_id 或 content_hash(绑定候选的硬字段)");
    }
    std::string error;
    if (const auto replay = root.find("replay"); replay != root.end() && replay->is_array()) {
        for (const auto& item : *replay) {
            if (item.is_object()) {
                // replay 行的 id 字段叫 source_id(指回来源),归一进 task_id。
                plan.replay.push_back(ParseEvalTask(item, "source_id", &error));
            }
        }
    }
    if (const auto holdout = root.find("holdout"); holdout != root.end() && holdout->is_array()) {
        for (const auto& item : *holdout) {
            if (item.is_object()) {
                plan.holdout.push_back(ParseEvalTask(item, "task_id", &error));
            }
        }
    }
    if (const auto baseline = root.find("baseline"); baseline != root.end() && baseline->is_object()) {
        plan.baseline_kind = GetString(*baseline, "kind");
        plan.baseline_ref = GetString(*baseline, "ref");
        plan.baseline_fixture = GetString(*baseline, "fixture");
        if (const auto metrics = baseline->find("metrics"); metrics != baseline->end() && metrics->is_array()) {
            for (const auto& metric : *metrics) {
                if (metric.is_string()) {
                    plan.baseline_metrics.push_back(metric.get<std::string>());
                }
            }
        }
    } else {
        error = "eval-plan.json 缺 baseline 节";
    }
    if (const auto budget = root.find("budget"); budget != root.end() && budget->is_object()) {
        plan.budget_max_tool_calls = GetInt(*budget, "max_tool_calls");
        plan.budget_max_tokens = GetInt(*budget, "max_tokens");
        plan.budget_timeout_ms = GetInt(*budget, "timeout_ms");
    }
    if (!error.empty()) {
        return std::unexpected("eval-plan.json 有问题: " + error);
    }
    return plan;
}

// ---------------------------------------------------------------------------
// 指标账
// ---------------------------------------------------------------------------

nlohmann::json EvalMetrics::ToJson() const {
    nlohmann::json out;
    out["success_rate"] = success_rate;
    out["acceptance_rate"] = acceptance_rate;
    out["tool_calls"] = tool_calls;
    out["tokens"] = tokens;
    out["wall_clock_ms"] = wall_clock_ms;
    out["permission_prompts"] = permission_prompts;
    out["workspace_writes"] = workspace_writes;
    return out;
}

EvalMetrics EvalMetrics::FromJson(const nlohmann::json& json) {
    EvalMetrics metrics;
    if (!json.is_object()) {
        return metrics;
    }
    metrics.success_rate = GetDouble(json, "success_rate");
    metrics.acceptance_rate = GetDouble(json, "acceptance_rate");
    metrics.tool_calls = GetInt(json, "tool_calls");
    metrics.tokens = GetInt(json, "tokens");
    metrics.wall_clock_ms = GetInt(json, "wall_clock_ms");
    metrics.permission_prompts = GetInt(json, "permission_prompts");
    metrics.workspace_writes = GetInt(json, "workspace_writes");
    return metrics;
}

// ---------------------------------------------------------------------------
// 检查/扫描的序列化
// ---------------------------------------------------------------------------

nlohmann::json CheckResult::ToJson() const {
    nlohmann::json out;
    out["kind"] = kind;
    out["detail"] = detail;
    out["outcome"] = skipped ? "skipped" : (pass ? "pass" : "fail");
    if (!note.empty()) {
        out["note"] = note;
    }
    return out;
}

std::optional<CheckResult> CheckResult::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    CheckResult out;
    out.kind = GetString(json, "kind");
    out.detail = GetString(json, "detail");
    const std::string outcome = GetString(json, "outcome");
    out.pass = outcome == "pass";
    out.skipped = outcome == "skipped";
    out.note = GetString(json, "note");
    if (out.kind.empty()) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 复杂度代价(阶段 5):组合包比最小 Skill 包多出的组件数与维护面
// ---------------------------------------------------------------------------

nlohmann::json ComplexityCost::ToJson() const {
    nlohmann::json out;
    out["shape"] = shape;
    out["has_workflow"] = has_workflow;
    out["has_agent"] = has_agent;
    out["has_plugin"] = has_plugin;
    out["components"] = components;
    out["minimal_components"] = minimal_components;
    out["extra_components"] = extra_components;
    out["files"] = files;
    out["minimal_files"] = minimal_files;
    out["extra_files"] = extra_files;
    return out;
}

std::optional<ComplexityCost> ComplexityCost::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ComplexityCost cost;
    cost.shape = GetString(json, "shape");
    if (cost.shape.empty()) {
        return std::nullopt;
    }
    cost.has_workflow = json.value("has_workflow", false);
    cost.has_agent = json.value("has_agent", false);
    cost.has_plugin = json.value("has_plugin", false);
    cost.components = static_cast<int>(GetInt(json, "components"));
    cost.minimal_components = static_cast<int>(GetInt(json, "minimal_components"));
    cost.extra_components = static_cast<int>(GetInt(json, "extra_components"));
    cost.files = static_cast<int>(GetInt(json, "files"));
    cost.minimal_files = static_cast<int>(GetInt(json, "minimal_files"));
    cost.extra_files = static_cast<int>(GetInt(json, "extra_files"));
    return cost;
}

std::string ComplexityCost::SummaryLine() const {
    if (shape.empty()) {
        return std::string();
    }
    if (shape == "skill-only") {
        return "最小可行包(Skill-only," + std::to_string(components) + " 件组件," +
               std::to_string(files) + " 个文件;与最小档持平)";
    }
    if (shape == "code-draft") {
        return "代码候选草稿(Skill + process Plugin 草稿," + std::to_string(components) +
               " 件组件、" + std::to_string(files) +
               " 个文件;零执行零挂载,指路 Package trust 人工审查)";
    }
    std::string line = "组合包(";
    line += has_workflow ? "带 workflow" : "无 workflow";
    line += has_agent ? "+agent" : "";
    line += "):" + std::to_string(components) + " 件组件、" + std::to_string(files) +
            " 个文件,比最小 Skill 包多 " + std::to_string(extra_components) + " 件组件、" +
            std::to_string(extra_files) + " 个文件要维护";
    return line;
}

ComplexityCost ComputeComplexityCost(const fs::path& package_dir) {
    ComplexityCost cost;
    std::error_code ec;
    if (!std::filesystem::is_directory(package_dir, ec) || ec) {
        cost.shape = std::string();  // 读不动:不冒充算过
        return cost;
    }
    int skills = 0;
    int workflows = 0;
    int agents = 0;
    int plugins = 0;
    int files = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(package_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        ++files;
        std::string rel = lubancode::platform::PathToUtf8(it->path().lexically_relative(package_dir));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (rel == "package.yaml") {
            continue;
        }
        if (rel.rfind("skills/", 0) == 0 && rel.size() >= 17 &&
            rel.find('/', 7) == rel.size() - 9 &&
            rel.compare(rel.size() - 9, 9, "/SKILL.md") == 0) {
            ++skills;  // skills/<id>/SKILL.md(目录名单段;最短 skills/a/SKILL.md 17 字符)
        } else if (rel.rfind("workflows/", 0) == 0 && rel.size() > 22 &&
                   rel.substr(rel.size() - 14) == "/workflow.yaml" &&
                   std::count(rel.begin(), rel.end(), '/') == 2) {
            ++workflows;
        } else if (rel.rfind("agents/", 0) == 0 && rel.size() > 12 &&
                   rel.find('/', 7) == std::string::npos && rel.substr(rel.size() - 5) == ".yaml") {
            ++agents;
        } else if (rel.rfind("plugins/", 0) == 0 && rel.size() > 19 &&
                   rel.compare(rel.size() - 11, 11, "plugin.json") == 0 &&
                   std::count(rel.begin(), rel.end(), '/') == 2) {
            ++plugins;  // plugins/<id>/plugin.json(阶段 6 草稿一件)
        }
    }
    cost.has_workflow = workflows > 0;
    cost.has_agent = agents > 0;
    cost.has_plugin = plugins > 0;
    cost.components = skills + workflows + agents + plugins;
    cost.extra_components = std::max(0, cost.components - cost.minimal_components);
    cost.files = files;
    cost.extra_files = std::max(0, files - cost.minimal_files);
    cost.shape = plugins > 0 ? "code-draft"
                             : ((workflows > 0 || agents > 0) ? "combination" : "skill-only");
    return cost;
}

nlohmann::json ScanFinding::ToJson() const {
    nlohmann::json out;
    out["kind"] = kind;
    out["path"] = path;
    out["line"] = line;
    out["detail"] = detail;
    return out;
}

// ---------------------------------------------------------------------------
// eval-results.jsonl
// ---------------------------------------------------------------------------

nlohmann::json EvalResultLine::ToJson() const {
    nlohmann::json out;
    out["schema"] = schema;
    out["seq"] = seq;
    out["gate"] = gate;
    out["task_id"] = task_id;
    out["candidate_id"] = candidate_id;
    out["content_hash"] = content_hash;
    if (gate == "baseline") {
        out["baseline_ref"] = baseline_ref;
    }
    out["outcome"] = outcome;
    out["metrics"] = metrics.ToJson();
    out["unverified"] = unverified;
    if (!verdict.empty()) {
        out["verdict"] = verdict;
    }
    out["recorded_at"] = recorded_at;
    if (!checks.empty()) {
        nlohmann::json items = nlohmann::json::array();
        for (const CheckResult& check : checks) {
            items.push_back(check.ToJson());
        }
        out["checks"] = items;
    }
    if (!findings.empty()) {
        nlohmann::json items = nlohmann::json::array();
        for (const ScanFinding& finding : findings) {
            items.push_back(finding.ToJson());
        }
        out["findings"] = items;
    }
    if (!notes.empty()) {
        out["notes"] = notes;
    }
    if (complexity.has_value()) {
        out["complexity"] = complexity->ToJson();
    }
    return out;
}

std::optional<EvalResultLine> EvalResultLine::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    if (!json.contains("schema") || !json["schema"].is_number_integer() ||
        json["schema"].get<int>() != 1) {
        return std::nullopt;
    }
    EvalResultLine line;
    line.seq = GetInt(json, "seq");
    line.gate = GetString(json, "gate");
    line.task_id = GetString(json, "task_id");
    line.candidate_id = GetString(json, "candidate_id");
    line.content_hash = GetString(json, "content_hash");
    line.outcome = GetString(json, "outcome");
    line.metrics = EvalMetrics::FromJson(json.value("metrics", nlohmann::json::object()));
    line.baseline_ref = GetString(json, "baseline_ref");
    line.verdict = GetString(json, "verdict");
    line.recorded_at = GetString(json, "recorded_at");
    if (const auto it = json.find("unverified"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) {
                line.unverified.push_back(item.get<std::string>());
            }
        }
    }
    if (const auto it = json.find("notes"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) {
                line.notes.push_back(item.get<std::string>());
            }
        }
    }
    if (const auto it = json.find("checks"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (auto check = CheckResult::FromJson(item); check.has_value()) {
                line.checks.push_back(std::move(*check));
            }
        }
    }
    if (const auto it = json.find("findings"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) {
                continue;
            }
            ScanFinding finding;
            finding.kind = GetString(item, "kind");
            finding.path = GetString(item, "path");
            finding.line = static_cast<int>(GetInt(item, "line"));
            finding.detail = GetString(item, "detail");
            if (!finding.kind.empty()) {
                line.findings.push_back(std::move(finding));
            }
        }
    }
    if (const auto it = json.find("complexity"); it != json.end() && it->is_object()) {
        line.complexity = ComplexityCost::FromJson(*it);
    }
    if (line.gate.empty() || line.outcome.empty()) {
        return std::nullopt;
    }
    return line;
}

std::string SerializeEvalResultLine(const EvalResultLine& line) {
    return line.ToJson().dump();
}

std::optional<EvalResultLine> ParseEvalResultLine(const std::string& text) {
    try {
        return EvalResultLine::FromJson(nlohmann::json::parse(text));
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::vector<EvalResultLine> LoadEvalResults(const fs::path& results_file) {
    std::vector<EvalResultLine> out;
    const auto text = ReadTextFile(results_file);
    if (!text.has_value()) {
        return out;
    }
    std::istringstream stream(*text);
    std::string line_text;
    while (std::getline(stream, line_text)) {
        if (!line_text.empty() && line_text.back() == '\r') {
            line_text.pop_back();
        }
        if (line_text.empty()) {
            continue;
        }
        if (auto line = ParseEvalResultLine(line_text); line.has_value()) {
            out.push_back(std::move(*line));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 密钥扫描与绝对路径扫描(纯函数,逐行报行号)
// ---------------------------------------------------------------------------

bool LooksBinary(const std::string& text) {
    const std::size_t probe = std::min<std::size_t>(text.size(), 512);
    for (std::size_t i = 0; i < probe; ++i) {
        if (text[i] == '\0') {
            return true;
        }
    }
    return false;
}

namespace {

// 与 skills::RedactSecrets 同一张关键词表(最长优先),这里是"找出来"而不是
// "掩掉":键形态后跟非占位值即命中。占位值({{…}}/<…>/$ {…}/[已打码]/
// null/…)不算——那是抽象过的写法,恰是起草器该产出的样子。
constexpr const char* kSecretKeyWords[] = {
    "authorization", "client_secret", "private_key", "access_key", "session_key",
    "api_key",       "apikey",        "password",    "passwd",     "cookie",
    "token",         "secret",
};

bool IsPlaceholderValue(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    if (value == "[已打码]" || value == "\"[已打码]\"" || value == "[REDACTED]" ||
        value == "\"[REDACTED]\"") {
        return true;
    }
    if (value.front() == '{' && value.back() == '}' && value.size() >= 4) {
        return true;  // {{token}} / {token}
    }
    if (value.front() == '<' && value.back() == '>' && value.size() >= 2) {
        return true;  // <token>
    }
    if (value.front() == '$' && value.back() == '}' && value.size() >= 4) {
        return true;  // ${TOKEN}
    }
    if (value == "..." || value == "…" || value == "null" || value == "None" ||
        value == "true" || value == "false") {
        return true;
    }
    if (value.find("placeholder") != std::string::npos ||
        value.find("example") != std::string::npos) {
        return true;  // 文档样例值(xxx-placeholder、example-xxx),不是真钥匙
    }
    // 纯 x/X 占位(xxx、XXXX……)
    bool all_x = true;
    for (const char c : value) {
        if (c != 'x' && c != 'X') {
            all_x = false;
            break;
        }
    }
    if (all_x && value.size() >= 2) {
        return true;
    }
    return false;
}

// 值的取法:到空白为止的一段(引号原样留着,占位判断已把带引号的认了)。
std::string ValueRun(const std::string& text, std::size_t begin) {
    std::size_t end = begin;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' &&
           text[end] != '\n') {
        ++end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

std::vector<ScanFinding> ScanTextForSecrets(const std::string& text) {
    std::vector<ScanFinding> out;
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    while (std::getline(stream, line_text)) {
        ++line_number;
        std::string lower;
        lower.reserve(line_text.size());
        for (const char c : line_text) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        for (std::size_t i = 0; i < lower.size(); ++i) {
            // 1) sk- 起头的裸 key 形态(整段长Token)。
            if (lower.compare(i, 3, "sk-") == 0 && (i == 0 || !IsWordChar(lower[i - 1]))) {
                std::size_t end = i;
                while (end < lower.size() && (IsWordChar(lower[end]) || lower[end] == '-' || lower[end] == '.')) {
                    ++end;
                }
                if (end - i >= 12) {
                    ScanFinding finding;
                    finding.kind = "secret";
                    finding.line = line_number;
                    finding.detail = "裸 key 形态(sk- 起头," +
                                     std::to_string(end - i) + " 字符,值已隐去)";
                    out.push_back(std::move(finding));
                    i = end - 1;
                    continue;
                }
            }
            // 2) 键形态:关键词 + ':'/'=' + 非占位值。
            if (i != 0 && IsWordChar(lower[i - 1])) {
                continue;
            }
            for (const char* word : kSecretKeyWords) {
                const std::size_t word_len = std::strlen(word);
                if (lower.compare(i, word_len, word) != 0) {
                    continue;
                }
                std::size_t p = i + word_len;
                while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) {
                    ++p;
                }
                if (p >= lower.size() || (lower[p] != ':' && lower[p] != '=')) {
                    break;
                }
                ++p;
                while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) {
                    ++p;
                }
                // 值以 bearer 起头:真值在再后一段(与 RedactSecrets 的 bearer
                // 规矩对齐),跳过词再取。
                if (lower.compare(p, 6, "bearer") == 0) {
                    const std::size_t after = p + 6;
                    if (after < lower.size() && (lower[after] == ' ' || lower[after] == '\t')) {
                        p = after;
                        while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) {
                            ++p;
                        }
                    }
                }
                const std::string value = ValueRun(line_text, p);
                if (!IsPlaceholderValue(value)) {
                    ScanFinding finding;
                    finding.kind = "secret";
                    finding.line = line_number;
                    finding.detail = "密钥赋值(" + std::string(word) + ",值已隐去)";
                    out.push_back(std::move(finding));
                }
                break;  // 一行一个关键词只报一次
            }
        }
    }
    return out;
}

std::vector<ScanFinding> ScanTextForAbsolutePaths(const std::string& text) {
    std::vector<ScanFinding> out;
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    while (std::getline(stream, line_text)) {
        ++line_number;
        const auto push = [&](std::size_t at, const char* what) {
            // 样例截到空白,至多 48 字符;这是要改掉的病灶,亮出来才好修。
            std::size_t end = at;
            while (end < line_text.size() && line_text[end] != ' ' && line_text[end] != '\t' &&
                   line_text[end] != '\r' && line_text[end] != ')' && line_text[end] != '"') {
                ++end;
            }
            std::string sample = line_text.substr(at, std::min<std::size_t>(end - at, 48));
            ScanFinding finding;
            finding.kind = "absolute-path";
            finding.line = line_number;
            finding.detail = std::string(what) + " " + sample;
            out.push_back(std::move(finding));
        };
        for (std::size_t i = 0; i < line_text.size(); ++i) {
            // 盘符形态 C:\ 或 C:/(词边界:前一个字符不得是字词字符,防 http:// 里的 p:)
            if (i + 2 < line_text.size() &&
                std::isalpha(static_cast<unsigned char>(line_text[i])) != 0 &&
                line_text[i + 1] == ':' &&
                (line_text[i + 2] == '\\' || line_text[i + 2] == '/') &&
                (i == 0 || !IsWordChar(line_text[i - 1]))) {
                push(i, "盘符绝对路径");
                continue;
            }
            // UNC \\server
            if (line_text[i] == '\\' && i + 1 < line_text.size() && line_text[i + 1] == '\\' &&
                (i == 0 || line_text[i - 1] == ' ' || line_text[i - 1] == '\t' ||
                 line_text[i - 1] == '(' || line_text[i - 1] == '"')) {
                push(i, "UNC 路径");
                continue;
            }
            // POSIX 家目录
            if (line_text[i] == '/') {
                static constexpr const char* kPosixRoots[] = {"/home/", "/Users/", "/root/"};
                for (const char* root : kPosixRoots) {
                    if (line_text.compare(i, std::strlen(root), root) == 0) {
                        push(i, "POSIX 绝对路径");
                        break;
                    }
                }
                continue;
            }
            // ~/ 起头的家目录写法(~/.lubancode/...)
            if (line_text[i] == '~' && i + 1 < line_text.size() && line_text[i + 1] == '/' &&
                (i == 0 || line_text[i - 1] == ' ' || line_text[i - 1] == '\t' ||
                 line_text[i - 1] == '(' || line_text[i - 1] == '"')) {
                push(i, "家目录路径");
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 阶段 6 四类安全夹具(代码候选的静态门)
// ---------------------------------------------------------------------------

namespace {

// 一行文本的小写化(匹配用)。
std::string LowerCopy(const std::string& text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

}  // namespace

// 恶意脚本:毁盘、远程拉码执行、反弹 shell 一类的形状。命中即报——注释里
// 出现也拦(草稿里不该有这些字样;误伤的人工审查线自会放行,这道门宁紧
// 不松)。夹具与样张一律用无害的"假装恶意"(只写注释/死串)。
std::vector<ScanFinding> ScanTextForMaliciousScript(const std::string& text) {
    std::vector<ScanFinding> out;
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    while (std::getline(stream, line_text)) {
        ++line_number;
        const std::string lower = LowerCopy(line_text);
        const auto hit = [&](const char* detail) {
            ScanFinding finding;
            finding.kind = "malicious-script";
            finding.line = line_number;
            finding.detail = detail;
            out.push_back(std::move(finding));
        };
        // 1) 毁盘形状:rm -rf / 一类连根拔、Windows 的 del/format 全卷。
        if (lower.find("rm -rf /") != std::string::npos ||
            lower.find("rm -rf ~") != std::string::npos ||
            lower.find("rm -fr /") != std::string::npos ||
            lower.find("del /f /s /q") != std::string::npos ||
            lower.find("del /s /q") != std::string::npos ||
            lower.find("format c:") != std::string::npos) {
            hit("毁盘命令形状(值已隐去)");
            continue;
        }
        // 2) 远程拉码执行:下载器(curl/wget/iwr/Invoke-WebRequest)与
        //    "| sh"/"| bash" 同行——拉下来就喂 shell。
        const bool has_fetcher = lower.find("curl") != std::string::npos ||
                                 lower.find("wget") != std::string::npos ||
                                 lower.find("invoke-webrequest") != std::string::npos ||
                                 lower.find("iwr ") != std::string::npos;
        const bool feeds_shell = lower.find("| sh") != std::string::npos ||
                                 lower.find("|sh") != std::string::npos ||
                                 lower.find("| bash") != std::string::npos ||
                                 lower.find("|bash") != std::string::npos ||
                                 lower.find("| zsh") != std::string::npos;
        if (has_fetcher && feeds_shell) {
            hit("远程拉码直接喂 shell 的形状");
            continue;
        }
        // 3) 动态执行远文:PowerShell 的 Invoke-Expression / iex(。
        if (lower.find("invoke-expression") != std::string::npos ||
            lower.find("iex (") != std::string::npos || lower.find("iex(") != std::string::npos) {
            hit("动态执行远文的形状(Invoke-Expression/iex)");
            continue;
        }
        // 4) 反弹 shell 帗见姿势:/dev/tcp/、bash -i >&、nc -e。
        if (lower.find("/dev/tcp/") != std::string::npos ||
            lower.find("bash -i >&") != std::string::npos ||
            lower.find("nc -e ") != std::string::npos) {
            hit("反弹 shell 的形状");
            continue;
        }
    }
    return out;
}

// 依赖投毒:只扫依赖清单文件(按文件名认),注释行(#)跳过。非注册表
// 直链(git+/svn+/hg+/bzr+/http/ftp/file)与改信任源的 pip 开关
// (--index-url/--extra-index-url/--trusted-host)一律拦——草稿的依赖只许
// 从默认注册表来,别的来源交人工审查。
std::vector<ScanFinding> ScanTextForDependencyPoisoning(const std::string& rel_path,
                                                        const std::string& text) {
    std::vector<ScanFinding> out;
    // 文件名认依赖清单:requirements*.txt、pyproject.toml、package.json。
    std::string name = rel_path;
    if (const std::size_t slash = name.rfind('/'); slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const bool is_dep_manifest =
        (name.rfind("requirements", 0) == 0 && name.find(".txt") != std::string::npos) ||
        name == "pyproject.toml" || name == "package.json" || name == "constraints.txt";
    if (!is_dep_manifest) {
        return out;
    }
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    while (std::getline(stream, line_text)) {
        ++line_number;
        const std::size_t first = line_text.find_first_not_of(" \t\r");
        if (first == std::string::npos || line_text[first] == '#') {
            continue;  // 空行/注释:依赖清单自己的说明不拦
        }
        const std::string lower = LowerCopy(line_text);
        const auto hit = [&](const char* detail) {
            ScanFinding finding;
            finding.kind = "dependency-poisoning";
            finding.line = line_number;
            finding.detail = detail;
            out.push_back(std::move(finding));
        };
        if (lower.find("git+") != std::string::npos || lower.find("svn+") != std::string::npos ||
            lower.find("hg+") != std::string::npos || lower.find("bzr+") != std::string::npos) {
            hit("依赖来自版本库直链(不走注册表,须人工审查)");
            continue;
        }
        if (lower.find("http://") != std::string::npos || lower.find("ftp://") != std::string::npos ||
            lower.find("file:") != std::string::npos) {
            hit("依赖来自明文/本地直链(不走注册表,须人工审查)");
            continue;
        }
        if (lower.find("--index-url") != std::string::npos ||
            lower.find("--extra-index-url") != std::string::npos ||
            lower.find("--trusted-host") != std::string::npos) {
            hit("依赖安装改了信任源(pip 开关,须人工审查)");
            continue;
        }
    }
    return out;
}

// 路径逃逸:路径段里的 ..(前有 / \ " 或行首、后有 / \ " 或行尾才算整段,
// 省略号与正文里的两个点不冤枉),另认 ${plugin_dir}/.. 形态。草稿的路径
// 一律钉在包根里。
std::vector<ScanFinding> ScanTextForPathEscape(const std::string& text) {
    std::vector<ScanFinding> out;
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    while (std::getline(stream, line_text)) {
        ++line_number;
        for (std::size_t i = 0; i + 1 < line_text.size(); ++i) {
            if (line_text[i] != '.' || line_text[i + 1] != '.') {
                continue;
            }
            const bool head_ok = i == 0 || line_text[i - 1] == '/' || line_text[i - 1] == '\\' ||
                                 line_text[i - 1] == '"' || line_text[i - 1] == '\'';
            const std::size_t after = i + 2;
            const bool tail_ok = after == line_text.size() || line_text[after] == '/' ||
                                 line_text[after] == '\\' || line_text[after] == '"' ||
                                 line_text[after] == '\'';
            if (!head_ok || !tail_ok) {
                continue;  // 不是整段 ..(省略号/正文两个点),不冤枉
            }
            ScanFinding finding;
            finding.kind = "path-escape";
            finding.line = line_number;
            finding.detail = "路径段里的 ..(伸出包根的逃逸形状)";
            out.push_back(std::move(finding));
            break;  // 一行报一次
        }
        if (line_text.find("${plugin_dir}/..") != std::string::npos) {
            ScanFinding finding;
            finding.kind = "path-escape";
            finding.line = line_number;
            finding.detail = "${plugin_dir} 后跟 ..(插件目录逃逸形状)";
            out.push_back(std::move(finding));
        }
    }
    return out;
}

// 代码文件的网络原语:python/node/lua/shell 常见的取网姿势。命中只说明
// "代码想用网",准不准由包级对账(ScanPackageNetworkOverreach)说了算。
std::vector<ScanFinding> ScanCodeForNetworkUse(const std::string& text) {
    std::vector<ScanFinding> out;
    std::istringstream stream(text);
    std::string line_text;
    int line_number = 0;
    static constexpr const char* kPrimitives[] = {
        "urllib",     "requests.",    "http.client", "httpx",
        "socket.",    "aiohttp",      "fetch(",      "axios",
        "net.Socket", "http.request", "http.get",    "http.post",
        "curl ",      "wget ",        "wget\t",      "resty.http",
        "lua-socket", "nc ",          "nc\t",
    };
    while (std::getline(stream, line_text)) {
        ++line_number;
        for (const char* primitive : kPrimitives) {
            if (line_text.find(primitive) != std::string::npos) {
                ScanFinding finding;
                finding.kind = "network-overreach";
                finding.line = line_number;
                finding.detail = std::string("代码带网络原语(") + primitive + ")";
                out.push_back(std::move(finding));
                break;  // 一行报一次
            }
        }
    }
    return out;
}

// 包级网络对账:清单(plugin.json/mcp.yaml 的网络声明)对代码(网络原语)。
namespace {

// 一只包的网络立场:包内全部清单并起来的最宽面。
//   Denied  全部清单都未许(或包内没有清单);
//   Broad   有清单布尔放行(network: true 的宽授权);
//   Precise 有清单落了精确声明(v2 network[] 数组)。
enum class NetworkStance { Denied, Broad, Precise };

NetworkStance StanceOfManifestText(const std::string& text, bool is_json) {
    // plugin.json 是 JSON,走 nlohmann;mcp.yaml 走行扫(permissions 段的
    // network 行)。两处都只问一件事:网络是关、是布尔开、还是精确开。
    if (is_json) {
        const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("permissions") &&
            parsed.at("permissions").is_object()) {
            const nlohmann::json& perms = parsed.at("permissions");
            if (perms.contains("network") && !perms.at("network").is_null()) {
                if (perms.at("network").is_boolean()) {
                    return perms.at("network").get<bool>() ? NetworkStance::Broad
                                                           : NetworkStance::Denied;
                }
                if (perms.at("network").is_array()) {
                    return NetworkStance::Precise;
                }
            }
        }
        return NetworkStance::Denied;
    }
    // YAML:认 permissions 段之后的 network: 行(v1 布尔;数组起头即精确)。
    std::istringstream stream(text);
    std::string line_text;
    bool in_permissions = false;
    while (std::getline(stream, line_text)) {
        if (line_text.rfind("permissions:", 0) == 0) {
            in_permissions = true;
            continue;
        }
        if (!line_text.empty() && line_text[0] != ' ' && line_text[0] != '\t' &&
            line_text.rfind("permissions:", 0) != 0) {
            in_permissions = false;  // 出了段
        }
        if (!in_permissions) {
            continue;
        }
        const std::size_t at = line_text.find("network:");
        if (at == std::string::npos) {
            continue;
        }
        std::string value = line_text.substr(at + 8);
        const std::size_t first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? "" : value.substr(first);
        if (value.rfind("true", 0) == 0) {
            return NetworkStance::Broad;
        }
        if (value.rfind('[', 0) == 0 || value.rfind("-", 0) == 0) {
            return NetworkStance::Precise;
        }
        return NetworkStance::Denied;
    }
    return NetworkStance::Denied;
}

// 代码文件后缀(网络原语只扫代码;SKILL/workflow 文本里的"curl"是文档)。
bool IsCodeFile(const std::string& rel_path) {
    static constexpr const char* kCodeExts[] = {".py", ".js",   ".mjs", ".ts",
                                                ".lua", ".sh",  ".bash", ".ps1",
                                                ".rb",  ".pl"};
    for (const char* ext : kCodeExts) {
        if (rel_path.size() >= std::strlen(ext) &&
            rel_path.compare(rel_path.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<ScanFinding> ScanPackageNetworkOverreach(const fs::path& package_dir) {
    std::vector<ScanFinding> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(package_dir, ec) || ec) {
        return out;
    }
    NetworkStance stance = NetworkStance::Denied;  // 没有清单 = 未许
    bool saw_manifest = false;
    std::vector<std::pair<std::string, std::string>> code_files;  // (rel, text)
    for (auto it = std::filesystem::recursive_directory_iterator(package_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        std::string rel = lubancode::platform::PathToUtf8(it->path().lexically_relative(package_dir));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const std::string name = rel.rfind('/') == std::string::npos
                                     ? rel
                                     : rel.substr(rel.rfind('/') + 1);
        if (name != "plugin.json" && name != "mcp.yaml") {
            if (IsCodeFile(rel)) {
                if (const auto text = ReadTextFile(it->path()); text.has_value()) {
                    code_files.emplace_back(rel, std::move(*text));
                }
            }
            continue;
        }
        saw_manifest = true;
        const auto text = ReadTextFile(it->path());
        if (!text.has_value()) {
            continue;
        }
        const NetworkStance here = StanceOfManifestText(*text, name == "plugin.json");
        if (here == NetworkStance::Broad || (here == NetworkStance::Precise &&
                                             stance != NetworkStance::Broad)) {
            stance = here == NetworkStance::Broad ? NetworkStance::Broad : NetworkStance::Precise;
        }
    }
    // 1) 宽授权:布尔放行本身即越权形状(草稿须落精确声明)。
    if (saw_manifest && stance == NetworkStance::Broad) {
        ScanFinding finding;
        finding.kind = "network-overreach";
        finding.path = "(manifest)";
        finding.detail = "清单布尔放行 network: true(宽授权)——草稿须落精确网络"
                         "声明,交人工审查";
        out.push_back(std::move(finding));
    }
    // 2) 代码用网,清单未许(或包内没有清单)。
    if (stance != NetworkStance::Precise) {
        for (const auto& [rel, text] : code_files) {
            for (ScanFinding finding : ScanCodeForNetworkUse(text)) {
                finding.path = rel;
                finding.detail = "代码用网而清单未许:" + finding.detail;
                out.push_back(std::move(finding));
            }
        }
        return out;
    }
    // 3) 精确声明之下:明文 http:// 取数仍是越权(只许 https)。
    for (const auto& [rel, text] : code_files) {
        std::istringstream stream(text);
        std::string line_text;
        int line_number = 0;
        while (std::getline(stream, line_text)) {
            ++line_number;
            if (line_text.find("http://") == std::string::npos) {
                continue;
            }
            ScanFinding finding;
            finding.kind = "network-overreach";
            finding.path = rel;
            finding.line = line_number;
            finding.detail = "明文 http:// 取数(精确声明也只许 https)";
            out.push_back(std::move(finding));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 静态门:Package doctor + 两道扫描
// ---------------------------------------------------------------------------

StaticGateResult RunStaticGate(const fs::path& package_dir) {
    StaticGateResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(package_dir, ec) || ec) {
        result.doctor_valid = false;
        result.diagnostics_errors = 1;
        result.errors.push_back("候选缺 package/ 目录: " +
                                lubancode::platform::PathToUtf8(package_dir));
        return result;
    }

    // ---- Package doctor(AnalyzePackage,不另写一套)----
    package::PackageCandidate candidate;
    candidate.scope = package::PackageScope::Dev;  // 直诊不参与遮蔽
    candidate.layer_root = package_dir.parent_path();
    candidate.package_root = package_dir;
    candidate.dir_name = lubancode::platform::PathToUtf8(package_dir.filename());
    const package::PackageRecord record =
        package::AnalyzePackage(candidate, package::ScanOptions{}, package::PackageRefIndex{}, {});

    result.doctor_valid = record.valid;
    result.diagnostics_errors = 0;
    for (const package::PackageDiagnostic& diagnostic : record.inventory.diagnostics) {
        if (diagnostic.kind == package::PackageDiagnostic::Kind::Error) {
            ++result.diagnostics_errors;
            if (result.errors.size() < 20) {
                result.errors.push_back(diagnostic.Format());
            }
        } else if (diagnostic.kind == package::PackageDiagnostic::Kind::Warning) {
            ++result.diagnostics_warnings;
        }
    }
    result.components_total = static_cast<int>(record.components.size());
    for (const package::ParsedComponent& component : record.components) {
        if (component.ok && !component.HasError()) {
            ++result.components_ok;
        } else if (result.errors.size() < 20) {
            std::string message = component.canonical_id + " (" + component.rel_path + "): ";
            message += component.issues.empty() ? "原生 parser 判坏"
                                                : component.issues.front().Format();
            result.errors.push_back(std::move(message));
        }
    }
    if (!record.valid && result.errors.empty()) {
        result.errors.push_back("doctor 判 invalid(引用不闭合或清单问题)");
    }

    // ---- 密钥扫描 + 绝对路径扫描:候选包全文(含 SKILL 正文)----
    // ---- 阶段 6 四类安全夹具:恶意脚本/依赖投毒/路径逃逸逐文件;网络
    //      越权是清单与代码的对账,包级跑一遍。----
    for (auto it = std::filesystem::recursive_directory_iterator(package_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        std::string rel = lubancode::platform::PathToUtf8(it->path().lexically_relative(package_dir));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const auto text = ReadTextFile(it->path());
        if (!text.has_value() || LooksBinary(*text)) {
            continue;  // 读不动/二进制:跳过扫描(doctor 那边另有账)
        }
        for (ScanFinding finding : ScanTextForSecrets(*text)) {
            finding.path = rel;
            result.findings.push_back(std::move(finding));
        }
        for (ScanFinding finding : ScanTextForAbsolutePaths(*text)) {
            finding.path = rel;
            result.findings.push_back(std::move(finding));
        }
        for (ScanFinding finding : ScanTextForMaliciousScript(*text)) {
            finding.path = rel;
            result.findings.push_back(std::move(finding));
        }
        for (ScanFinding finding : ScanTextForDependencyPoisoning(rel, *text)) {
            finding.path = rel;
            result.findings.push_back(std::move(finding));
        }
        for (ScanFinding finding : ScanTextForPathEscape(*text)) {
            finding.path = rel;
            result.findings.push_back(std::move(finding));
        }
    }
    for (ScanFinding finding : ScanPackageNetworkOverreach(package_dir)) {
        result.findings.push_back(std::move(finding));
    }
    return result;
}

// ---------------------------------------------------------------------------
// 一次任务门的确定性执行
// ---------------------------------------------------------------------------

TaskRunResult RunEvalTask(const std::string& gate, const EvalTask& task,
                          const std::string& candidate_id, const std::string& content_hash,
                          const fs::path& candidate_dir, const EvalPlan& plan) {
    TaskRunResult run;
    EvalResultLine& line = run.line;
    line.gate = gate;
    line.task_id = task.task_id;
    line.candidate_id = candidate_id;
    line.content_hash = content_hash;

    fs::path workspace;
    if (!task.workspace.empty()) {
        workspace = candidate_dir / lubancode::platform::Utf8ToPath(task.workspace);
        std::error_code ec;
        if (!std::filesystem::is_directory(workspace, ec) || ec) {
            run.fixture_missing = true;
            workspace.clear();
        }
    }
    const std::map<std::string, FileStamp> before = SnapshotWorkspace(workspace);
    const auto started = std::chrono::steady_clock::now();

    int executable = 0;
    int passed = 0;
    int manual = 0;
    std::int64_t tool_calls = 0;

    for (const AcceptanceCheck& check : task.acceptance) {
        CheckResult result;
        result.kind = ToString(check.kind);
        result.detail = check.raw;
        // 任务没给 workspace:文件与命令检查无处落——评测的规矩是隔离工作
        // 区,不在评测进程的当前目录里乱翻。没测,如实 skipped。
        if (workspace.empty() && check.kind != AcceptanceCheckKind::Manual) {
            result.skipped = true;
            result.note = "任务未给 workspace,该项无处落(没测,不冒充)";
            line.checks.push_back(std::move(result));
            continue;
        }
        switch (check.kind) {
            case AcceptanceCheckKind::Manual:
                result.skipped = true;
                result.note = "人工验收,确定性评测判不了";
                ++manual;
                break;
            case AcceptanceCheckKind::FileExists: {
                ++executable;
                std::error_code ec;
                const fs::path target = workspace / lubancode::platform::Utf8ToPath(check.path);
                if (std::filesystem::is_regular_file(target, ec) && !ec) {
                    result.pass = true;
                } else {
                    result.note = "文件不存在: " + check.path;
                }
                break;
            }
            case AcceptanceCheckKind::JsonParses: {
                ++executable;
                const auto text = ReadTextFile(workspace /
                                               lubancode::platform::Utf8ToPath(check.path));
                if (!text.has_value()) {
                    result.note = "读不到文件: " + check.path;
                    break;
                }
                try {
                    const nlohmann::json parsed = nlohmann::json::parse(*text);
                    result.pass = parsed.is_object() || parsed.is_array();
                    if (!result.pass) {
                        result.note = "JSON 顶层不是对象/数组";
                    }
                } catch (const nlohmann::json::exception& e) {
                    result.note = std::string("JSON 解析失败: ") + e.what();
                }
                break;
            }
            case AcceptanceCheckKind::FileContains: {
                ++executable;
                const auto text = ReadTextFile(workspace /
                                               lubancode::platform::Utf8ToPath(check.path));
                if (!text.has_value()) {
                    result.note = "读不到文件: " + check.path;
                    break;
                }
                if (text->find(check.text) != std::string::npos) {
                    result.pass = true;
                } else {
                    result.note = "正文不含 \"" + check.text + "\"";
                }
                break;
            }
            case AcceptanceCheckKind::Command: {
                ++executable;
                ++tool_calls;  // 真起了进程才算 tool call,起了失败也算
                const std::vector<std::string> argv = SplitArgv(check.command);
                const int timeout_ms =
                    plan.budget_timeout_ms > 0 ? static_cast<int>(plan.budget_timeout_ms) : 120000;
                const platform::ProcessResult process = platform::RunProcess(
                    argv, timeout_ms, nullptr, {}, platform::kDefaultMaxOutputBytes,
                    workspace.empty() ? std::string()
                                      : lubancode::platform::PathToUtf8(workspace));
                if (process.spawn_failed) {
                    result.note = "起不了进程(" + process.spawn_error + "): " + check.command;
                } else if (process.timed_out) {
                    result.note = "命令超时(" + std::to_string(timeout_ms) + "ms)";
                } else if (process.exit_code != 0) {
                    result.note = "退出码 " + std::to_string(process.exit_code);
                } else {
                    result.pass = true;
                }
                break;
            }
        }
        if (result.pass) {
            ++passed;
        }
        line.checks.push_back(std::move(result));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const std::map<std::string, FileStamp> after = SnapshotWorkspace(workspace);
    std::int64_t writes = 0;
    for (const auto& [rel, stamp] : after) {
        const auto old = before.find(rel);
        if (old == before.end() || old->second.size != stamp.size || old->second.mtime != stamp.mtime) {
            ++writes;
        }
    }

    line.metrics.tool_calls = tool_calls;
    line.metrics.tokens = 0;  // 确定性代跑不起模型,tokens 恒 0(unverified 有账)
    line.metrics.wall_clock_ms = elapsed;
    line.metrics.permission_prompts = 0;  // 非交互检查,零确认
    line.metrics.workspace_writes = writes;
    line.metrics.acceptance_rate = executable > 0 ? static_cast<double>(passed) / executable : 0.0;

    // ---- unverified:没测到的写明,不冒充 ----
    line.unverified.push_back("model-in-the-loop");
    line.unverified.push_back("agent-metrics");
    if (manual > 0) {
        line.unverified.push_back("manual-acceptance");
    }
    if (run.fixture_missing) {
        line.unverified.push_back("fixture-missing");
        line.notes.push_back("workspace 缺失: " + task.workspace + "(没测,不是测砸)");
        line.outcome = "skipped";
        line.metrics.success_rate = 0.0;
    } else if (executable == 0) {
        line.outcome = "skipped";  // 全是人工验收:如实记跳过
        line.metrics.success_rate = 0.0;
    } else if (passed == executable) {
        line.outcome = "pass";
        line.metrics.success_rate = 1.0;
    } else {
        line.outcome = "fail";
        line.metrics.success_rate = 0.0;
    }

    // ---- 预算:越帽即 fail(契约)----
    if (line.outcome == "pass") {
        if (plan.budget_max_tool_calls > 0 && tool_calls > plan.budget_max_tool_calls) {
            line.outcome = "fail";
            line.notes.push_back("tool calls " + std::to_string(tool_calls) + " 越帽 " +
                                 std::to_string(plan.budget_max_tool_calls));
        }
        if (plan.budget_max_tokens > 0 && line.metrics.tokens > plan.budget_max_tokens) {
            line.outcome = "fail";
            line.notes.push_back("tokens 越帽");
        }
        if (plan.budget_timeout_ms > 0 && elapsed > plan.budget_timeout_ms) {
            line.outcome = "fail";
            line.notes.push_back("墙钟 " + std::to_string(elapsed) + "ms 越帽 " +
                                 std::to_string(plan.budget_timeout_ms) + "ms");
        }
        if (line.outcome == "fail") {
            line.metrics.success_rate = 0.0;
        }
    }
    line.recorded_at = IsoNowUtc();
    return run;
}

// ---------------------------------------------------------------------------
// 基线夹具
// ---------------------------------------------------------------------------

std::optional<BaselineFixture> ParseBaselineFixture(const std::string& text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!root.is_object() || !root.contains("schema") || !root["schema"].is_number_integer() ||
        root["schema"].get<int>() != 1) {
        return std::nullopt;
    }
    BaselineFixture fixture;
    fixture.kind = GetString(root, "kind");
    fixture.ref = GetString(root, "ref");
    fixture.task_id = GetString(root, "task_id");
    const auto metrics = root.find("metrics");
    if (metrics != root.end() && metrics->is_object()) {
        fixture.metrics = EvalMetrics::FromJson(*metrics);
    } else {
        return std::nullopt;  // 基线没带指标账就不是一份可用夹具
    }
    if (fixture.kind.empty() || fixture.ref.empty()) {
        return std::nullopt;
    }
    if (const auto it = root.find("unverified"); it != root.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) {
                fixture.unverified.push_back(item.get<std::string>());
            }
        }
    }
    return fixture;
}

// ---------------------------------------------------------------------------
// 账面汇总
// ---------------------------------------------------------------------------

namespace {

void TallyLine(const EvalResultLine& line, GateTally& tally) {
    if (line.outcome == "pass") {
        ++tally.pass;
    } else if (line.outcome == "fail") {
        ++tally.fail;
    } else {
        ++tally.skipped;
    }
}

}  // namespace

EvalSummary SummarizeEvalLedger(const std::vector<EvalResultLine>& lines) {
    EvalSummary summary;
    summary.line_count = lines.size();

    double candidate_success_sum = 0.0;
    double candidate_acceptance_sum = 0.0;
    int candidate_rate_rows = 0;
    double baseline_success_sum = 0.0;
    double baseline_acceptance_sum = 0.0;
    int baseline_rate_rows = 0;
    std::int64_t candidate_tool_calls = 0;
    std::int64_t candidate_tokens = 0;
    std::int64_t candidate_wall = 0;
    std::int64_t candidate_prompts = 0;
    std::int64_t candidate_writes = 0;
    std::int64_t baseline_tool_calls = 0;
    std::int64_t baseline_tokens = 0;
    std::int64_t baseline_wall = 0;
    std::int64_t baseline_prompts = 0;
    std::int64_t baseline_writes = 0;
    bool has_baseline_rows = false;

    for (const EvalResultLine& line : lines) {
        if (line.gate == "static") {
            TallyLine(line, summary.static_gate);
            if (line.outcome == "pass") {
                ++summary.checks_passed;
            } else if (line.outcome == "fail") {
                ++summary.checks_failed;
            } else {
                ++summary.checks_skipped;
            }
            if (line.complexity.has_value()) {
                summary.complexity = line.complexity;  // 取最近一带账的静态行
            }
        } else if (line.gate == "replay" || line.gate == "holdout") {
            TallyLine(line, line.gate == "replay" ? summary.replay : summary.holdout);
            if (line.gate == "holdout") {
                summary.has_holdout = true;
            }
            candidate_tool_calls += line.metrics.tool_calls;
            candidate_tokens += line.metrics.tokens;
            candidate_wall += line.metrics.wall_clock_ms;
            candidate_prompts += line.metrics.permission_prompts;
            candidate_writes += line.metrics.workspace_writes;
            if (line.outcome != "skipped") {
                candidate_success_sum += line.metrics.success_rate;
                candidate_acceptance_sum += line.metrics.acceptance_rate;
                ++candidate_rate_rows;
            }
        } else if (line.gate == "baseline") {
            TallyLine(line, summary.baseline);
            has_baseline_rows = true;
            if (!line.baseline_ref.empty()) {
                summary.baseline_ref = line.baseline_ref;
            }
            if (line.outcome != "skipped") {
                summary.has_baseline_metrics = true;
                baseline_success_sum += line.metrics.success_rate;
                baseline_acceptance_sum += line.metrics.acceptance_rate;
                ++baseline_rate_rows;
            }
            baseline_tool_calls += line.metrics.tool_calls;
            baseline_tokens += line.metrics.tokens;
            baseline_wall += line.metrics.wall_clock_ms;
            baseline_prompts += line.metrics.permission_prompts;
            baseline_writes += line.metrics.workspace_writes;
        }
        for (const CheckResult& check : line.checks) {
            if (line.gate == "static") {
                continue;  // static 行没有 checks(发现走 findings)
            }
            if (check.skipped) {
                ++summary.checks_skipped;
            } else if (check.pass) {
                ++summary.checks_passed;
            } else {
                ++summary.checks_failed;
            }
        }
        for (const std::string& item : line.unverified) {
            if (std::find(summary.unverified.begin(), summary.unverified.end(), item) ==
                summary.unverified.end()) {
                summary.unverified.push_back(item);
            }
        }
    }
    std::sort(summary.unverified.begin(), summary.unverified.end());

    const double candidate_success =
        candidate_rate_rows > 0 ? candidate_success_sum / candidate_rate_rows : 0.0;
    const double candidate_acceptance =
        candidate_rate_rows > 0 ? candidate_acceptance_sum / candidate_rate_rows : 0.0;
    const double baseline_success =
        baseline_rate_rows > 0 ? baseline_success_sum / baseline_rate_rows : 0.0;
    const double baseline_acceptance =
        baseline_rate_rows > 0 ? baseline_acceptance_sum / baseline_rate_rows : 0.0;

    FillDelta(summary.success_rate, candidate_success, baseline_success,
              summary.has_baseline_metrics);
    FillDelta(summary.acceptance_rate, candidate_acceptance, baseline_acceptance,
              summary.has_baseline_metrics);
    FillDelta(summary.tool_calls, static_cast<double>(candidate_tool_calls),
              static_cast<double>(baseline_tool_calls), has_baseline_rows);
    FillDelta(summary.tokens, static_cast<double>(candidate_tokens),
              static_cast<double>(baseline_tokens), has_baseline_rows);
    FillDelta(summary.wall_clock_ms, static_cast<double>(candidate_wall),
              static_cast<double>(baseline_wall), has_baseline_rows);
    FillDelta(summary.permission_prompts, static_cast<double>(candidate_prompts),
              static_cast<double>(baseline_prompts), has_baseline_rows);
    FillDelta(summary.workspace_writes, static_cast<double>(candidate_writes),
              static_cast<double>(baseline_writes), has_baseline_rows);
    return summary;
}

bool EvalSummary::any_fixture_missing() const {
    return std::find(unverified.begin(), unverified.end(), "fixture-missing") != unverified.end();
}

std::string BuildDeterministicVerdict(const EvalSummary& summary) {
    std::ostringstream out;
    out << "确定性评测汇总(Evaluator 首版=结构化汇总,未接评判模型;判词只算一份证据,不压过测试与产物):\n";
    out << "  静态门 " << (summary.static_gate.fail > 0 ? "fail" : "pass") << ";replay pass "
        << summary.replay.pass << " / fail " << summary.replay.fail << " / 跳过 "
        << summary.replay.skipped << ";holdout pass " << summary.holdout.pass << " / fail "
        << summary.holdout.fail << " / 跳过 " << summary.holdout.skipped << ";baseline "
        << (summary.baseline.total() == 0 ? "未跑" : "pass " + std::to_string(summary.baseline.pass) +
                                                          " / fail " +
                                                          std::to_string(summary.baseline.fail))
        << "\n";
    out << "  通过 " << summary.checks_passed << " 项检查,失败 " << summary.checks_failed
        << ",跳过 " << summary.checks_skipped << "(人工验收/缺夹具,没测不冒充)\n";
    if (summary.unverified.empty()) {
        out << "  没测到: (账面干净)\n";
    } else {
        out << "  没测到: ";
        for (std::size_t i = 0; i < summary.unverified.size(); ++i) {
            out << (i > 0 ? "," : "") << summary.unverified[i];
        }
        out << "\n";
    }
    if (summary.has_baseline_metrics) {
        out << "  对照基线 " << (summary.baseline_ref.empty() ? "(未记)" : summary.baseline_ref)
            << ":成功率 " << FormatRate(summary.success_rate.candidate) << " 对 "
            << FormatRate(summary.success_rate.baseline) << ",验收率 "
            << FormatRate(summary.acceptance_rate.candidate) << " 对 "
            << FormatRate(summary.acceptance_rate.baseline) << ";"
            << FormatCost(summary.tool_calls, "tool calls") << ","
            << FormatCost(summary.tokens, "tokens") << ","
            << FormatCost(summary.wall_clock_ms, "墙钟ms") << ","
            << FormatCost(summary.permission_prompts, "确认") << ","
            << FormatCost(summary.workspace_writes, "写入") << "\n";
    } else {
        out << "  对照基线: 基线侧没有指标账,代价对照缺(未测)\n";
    }
    if (!summary.has_holdout) {
        out << "  (无留出任务:只可标 experimental,不可自动建议晋升稳定版)\n";
    }
    if (summary.complexity.has_value() && !summary.complexity->shape.empty()) {
        out << "  复杂度代价: " << summary.complexity->SummaryLine() << ";不是组件越多越容易晋升\n";
    }
    return out.str();
}

int EvalExitCode(const EvalSummary& summary, bool plan_loaded, bool fixture_missing_any) {
    if (!plan_loaded || fixture_missing_any || summary.any_fixture_missing()) {
        return 2;
    }
    if (summary.any_fail()) {
        return 1;
    }
    return 0;
}

}  // namespace lubancode::evolution
