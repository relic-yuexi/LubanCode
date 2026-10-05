#include "runtime/assembly/builtin_tools.hpp"

#include <memory>
#include <utility>

#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

namespace lubancode::runtime::assembly {

std::unique_ptr<tools::Tool> CreateLocalTool(
    const std::string& name, std::shared_ptr<tools::IRipgrepRunner> search_runner,
    const tools::WebFetchOptions* web_fetch_options) {
    if (name == "read_file") return std::make_unique<tools::ReadFileTool>();
    if (name == "run_command") return std::make_unique<tools::RunCommandTool>();
    if (name == "write_file") return std::make_unique<tools::WriteFileTool>();
    if (name == "edit_file") return std::make_unique<tools::EditFileTool>();
    if (name == "search" && search_runner) return std::make_unique<tools::SearchTool>(std::move(search_runner));
    if (name == "web_fetch" && web_fetch_options)
        return std::make_unique<tools::WebFetchTool>(*web_fetch_options);
    // A new table belongs to this exact opening, never a project/host singleton.
    // SDK Close retires it with the registry; resume starts a fresh table.
    if (name == "todo_write") return std::make_unique<tools::TodoWriteTool>(std::make_shared<tools::TodoListState>());
    return nullptr;
}

tools::ToolRegistry BuildBaseToolRegistry(const std::vector<tools::SkillMeta>& skills,
                                        const config::SearchConfig& search_config,
                                        std::string user_agent) {
    tools::ToolRegistry registry;
    registry.Register(CreateLocalTool("read_file"));
    registry.Register(CreateLocalTool("run_command"));
    // 三件仍采用当前宿主内后台命令寿命,不是跨宿主重启的实验 Runner。
    registry.Register(std::make_unique<tools::BackgroundOutputTool>());
    registry.Register(std::make_unique<tools::StopBackgroundTool>());
    registry.Register(CreateLocalTool("write_file"));
    registry.Register(CreateLocalTool("edit_file"));
    // 沿用随包 ripgrep 定位;缺资源时保持原错误,不悄悄换后端。
    registry.Register(std::make_unique<tools::SearchTool>(
        std::make_shared<tools::BundledRipgrepRunner>()));
    registry.Register(std::make_unique<tools::SkillTool>(skills));
    registry.Register(std::make_unique<tools::WebFetchTool>(std::move(user_agent)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<tools::WebSearchTool>(search_config));
    }
    return registry;
}

tools::ToolRegistry BuildExploreToolRegistry(const config::SearchConfig& search_config,
                                           std::string user_agent) {
    tools::ToolRegistry registry;
    registry.Register(CreateLocalTool("read_file"));
    registry.Register(std::make_unique<tools::SearchTool>(
        std::make_shared<tools::BundledRipgrepRunner>()));
    registry.Register(std::make_unique<tools::WebFetchTool>(std::move(user_agent)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<tools::WebSearchTool>(search_config));
    }
    return registry;
}

}  // namespace lubancode::runtime::assembly
