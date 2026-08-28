// Workflows 单第 5 批:Draft 解析/追问/预览/原子安装/enable/remove、
// /workflow 命令解析与 run 参数解析。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "app/commands/workflow_commands.hpp"
#include "cli/theme.hpp"
#include "workflow/compiler.hpp"
#include "workflow/parser.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_wf_compiler_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 一份完整候选 AST(论文检索两路精简版)。
nlohmann::json MakeCandidate() {
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(nlohmann::json{{"id", "parse_query"},
                                   {"kind", "llm"},
                                   {"prompt", "prompts/parse-query.md"},
                                   {"input", nlohmann::json{{"q", "${inputs.topic}"}}}});
    nodes.push_back(nlohmann::json{{"id", "search"},
                                   {"kind", "parallel"},
                                   {"branches", nlohmann::json::array({"arxiv", "dblp"})},
                                   {"join", "all_settled"}});
    nodes.push_back(nlohmann::json{{"id", "arxiv"},
                                   {"kind", "tool"},
                                   {"tool", "arxiv_search"},
                                   {"on_unavailable", "skip"}});
    nodes.push_back(nlohmann::json{{"id", "dblp"},
                                   {"kind", "tool"},
                                   {"tool", "dblp_search"},
                                   {"on_unavailable", "skip"}});
    nodes.push_back(nlohmann::json{{"id", "fin"}, {"kind", "end"}});
    return nlohmann::json{
        {"schema_version", 1},
        {"id", "paper-research"},
        {"version", "1.0.0"},
        {"name", "论文检索"},
        {"description", "两路检索"},
        {"alias", "论文检索"},
        {"entry", "parse_query"},
        {"inputs", nlohmann::json{{"type", "object"},
                                  {"required", nlohmann::json::array({"topic"})},
                                  {"properties", nlohmann::json{{"topic", nlohmann::json{{"type", "string"}}}}}}},
        {"nodes", nodes},
        {"edges", nlohmann::json::array({
                      nlohmann::json{{"from", "parse_query"}, {"outcome", "success"}, {"to", "search"}},
                      nlohmann::json{{"from", "search"}, {"outcome", "success"}, {"to", "fin"}},
                  })},
        {"result", nlohmann::json{{"topic", "${inputs.topic}"}}},
        {"unknowns", nlohmann::json::array()},
    };
}

}  // namespace

TEST_SUITE("workflows-compiler") {

TEST_CASE("ParseDraftJson:完整候选过,杜撰工具被抓") {
    using namespace lubancode::workflow;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};

    SUBCASE("完整候选") {
        auto draft = ParseDraftJson(MakeCandidate(), caps);
        REQUIRE(draft.has_value());
        CHECK(draft->definition.id == "paper-research");
        CHECK(draft->definition.alias == "论文检索");
        CHECK(draft->complete);
    }
    SUBCASE("模型杜撰工具:unknown_tool 拦下") {
        nlohmann::json candidate = MakeCandidate();
        candidate["nodes"][2]["tool"] = "google_scholar_search";  // 能力表里没有
        auto draft = ParseDraftJson(candidate, caps);
        REQUIRE(!draft.has_value());
        CHECK(draft.error()[0].code == "unknown_tool");
        CHECK(draft.error()[0].path == "nodes.arxiv.tool");
    }
    SUBCASE("明文 token:拒") {
        nlohmann::json candidate = MakeCandidate();
        candidate["nodes"][0]["input"] = nlohmann::json{{"api_key", "sk-123"}};
        auto draft = ParseDraftJson(candidate, caps);
        REQUIRE(!draft.has_value());
        CHECK(draft.error()[0].code == "plaintext_secret");
    }
}

TEST_CASE("ComputeClarifications:只列 blocking 缺口,已有答案不重问") {
    using namespace lubancode::workflow;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};
    auto draft_result = ParseDraftJson(MakeCandidate(), caps);
    REQUIRE(draft_result.has_value());
    WorkflowDraft draft = *draft_result;
    CHECK(ComputeClarifications(draft).empty());

    // 造缺口:输入没定,追问列出来。
    draft.definition.inputs = nlohmann::json::object();
    draft.unknowns.push_back(DraftUnknown{"inputs", "你想拿什么作输入?", true});
    draft.unknowns.push_back(DraftUnknown{"color", "报告用什么颜色?", false});  // 不 blocking
    const auto questions = ComputeClarifications(draft);
    REQUIRE(questions.size() == 1);
    CHECK(questions[0].field == "inputs");
}

