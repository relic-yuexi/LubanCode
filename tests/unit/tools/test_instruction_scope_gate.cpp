// AGENTS.md 作用域单 P0 的写前闸册。钉的账(单子 §7.3/§7.4/§12.2):
//   1. 握手:首次写被拦、规则注入 tool_result、零副作用;同目标重试放行;
//   2. 已见指纹:同 scope 后续写不重复拦;AGENTS 内容一变指纹失效重拦;
//   3. 多 scope:两棵子树两份规则,任一未确认整笔拦,两边都确认才放行;
//   4. 基线预登记:root->cwd 链拼进提示的(逐字节对上)不拦;截断的不算;
//   5. RunOneTool 端到端:闸在 ModePolicy 之后、任何文件副作用之前;
//      终态 ScopeGatePending + 稳定码 scope.instructions_required;
//   6. 没有 AGENTS.md 的目标照旧直写(旧项目零退化)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/types.hpp"
#include "config/project_instructions.hpp"
#include "tools/instruction_scope.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "tools/write_file.hpp"
#include "tools/edit_file.hpp"

namespace {

class TempProject {
public:
    TempProject() {
        path = std::filesystem::temp_directory_path() /
               ("lubancode_gate_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path / ".git");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void Write(const std::filesystem::path& relative, const std::string& content) {
        std::filesystem::create_directories((path / relative).parent_path());
        std::ofstream file(path / relative, std::ios::binary);
        file << content;
    }

    std::string Utf8(const std::filesystem::path& relative) const {
        return lubancode::tools::PathToUtf8(path / relative);
    }

    std::filesystem::path path;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

using lubancode::config::ProjectInstructionResolver;
using lubancode::tools::InstructionScopeState;
using lubancode::tools::ScopedInstructionGate;

TEST_CASE("gate handshake blocks first write, injects rules, allows retry") {
    TempProject project;
    project.Write("AGENTS.md", "root rule: keep it small");
    project.Write("src/parser/AGENTS.md", "parser rule: never touch legacy paths");

    const ProjectInstructionResolver resolver;
    InstructionScopeState state;
    ScopedInstructionGate gate(resolver, state);

    const std::string target = project.Utf8("src/parser/token.cpp");
    const auto first = gate.CheckTargets({target});
    REQUIRE(first.has_value());
    // 注入了完整规则:两份都在,nearest 标注优先级,重试指引在。
    CHECK(Contains(first->message, "root rule: keep it small"));
    CHECK(Contains(first->message, "parser rule: never touch legacy paths"));
    CHECK(Contains(first->message, "instructions_required"));
    CHECK(Contains(first->message, "重试"));
    CHECK(first->presented_fingerprints.size() == 1);
    // 零副作用:文件没被建出来。
    CHECK(!std::filesystem::exists(project.path / "src/parser/token.cpp"));

    // 同目标重试:放行。
    const auto second = gate.CheckTargets({target});
    CHECK_FALSE(second.has_value());
}

TEST_CASE("gate re-blocks when agents content changes between attempts") {
    TempProject project;
    project.Write("AGENTS.md", "v1 rule");

    const ProjectInstructionResolver resolver;
    InstructionScopeState state;
    ScopedInstructionGate gate(resolver, state);
    const std::string target = project.Utf8("a.cpp");
    REQUIRE(gate.CheckTargets({target}).has_value());

    // 内容变了:旧指纹不在账上,重新拦、重新注入新规则。
    project.Write("AGENTS.md", "v2 rule");
    const auto again = gate.CheckTargets({target});
    REQUIRE(again.has_value());
    CHECK(Contains(again->message, "v2 rule"));
}

TEST_CASE("gate groups multi-path targets by chain fingerprint and blocks atomically") {
    TempProject project;
    project.Write("frontend/AGENTS.md", "frontend rule");
    project.Write("backend/AGENTS.md", "backend rule");

    const ProjectInstructionResolver resolver;
    InstructionScopeState state;
    ScopedInstructionGate gate(resolver, state);

    const std::string fe = project.Utf8("frontend/src/a.ts");
    const std::string be = project.Utf8("backend/src/b.cpp");

    // 先确认 frontend 那只 scope。
    REQUIRE(gate.CheckTargets({fe}).has_value());
    CHECK_FALSE(gate.CheckTargets({fe}).has_value());

    // backend 未确认:整笔拦,注入的只有 backend 的规则(frontend 已见过,
    // 不重复塞)。
    const auto mixed = gate.CheckTargets({fe, be});
    REQUIRE(mixed.has_value());
    CHECK(Contains(mixed->message, "backend rule"));
    CHECK(!Contains(mixed->message, "frontend rule"));
    CHECK(mixed->presented_fingerprints.size() == 1);

    // 两边都确认过才放行。
    CHECK_FALSE(gate.CheckTargets({fe, be}).has_value());
}

TEST_CASE("gate passes targets with no instruction documents") {
    TempProject project;  // 只有 .git,没有 AGENTS.md
    const ProjectInstructionResolver resolver;
    InstructionScopeState state;
    ScopedInstructionGate gate(resolver, state);
    CHECK_FALSE(gate.CheckTargets({project.Utf8("plain.txt")}).has_value());
    // 空链不入账:没有可确认的规则,不放行权也没发出去过。
    CHECK(state.Seen("0000000000000000000000000000000000000000000000000000000000000000") == false);
}

TEST_CASE("baseline marking keeps root-to-cwd writes unblocked and refuses truncated baselines") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "src rule");
    project.Write("src/parser/AGENTS.md", "parser rule");

