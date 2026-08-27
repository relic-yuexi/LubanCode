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

// /model 直切的跨家判定(纯函数,单测钉),判定序三条:
//   1. 当前家目录条目里有这个模型(含用户自写 models.json 条目的全局
//      覆盖)→ 返回 nullopt:本家切换,零提示零动作。中转家的常态——
//      目录里中转家也摆着官方家的模型名,那是本家的模型,不拿别家
//      条目的归属说事(FindBySlug 的"重名取先出现"在这里独断就是误报)。
//   2. 当前家没有 → 列了这名的各家条目里挑配置里真有的那家:只有一家
//      真配了才算权威且唯一的映射,返回 configured=true(自动跳家只吃
//      这种映射,ccmoon 巡检单 P1);多家都配了返回 ambiguous=true,
//      provider_id 拼着各家名,调用方提示并留在本家,不拿目录序独断。
//   3. 同名条目全是没配的家 → 返回第一家 (configured=false),调用方
//      以"某家目录也收录"的口吻提示并保持本家连接——不可断言中转站
//      不认这名字(中转家活列表常常正列着它)。
// 目录压根没这名的 → nullopt(手敲的裸名,不猜归属)。
// 活列表证据(裸 /model 从当前家真机列表选出的项)不走这只函数——
// 调用方先落痕(RememberModelChoiceInCatalog)再判定,第 1 条自然接住,
// 本轮零跨家提示(巡检单 P1:先认活证据,再判静态归属)。
struct ModelProviderHop {
    std::string provider_id;
    bool configured = false;
    bool ambiguous = false;  // 多家已配目录都列这名:不自动跳,只提示
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