TEST_CASE("BuildPreviewText:摘要/图/将写文件/警告全有") {
    using namespace lubancode::workflow;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};
    auto draft = ParseDraftJson(MakeCandidate(), caps);
    REQUIRE(draft.has_value());
    draft->warnings.push_back("Scholar 没有可倚仗的公开 API,该分支未接入");

    PreviewOptions options;
    options.scope = WorkflowScope::Project;
    options.install_dir = options.scope == WorkflowScope::Project ? fs::path("/proj/.lubancode/workflows/paper-research") : fs::path();
    const std::string text = BuildPreviewText(*draft, options);
    CHECK(text.find("论文检索") != std::string::npos);
    CHECK(text.find("/论文检索") != std::string::npos);
    CHECK(text.find("all_settled") != std::string::npos);
    CHECK(text.find("parse_query") != std::string::npos);
    CHECK(text.find("prompts/parse-query.md") != std::string::npos);
    CHECK(text.find("Scholar 没有") != std::string::npos);
}

TEST_CASE("InstallWorkflow:staging+rename 原子,round-trip 保 hash") {
    using namespace lubancode::workflow;
    TempDir tmp;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};
    auto draft = ParseDraftJson(MakeCandidate(), caps);
    REQUIRE(draft.has_value());

    InstallOptions options;
    options.scope = WorkflowScope::Project;
    std::map<std::string, std::string> prompts{{"prompts/parse-query.md", "解析这个查询"}};
    auto installed = InstallWorkflow(draft->definition, tmp.Get(), std::nullopt, options, prompts);
    REQUIRE(installed.has_value());
    CHECK(fs::exists(installed->dir / "workflow.yaml"));
    CHECK(fs::exists(installed->dir / "prompts" / "parse-query.md"));
    // staging 不留。
    std::error_code ec;
    CHECK_FALSE(fs::exists(tmp.Get() / ".lubancode" / "workflows" / ".staging-paper-research", ec));

    // 装好的定义 round-trip:读回解析,hash 一致。
    auto reloaded = LoadWorkflowDefinition(installed->dir / "workflow.yaml");
    REQUIRE(reloaded.has_value());
    CHECK(ContentHash(*reloaded) == installed->content_hash);

    // 同 id 不许重复装。
    auto again = InstallWorkflow(draft->definition, tmp.Get(), std::nullopt, options, prompts);
    REQUIRE(!again.has_value());
    CHECK(again.error().find("已存在") != std::string::npos);

    // catalog 立即可见。
    const Catalog catalog = LoadCatalog(tmp.Get(), std::nullopt);
    CHECK(catalog.Find("paper-research") != nullptr);
    CHECK(catalog.FindByAlias("论文检索") != nullptr);
}

TEST_CASE("UpdateWorkflow:留回滚件,内容 hash 变") {
    using namespace lubancode::workflow;
    TempDir tmp;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};
    auto draft = ParseDraftJson(MakeCandidate(), caps);
    REQUIRE(draft.has_value());
    InstallOptions options;
    auto installed =
        InstallWorkflow(draft->definition, tmp.Get(), std::nullopt, options, {{"prompts/parse-query.md", "v1"}});
    REQUIRE(installed.has_value());

    // 改版本再 Update。
    WorkflowDefinition updated = draft->definition;
    updated.version = "1.0.1";
    updated.normalized = BuildNormalizedJson(updated);
    auto result = UpdateWorkflow(updated, installed->dir, {{"prompts/parse-query.md", "v2"}});
    REQUIRE(result.has_value());
    CHECK(ContentHash(updated) == result->content_hash);
    CHECK(fs::exists(installed->dir / "workflow.yaml.bak"));  // 可回滚
    CHECK(ReadAll(installed->dir / "prompts" / "parse-query.md") == "v2");
    CHECK(result->content_hash != installed->content_hash);
}