    const ProjectInstructionResolver resolver;
    InstructionScopeState state;
    // 基线 = root->cwd 的投影串(装配时拼进系统提示的那截)。
    const std::string baseline = resolver.ResolveForPath(project.path / "src").content;
    lubancode::tools::MarkBaselineSeen(resolver, state, project.path / "src", baseline);

    // 同 scope 写:不拦(root->src 已在提示里)。
    ScopedInstructionGate gate(resolver, state);
    CHECK_FALSE(gate.CheckTargets({project.Utf8("src/leaf.cpp")}).has_value());
    // 更深一层的新 scope(root->src->parser)仍要拦。
    const auto deeper = gate.CheckTargets({project.Utf8("src/parser/leaf.cpp")});
    CHECK(deeper.has_value());

    // 串对不上(搬房/外部改动):不预登记,首写重新注入。
    InstructionScopeState mismatched;
    lubancode::tools::MarkBaselineSeen(resolver, mismatched, project.path / "src", "some other string");
    ScopedInstructionGate mismatch_gate(resolver, mismatched);
    CHECK(mismatch_gate.CheckTargets({project.Utf8("src/leaf.cpp")}).has_value());

    // 截断的基线不算已见:模型没读全,首写重新注入。
    const ProjectInstructionResolver capped(32);
    InstructionScopeState truncated_state;
    lubancode::tools::MarkBaselineSeen(
        capped, truncated_state, project.path / "src",
        capped.ResolveForPath(project.path / "src").content);
    ScopedInstructionGate truncated_gate(capped, truncated_state);
    CHECK(truncated_gate.CheckTargets({project.Utf8("src/leaf.cpp")}).has_value());
}

TEST_CASE("collect write targets only knows path-carrying write tools") {
    nlohmann::json input;
    input["path"] = "some/file.txt";
    input["content"] = "x";
    CHECK(lubancode::tools::CollectWriteTargets("write_file", input) ==
          std::vector<std::string>{"some/file.txt"});
    CHECK(lubancode::tools::CollectWriteTargets("edit_file", input) ==
          std::vector<std::string>{"some/file.txt"});
    CHECK(lubancode::tools::CollectWriteTargets("read_file", input).empty());
    CHECK(lubancode::tools::CollectWriteTargets("mcp__server__tool", input).empty());
    CHECK(lubancode::tools::CollectWriteTargets("write_file", nlohmann::json::object()).empty());
}

TEST_CASE("run one tool gates the write before any file side effect") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/parser/AGENTS.md", "parser rule");

    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());

    const auto resolver = std::make_shared<const ProjectInstructionResolver>();
    const auto state = std::make_shared<InstructionScopeState>();
    lubancode::agent::TurnWiring wiring;
    wiring.on_scope_gate = lubancode::tools::BuildScopeGateCallback(resolver, state);

    const std::string target = project.Utf8("src/parser/token.cpp");
    lubancode::api::ToolUseBlock call;
    call.id = "gate-1";
    call.name = "write_file";
    call.input = nlohmann::json{{"path", target}, {"content", "int main(){}"}};

    // 第一发:被闸拦下,零副作用,终态与稳定码钉住,规则进了 tool_result。
    const auto blocked = lubancode::agent::RunOneTool(registry, call, wiring, nullptr);
    CHECK(blocked.is_error);
    CHECK(blocked.outcome == lubancode::agent::ToString(lubancode::agent::ToolOutcome::ScopeGatePending));
    CHECK(blocked.error_code == lubancode::agent::kErrScopeInstructionsRequired);
    CHECK(Contains(blocked.content, "parser rule"));
    CHECK(!std::filesystem::exists(lubancode::tools::Utf8ToPath(target)));

    // 第二发(模型读了规则,原样重试):放行落盘。
    call.id = "gate-2";
    const auto allowed = lubancode::agent::RunOneTool(registry, call, wiring, nullptr);
    CHECK_FALSE(allowed.is_error);
    CHECK(std::filesystem::exists(lubancode::tools::Utf8ToPath(target)));

    // 同 scope 的后续写(edit_file)不再拦。
    lubancode::api::ToolUseBlock edit;
    edit.id = "gate-3";
    edit.name = "edit_file";
    edit.input = nlohmann::json{{"path", target},
                                {"old_string", "main"},
                                {"new_string", "win"}};
    const auto edited = lubancode::agent::RunOneTool(registry, edit, wiring, nullptr);
    CHECK_FALSE(edited.is_error);

    // 没装闸的旧装配:行为与从前一字不差(直写)。
    lubancode::agent::TurnWiring legacy;
    lubancode::api::ToolUseBlock legacy_call;
    legacy_call.id = "gate-4";
    legacy_call.name = "write_file";
    legacy_call.input = nlohmann::json{{"path", project.Utf8("fresh.cpp")}, {"content", "x"}};
    const auto legacy_result = lubancode::agent::RunOneTool(registry, legacy_call, legacy, nullptr);
    CHECK_FALSE(legacy_result.is_error);
}

