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
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

namespace lubancode::runtime::assembly {

tools::ToolRegistry BuildBaseToolRegistry(const std::vector<tools::SkillMeta>& skills,
                                        const config::SearchConfig& search_config,
                                        std::string user_agent) {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<tools::ReadFileTool>());
    registry.Register(std::make_unique<tools::RunCommandTool>());
    // 三件仍采用当前宿主内后台命令寿命,不是跨宿主重启的实验 Runner。
    registry.Register(std::make_unique<tools::BackgroundOutputTool>());
    registry.Register(std::make_unique<tools::StopBackgroundTool>());
    registry.Register(std::make_unique<tools::WriteFileTool>());
    registry.Register(std::make_unique<tools::EditFileTool>());
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
    registry.Register(std::make_unique<tools::ReadFileTool>());
    registry.Register(std::make_unique<tools::SearchTool>(
        std::make_shared<tools::BundledRipgrepRunner>()));
    registry.Register(std::make_unique<tools::WebFetchTool>(std::move(user_agent)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<tools::WebSearchTool>(search_config));
    }
    return registry;
}

}  // namespace lubancode::runtime::assembly
