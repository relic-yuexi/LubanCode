// AgentFaceIsReadOnly(真机实测 P2-3):agent_type 的工具面是否只读。
// Plan 闸放行"tools.allow 全为只读工具"的自定义 Agent(契约 4.9:只读不
// 设权限档,由 tools.allow 表达)。注册表用真工具(ReadFileTool 等自带
// effect_class 声明),不造假元数据。

#include <doctest/doctest.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tools/agent_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_tool.hpp"
#include "tools/write_file.hpp"

using lubancode::tools::AgentFaceIsReadOnly;
using lubancode::tools::CustomAgentMaterial;
using lubancode::tools::ReadFileTool;
using lubancode::tools::RunCommandTool;
using lubancode::tools::SearchTool;
using lubancode::tools::SkillTool;
using lubancode::tools::ToolRegistry;
using lubancode::tools::WriteFileTool;

namespace {

ToolRegistry MakeRegistry() {
    ToolRegistry registry;
    registry.Register(std::make_unique<ReadFileTool>());
    registry.Register(std::make_unique<SearchTool>());
    registry.Register(std::make_unique<WriteFileTool>());
    registry.Register(std::make_unique<RunCommandTool>());
    registry.Register(std::make_unique<SkillTool>(std::vector<lubancode::tools::SkillMeta>{}));
    return registry;
}

// resolver:给 allow 表造一份自定义 Agent 材料(名字无所谓,只看工具面)。
std::function<std::optional<CustomAgentMaterial>(const std::string&)> ResolverFor(
    std::vector<std::string> allow) {
    return [allow = std::move(allow)](const std::string&) -> std::optional<CustomAgentMaterial> {
        CustomAgentMaterial material;
        material.definition.name = "reviewer";
        material.definition.tools.allow = allow;
        return material;
    };
}

}  // namespace

TEST_CASE("agent_face: 内置两枚——Explore 只读,general-purpose 不只读") {
    ToolRegistry registry = MakeRegistry();
    const auto none = std::function<std::optional<CustomAgentMaterial>(const std::string&)>{};
    CHECK(AgentFaceIsReadOnly(none, registry, "Explore"));
    CHECK(AgentFaceIsReadOnly(none, registry, "explore"));    // 大小写不敏感
    CHECK_FALSE(AgentFaceIsReadOnly(none, registry, "general-purpose"));
    CHECK_FALSE(AgentFaceIsReadOnly(none, registry, "anything-else"));  // 没挂解析口:认不得就拒
}

TEST_CASE("agent_face: tools.allow 全只读的自定义 Agent 算只读") {
    ToolRegistry registry = MakeRegistry();
    CHECK(AgentFaceIsReadOnly(ResolverFor({"read_file", "search"}), registry, "library-reviewer"));
    CHECK(AgentFaceIsReadOnly(ResolverFor({"read_file"}), registry, "library-reviewer"));
    // skill 是只读档(P2-3 声明过):放进白名单不改只读判定。
    CHECK(AgentFaceIsReadOnly(ResolverFor({"read_file", "search", "skill"}), registry, "library-reviewer"));
}

TEST_CASE("agent_face: 白名单混入写盘/命令/未知档即不只读") {
    ToolRegistry registry = MakeRegistry();
    CHECK_FALSE(AgentFaceIsReadOnly(ResolverFor({"read_file", "write_file"}), registry, "builder"));
    CHECK_FALSE(AgentFaceIsReadOnly(ResolverFor({"run_command"}), registry, "runner"));
    CHECK_FALSE(AgentFaceIsReadOnly(ResolverFor({"read_file", "not_a_tool"}), registry, "ghost"));
    // 空 allow = 不裁,继承全工具面:不算只读。
    CHECK_FALSE(AgentFaceIsReadOnly(ResolverFor({}), registry, "wide-open"));
}

TEST_CASE("agent_face: 解析不出名字(不存在/unavailable)不算只读") {
    ToolRegistry registry = MakeRegistry();
    const auto missing = [](const std::string&) -> std::optional<CustomAgentMaterial> { return std::nullopt; };
    CHECK_FALSE(AgentFaceIsReadOnly(missing, registry, "no-such-agent"));
}