TEST_CASE("smoke: nested gate, root writes unaffected, override still wins at the gate") {
    // 冒烟场景(单子 §13 验收):临时目录造嵌套 AGENTS.md(根 + src/parser/
    // 一层),从仓库根写 src/parser/ 下文件——首拦、注入、重试放行;根下
    // 文件不受嵌套层影响;同层 override 压过 AGENTS.md 的机械表照旧。
    TempProject project;
    project.Write("AGENTS.md", "root: run ctest");
    project.Write("src/parser/AGENTS.md", "parser: legacy");
    project.Write("src/parser/AGENTS.override.md", "parser override: new style only");

    const auto resolver = std::make_shared<const ProjectInstructionResolver>();
    const auto state = std::make_shared<InstructionScopeState>();
    // 从仓库根起会话:基线 = 根链(root->cwd,cwd 即仓库根)。
    lubancode::tools::MarkBaselineSeen(*resolver, *state, project.path,
                                       resolver->ResolveForPath(project.path).content);
    lubancode::agent::TurnWiring wiring;
    wiring.on_scope_gate = lubancode::tools::BuildScopeGateCallback(resolver, state);

    // 根下文件:链 = 基线,不拦、不受嵌套层影响。
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    lubancode::api::ToolUseBlock root_write;
    root_write.id = "s-1";
    root_write.name = "write_file";
    root_write.input = nlohmann::json{{"path", project.Utf8("root_file.txt")}, {"content", "x"}};
    CHECK_FALSE(lubancode::agent::RunOneTool(registry, root_write, wiring, nullptr).is_error);

    // src/parser/ 下首写:拦,注入的规则里 override 压过 AGENTS.md。
    lubancode::api::ToolUseBlock deep_write;
    deep_write.id = "s-2";
    deep_write.name = "write_file";
    deep_write.input = nlohmann::json{{"path", project.Utf8("src/parser/new.cpp")}, {"content", "y"}};
    const auto blocked = lubancode::agent::RunOneTool(registry, deep_write, wiring, nullptr);
    CHECK(blocked.is_error);
    CHECK(Contains(blocked.content, "parser override: new style only"));
    CHECK(!Contains(blocked.content, "parser: legacy"));
    CHECK(!std::filesystem::exists(project.path / "src/parser/new.cpp"));

    // 重试放行。
    deep_write.id = "s-3";
    const auto retried = lubancode::agent::RunOneTool(registry, deep_write, wiring, nullptr);
    CHECK_FALSE(retried.is_error);
    CHECK(std::filesystem::exists(project.path / "src/parser/new.cpp"));
}

TEST_CASE("injection fits nearest documents first under budget") {
    TempProject project;
    project.Write("AGENTS.md", std::string(1000, 'r'));  // 根文件很大
    project.Write("src/AGENTS.md", "nearest rule");

    const ProjectInstructionResolver resolver(512);  // 小帽:两份装不下
    const auto chain = resolver.ResolveForPath(project.path / "src/x.cpp");
    const std::string injection = lubancode::tools::BuildChainInjection(chain, 512);
    // 近处规则永不被挤没;没装下的远端文档点名列出,不冒充全部已装。
    CHECK(Contains(injection, "nearest rule"));
    CHECK(Contains(injection, "AGENTS.md"));
    CHECK(Contains(injection, "未完整装入"));
}

