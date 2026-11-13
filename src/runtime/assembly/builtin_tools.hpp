// 基础/探索工具表的共用注册实现,不加载宿主配置、不启动外部服务。
// 本件只建工具集合,不替宿主授权;托管宿主仍须按部署策略挑选工具。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "tools/registry.hpp"
#include "tools/skill_loader.hpp"

namespace lubancode::tools {
class IRipgrepRunner;
struct WebFetchOptions;
struct WebSearchOptions;
}

namespace lubancode::runtime::assembly {

// Explicit local capability admission. Unknown names return null. The caller
// supplies cwd/permission policy; no process-global state is changed here.
// Search requires a caller-owned runner. Its preparation belongs to the caller,
// before session startup; the default does not discover ambient rg for the SDK.
std::unique_ptr<tools::Tool> CreateLocalTool(
    const std::string& name, std::shared_ptr<tools::IRipgrepRunner> search_runner = {},
    const tools::WebFetchOptions* web_fetch_options = nullptr,
    const tools::WebSearchOptions* web_search_options = nullptr);

// user_agent 由宿主显式传入,本层不依赖发行版本头。
// skills 是已解析清单,不在此处扫描个人目录或项目目录。
tools::ToolRegistry BuildBaseToolRegistry(const std::vector<tools::SkillMeta>& skills,
                                        const config::SearchConfig& search_config,
                                        std::string user_agent);

// 只读探索集合:read_file、search、web_fetch,可选 web_search。
tools::ToolRegistry BuildExploreToolRegistry(const config::SearchConfig& search_config,
                                           std::string user_agent);

}  // namespace lubancode::runtime::assembly
