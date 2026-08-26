// 终端接线收尾单:/model 命令 presenter。原先 156 行的 Model case 住在
// interactive_session 的 DispatchSlashCommand 里,按病灶二拆出:命令产出
// 数据行(CommandService 的 typed 调用与结果),presenter 负责怎么画
// (roles 短表、清单菜单、写回问话;输出全走 TerminalPort);大类只留
// 分派。
//
// P7(显示系统剥离单):typed API 在 runtime::CommandService,终端这条是
// 薄翻译。带参直切与裸敲菜单选出的是同一个 id,选定后同走 SetModel 提交,
// 业务一处(两条输入路不许再分叉)。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)
#include "config/config.hpp"               // ProviderConfig(跨家判定)

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::agent {
struct ModelRouteTable;
enum class TaskKind;
}
namespace lubancode::cli {
struct Theme;
class ContextTracker;
}
namespace lubancode::config {
struct Config;
struct ModelCatalog;
}

namespace lubancode::app {

class ModelRouterService;

struct ModelCommandContext {
    lubancode::config::Config* config = nullptr;
    const lubancode::config::ModelCatalog* model_catalog = nullptr;
    const lubancode::cli::Theme* theme = nullptr;
    lubancode::cli::ContextTracker* context_tracker = nullptr;
    std::shared_ptr<std::string> current_model;
    std::shared_ptr<std::string> current_think;
    std::shared_ptr<std::string> current_model_instructions;
    // /model roles 的路由表来源;空 = 没装路由(表打空)。
    const ModelRouterService* model_router = nullptr;
    // 当前活跃 provider 名(跨家判定用);空 = 调用方没递(单测),判定
    // 跳过,行为与旧版一致。
    const std::string* active_provider = nullptr;
    // 已配 providers(查归属家在不在配置里);空 = 同上,判定跳过。
    const std::vector<lubancode::config::ProviderConfig>* providers = nullptr;
    // 模型属别家时连 provider 一起切(/model 跨家收口):名字进去,成功
    // true;失败已自打提示。可空 = 没递切换能力,如实提示连接未换。
    std::function<bool(const std::string&)> switch_provider;
    // 写回目标(2026-08-25 起):默认全局配置文件,没有全局文件时退调用方
    // 递进来的 merged 路径。调用方已按优先级算好。
    std::optional<std::string> config_file_path;
    // 目录窗口生效口:runtime 不认得 cli 的 ContextTracker,回调里替它把
    // 会话窗口落账。
    std::function<void(std::size_t)> apply_context_window;
    // 拉远端模型清单(裸敲菜单用);空 = 单测/离线场景,QueryModels 走
    // service 自带的目录条目路。
    std::function<std::expected<std::vector<std::pair<std::string, std::string>>, std::string>()> fetch_models;
    // 切换成功后的会话级同步(SyncAgentRequestPolicy:皮上的 request 档案
    // 与叠层刷新,下一份请求即时生效)。可空。
    std::function<void()> sync_request_policy;
};

// /model 直切的跨家判定(纯函数,单测钉):目录条目带 provider_id 且与
// active 不同 → 该模型属别家。返回 nullopt = 不用切(无条目、条目没写
// 归属、或就是当前家);有值时 provider_id 是归属家,configured 说配置里
// 有没有这家(没有就切不了,调用方如实提示)。
struct ModelProviderHop {
    std::string provider_id;
    bool configured = false;
};
std::optional<ModelProviderHop> ModelProviderHopFor(const lubancode::config::ModelCatalog& catalog,
                                                    const std::vector<lubancode::config::ProviderConfig>& providers,
                                                    const std::string& active_provider,
                                                    const std::string& model_id);

// /model 的三路:roles(路由表短打)/ <role> <id>(角色设置,两段式)/
// <id> 或裸敲(直切与菜单选,两条输入路合流)。
void HandleModelCommand(const ModelCommandContext& ctx, const std::string& args);

// 命令分派注册制(会话终章):/model 的分派位(材料装包 + presenter),
// 自 interactive_session 大 switch 原样搬来。
struct SlashDispatchContext;
CommandFlow HandleSlashModel(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