// ---------------------------------------------------------------------------
// P1-4 fail closed:active write chain 按整份文档计,预算内装不下 → 拒收
// 明说(不注入半截、不登记指纹、重试不放行),零副作用照旧。
// ---------------------------------------------------------------------------

TEST_CASE("gate fails closed when the active write chain exceeds the budget") {
    TempProject project;
    project.Write("AGENTS.md", std::string(5000, 'r'));
    project.Write("src/AGENTS.md", "src rule");

    const ProjectInstructionResolver resolver(1024);  // 小帽:整链装不下
    InstructionScopeState state;
    ScopedInstructionGate gate(resolver, state);
    const std::string target = project.Utf8("src/a.cpp");

    const auto denial = gate.CheckTargets({target});
    REQUIRE(denial.has_value());
    CHECK(denial->over_budget);
    CHECK(Contains(denial->message, "instructions_over_budget"));
    CHECK(Contains(denial->message, "重试同一操作不会放行"));
    // 拆分/调大建议在文案里。
    CHECK(Contains(denial->message, "拆短"));
    CHECK(Contains(denial->message, "调大预算"));
    // fail closed 不发确认权:指纹不登记(模型没见过完整规则)。
    CHECK(denial->presented_fingerprints.empty());
    CHECK(!state.Seen(resolver.ResolveForPath(lubancode::tools::Utf8ToPath(target)).fingerprint));
    // 重试仍拒——与握手的根本区别。零副作用:文件没被建出来。
    CHECK(gate.CheckTargets({target}).has_value());
    CHECK(!std::filesystem::exists(project.path / "src/a.cpp"));

    // 同一棵树换只宽帽的 Resolver:恢复握手语义(首拦注入、重试放行)。
    const ProjectInstructionResolver roomy(64 * 1024);
    InstructionScopeState fresh_state;
    ScopedInstructionGate roomy_gate(roomy, fresh_state);
    const auto handshake = roomy_gate.CheckTargets({target});
    REQUIRE(handshake.has_value());
    CHECK_FALSE(handshake->over_budget);
    CHECK(Contains(handshake->message, "instructions_required"));
    CHECK(Contains(handshake->message, "src rule"));
    CHECK_FALSE(roomy_gate.CheckTargets({target}).has_value());

    // ChainInjectionBytes 与注入正文同一把尺:整链字节 ≥ 正文合计。
    const auto chain = roomy.ResolveForPath(lubancode::tools::Utf8ToPath(target));
    std::size_t content_total = 0;
    for (const auto& doc : chain.documents) {
        content_total += doc.content.size();
    }
    CHECK(lubancode::tools::ChainInjectionBytes(chain) >= content_total);
}

TEST_CASE("run one tool reports over-budget denials with their own stable outcome") {
    TempProject project;
    project.Write("AGENTS.md", std::string(5000, 'r'));

    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());

    const auto resolver = std::make_shared<const ProjectInstructionResolver>(512);
    const auto state = std::make_shared<InstructionScopeState>();
    lubancode::agent::TurnWiring wiring;
    wiring.on_scope_gate = lubancode::tools::BuildScopeGateCallback(resolver, state);

    lubancode::api::ToolUseBlock call;
    call.id = "budget-1";
    call.name = "write_file";
    call.input = nlohmann::json{{"path", project.Utf8("a.cpp")}, {"content", "x"}};

    const auto blocked = lubancode::agent::RunOneTool(registry, call, wiring, nullptr);
    CHECK(blocked.is_error);
    CHECK(blocked.outcome == lubancode::agent::ToString(lubancode::agent::ToolOutcome::ScopeGateOverBudget));
    CHECK(blocked.error_code == lubancode::agent::kErrScopeInstructionsOverBudget);
    CHECK(Contains(blocked.content, "instructions_over_budget"));
    CHECK(!std::filesystem::exists(project.path / "a.cpp"));

    // 再试一次:仍是 over_budget(不是握手,重试不放行)。
    call.id = "budget-2";
    const auto again = lubancode::agent::RunOneTool(registry, call, wiring, nullptr);
    CHECK(again.error_code == lubancode::agent::kErrScopeInstructionsOverBudget);
    CHECK(!std::filesystem::exists(project.path / "a.cpp"));
}