TEST_CASE("enable/disable 与 remove") {
    using namespace lubancode::workflow;
    TempDir tmp;
    CapabilityTable caps;
    caps.tools = {"arxiv_search", "dblp_search"};
    auto draft = ParseDraftJson(MakeCandidate(), caps);
    REQUIRE(draft.has_value());
    InstallOptions options;
    auto installed = InstallWorkflow(draft->definition, tmp.Get(), std::nullopt, options);
    REQUIRE(installed.has_value());

    // disable:catalog 仍列,alias 不响应。
    CHECK(SetWorkflowEnabled(installed->dir, false).has_value());
    {
        Catalog catalog = LoadCatalog(tmp.Get(), std::nullopt);
        CHECK(catalog.Find("paper-research") != nullptr);       // 仍列
        CHECK(catalog.FindByAlias("论文检索") == nullptr);      // 不响应
    }
    CHECK(SetWorkflowEnabled(installed->dir, true).has_value());
    {
        Catalog catalog = LoadCatalog(tmp.Get(), std::nullopt);
        CHECK(catalog.FindByAlias("论文检索") != nullptr);
    }

    // remove:定义目录删掉。
    CHECK(RemoveWorkflow(installed->dir).has_value());
    std::error_code ec;
    CHECK_FALSE(fs::exists(installed->dir, ec));
}

TEST_CASE("/workflow 命令解析:子命令矩阵") {
    using lubancode::app::ParseWorkflowCommand;
    using A = lubancode::app::WorkflowCommandAction;

    CHECK(ParseWorkflowCommand("list").action == A::List);
    CHECK(ParseWorkflowCommand("list project").scope == "project");
    CHECK(ParseWorkflowCommand("list bogus").action == A::Invalid);
    CHECK(ParseWorkflowCommand("show paper-research").action == A::Show);
    CHECK(ParseWorkflowCommand("graph x mermaid").format == "mermaid");
    CHECK(ParseWorkflowCommand("graph x bogus").action == A::Invalid);
    CHECK(ParseWorkflowCommand("validate x").action == A::Validate);

    const auto run = ParseWorkflowCommand("run paper-research --topic \"graph neural networks\" --since 2024");
    CHECK(run.action == A::Run);
    CHECK(run.id == "paper-research");
    CHECK(run.rest.find("--topic") != std::string::npos);

    CHECK(ParseWorkflowCommand("resume run-1").action == A::Resume);
    CHECK(ParseWorkflowCommand("cancel run-1").action == A::Cancel);
    CHECK(ParseWorkflowCommand("history paper-research").action == A::History);
    CHECK(ParseWorkflowCommand("enable x").rest == "enable");
    CHECK(ParseWorkflowCommand("disable x").rest == "disable");
    const auto remove = ParseWorkflowCommand("remove x yes");
    CHECK(remove.action == A::Remove);
    CHECK(remove.confirm);
    CHECK(ParseWorkflowCommand("remove x").confirm == false);  // 不带确认词
    const auto create = ParseWorkflowCommand("create 四路检索论文写成 Markdown");
    CHECK(create.action == A::Create);
    CHECK(create.rest.find("四路") != std::string::npos);
    CHECK(ParseWorkflowCommand("alias").action == A::Alias);
    CHECK(ParseWorkflowCommand("").action == A::Invalid);
}

TEST_CASE("RunWorkflowById:参数解析与全流程(假执行器)") {
    using namespace lubancode::workflow;
    TempDir tmp;
    // 装一份单节点图。
    const char* yaml = R"YAML(
schema_version: 1
id: echo-flow
version: 1.0.0
name: echo
alias: 回声
entry: x
inputs:
  type: object
  required: [topic]
  properties:
    topic: { type: string }
    review_limit: { type: integer, default: 2 }
nodes:
  x:
    type: transform
    operation: echo
    input: { got: "${inputs.topic}" }
  fin:
    type: end
edges:
  - { from: x, on: success, to: fin }
result:
  topic: "${inputs.topic}"
  review_limit: "${inputs.review_limit}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    InstallOptions options;
    auto installed = InstallWorkflow(*parsed, tmp.Get(), std::nullopt, options);
    REQUIRE(installed.has_value());

    lubancode::cli::Theme theme;
    lubancode::app::WorkflowCommandContext ctx;
    ctx.project_root = tmp.Get();
    ctx.home_lubancode = tmp.Get() / "home";
    ctx.theme = &theme;

    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });
    std::map<NodeKind, std::shared_ptr<NodeExecutor>> executors;
    executors[NodeKind::Transform] = echo;

    SUBCASE("--topic 具名参数") {
        const std::string out =
            lubancode::app::RunWorkflowById(ctx, "echo-flow", "--topic \"量子纠错\"", executors);
        CHECK(out.find("succeeded") != std::string::npos);
    }
    SUBCASE("位置参填 required 首字段") {
        const std::string out = lubancode::app::RunWorkflowById(ctx, "echo-flow", "量子纠错", executors);
        CHECK(out.find("succeeded") != std::string::npos);
    }
    SUBCASE("裸启动会按 schema 问必填需求，再把整句送进 workflow") {
        int questions = 0;
        bool answered = false;
        bool run_started = false;
        std::vector<std::string> event_types;
        lubancode::runtime::FunctionEventSink events(
            [&](const lubancode::runtime::ServerEvent& event) {
                if (event.payload.contains("type")) {
                    event_types.push_back(event.payload["type"].get<std::string>());
                    if (event.payload["type"] == kEventNodeStarted) {
                        const std::string node_id = event.payload.value("node_id", std::string());
                        CHECK(event.payload.value("node_run_id", std::string()).find(
                                  "-" + node_id + "-a1") != std::string::npos);
                    }
                }
            });
        ctx.request_input = [&](const std::string& field,
                                const nlohmann::json& schema) -> std::optional<std::string> {
            ++questions;
            CHECK(field == "topic");
            CHECK(schema["type"] == "string");
            answered = true;
            return "先查清根因，再修掉这个 bug";
        };
        ctx.on_run_start = [&] {
            CHECK(answered);
            run_started = true;
        };
        ctx.event_sink = &events;
        const std::string out = lubancode::app::RunWorkflowById(ctx, "echo-flow", "", executors);
        CHECK(questions == 1);
        CHECK(run_started);
        CHECK((event_types == std::vector<std::string>{
                                  kEventRunStarted,
                                  kEventNodeStarted,
                                  kEventNodeCompleted,
                                  kEventRunCompleted,
                              }));
        CHECK(out.find("succeeded") != std::string::npos);
        CHECK(out.find("先查清根因，再修掉这个 bug") != std::string::npos);
    }
    SUBCASE("别名后带需求便直送，不再补问") {
        int questions = 0;
        ctx.request_input = [&](const std::string&, const nlohmann::json&) -> std::optional<std::string> {
            ++questions;
            return "不该问到这里";
        };
        const std::string out = lubancode::app::RunWorkflowById(
            ctx, "echo-flow", "修复 src 里的工作流入口", executors);
        CHECK(questions == 0);
        CHECK(out.find("succeeded") != std::string::npos);
        CHECK(out.find("修复 src 里的工作流入口") != std::string::npos);
    }
    SUBCASE("自然语言尾巴原样直送，正文里的双横线不当参数") {
        const std::string out = lubancode::app::RunWorkflowById(
            ctx, "echo-flow", "检查  --dry-run  不该丢", executors);
        CHECK(out.find("succeeded") != std::string::npos);
        CHECK(out.find("检查  --dry-run  不该丢") != std::string::npos);
    }
    SUBCASE("单一 string 必填项吃完整自然语言,并宽容提示符分隔") {
        const std::string out = lubancode::app::RunWorkflowById(
            ctx, "echo-flow", "> /chaoting D:\\lubancode\\src 美化 /skills 显示", executors);
        CHECK(out.find("succeeded") != std::string::npos);
        CHECK(out.find("/chaoting D:\\\\lubancode\\\\src 美化 /skills 显示") != std::string::npos);
    }
    SUBCASE("具名参数按 schema 类型取值") {
        const std::string out = lubancode::app::RunWorkflowById(
            ctx, "echo-flow", "--topic test --review_limit=5", executors);
        CHECK(out.find("succeeded") != std::string::npos);
        CHECK(out.find("invalid_inputs") == std::string::npos);
    }
    SUBCASE("缺必填:invalid_inputs 结构化错") {
        const std::string out = lubancode::app::RunWorkflowById(ctx, "echo-flow", "", executors);
        CHECK(out.find("invalid_inputs") != std::string::npos);
    }
    SUBCASE("取消令牌从终端入口传进 workflow runtime") {
        std::atomic<bool> cancel{true};
        const std::string out =
            lubancode::app::RunWorkflowById(ctx, "echo-flow", "量子纠错", executors, &cancel);
        CHECK(out.find("cancelled") != std::string::npos);
        CHECK(out.find("用户取消") != std::string::npos);
    }
    SUBCASE("找不到 workflow") {
        const std::string out = lubancode::app::RunWorkflowById(ctx, "nope", "", executors);
        CHECK(out.find("找不到") != std::string::npos);
    }
}

TEST_CASE("终端执行器装配:交互、skill、subflow 齐全,子流程能跑") {
    using namespace lubancode::workflow;
    TempDir tmp;
    const char* child_yaml = R"YAML(
schema_version: 1
id: child-flow
version: 1.0.0
entry: fin
nodes:
  fin: { type: end }
result:
  value: "${inputs.value}"
)YAML";
    const char* parent_yaml = R"YAML(
schema_version: 1
id: parent-flow
version: 1.0.0
name: parent
entry: call
nodes:
  call:
    type: subflow
    subflow: child-flow
    input: { value: nested }
  fin: { type: end }
edges:
  - { from: call, on: success, to: fin }
result:
  value: "${nodes.call.output.value}"
)YAML";
    auto child = ParseWorkflowYaml(child_yaml);
    auto parent = ParseWorkflowYaml(parent_yaml);
    REQUIRE(child.has_value());
    REQUIRE(parent.has_value());
    InstallOptions install;
    REQUIRE(InstallWorkflow(*child, tmp.Get(), std::nullopt, install).has_value());
    REQUIRE(InstallWorkflow(*parent, tmp.Get(), std::nullopt, install).has_value());

    lubancode::cli::Theme theme;
    lubancode::app::WorkflowCommandContext wf_ctx;
    wf_ctx.project_root = tmp.Get();
    wf_ctx.theme = &theme;
    lubancode::app::WorkflowExecutorContext exec_ctx;
    exec_ctx.build_tool_options = [] { return ToolExecutor::Options{}; };
    const auto executors = lubancode::app::BuildWorkflowExecutors(wf_ctx, exec_ctx, "parent-flow");
    CHECK(executors.contains(NodeKind::Approval));
    CHECK(executors.contains(NodeKind::AskUser));
    CHECK(executors.contains(NodeKind::Skill));
    CHECK(executors.contains(NodeKind::Subflow));

    const std::string out = lubancode::app::RunWorkflowById(wf_ctx, "parent-flow", "", executors);
    CHECK(out.find("succeeded") != std::string::npos);
    CHECK(out.find("nested") != std::string::npos);
}

TEST_CASE("alias 直呼解析:ResolveWorkflowAlias") {
    using namespace lubancode::workflow;
    TempDir tmp;
    const char* yaml = "schema_version: 1\nid: aliased\nversion: 1.0.0\nname: a\nalias: 论文检索\n"
                       "entry: only\nnodes:\n  only:\n    type: end\n";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    InstallOptions options;
    REQUIRE(InstallWorkflow(*parsed, tmp.Get(), std::nullopt, options).has_value());

    lubancode::cli::Theme theme;
    lubancode::app::WorkflowCommandContext ctx;
    ctx.project_root = tmp.Get();
    ctx.theme = &theme;
    CHECK(lubancode::app::ResolveWorkflowAlias(ctx, "论文检索") == "aliased");
    CHECK(lubancode::app::ResolveWorkflowAlias(ctx, "not-registered").empty());
    const auto candidates = lubancode::app::BuildWorkflowSlashCompletionCandidates(ctx);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].name == "/论文检索");

    // 撞 skill 后，执行入口与补全须同拍禁用，不能一边提示、一边不认。
    ctx.skill_names.push_back("论文检索");
    CHECK(lubancode::app::ResolveWorkflowAlias(ctx, "论文检索").empty());
    CHECK(lubancode::app::BuildWorkflowSlashCompletionCandidates(ctx).empty());
}

}  // TEST_SUITE(workflows-compiler)
