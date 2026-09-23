// /model 命令 presenter 实现(合同见 model_commands.hpp)。函数体自
// interactive_session 的 DispatchSlashCommand Model case 原文搬家(改道:
// 会话依赖走 ctx、输出走 TerminalPort、break 收成 return),行为一字
// 不差——注释一并随行。

#include "app/commands/model_commands.hpp"

// HC-06 第二小批:装包段(活清单拉取/跨家切换)迁组合根后,ListModels
// 的 include 随行搬走(底下那枚 provider_catalog 是
// ModelProviderHopFor 等纯函数要的,与装包段无关,照留)。

#include <algorithm>
#include <cctype>
#include <vector>

#include "app/commands/settings_commands.hpp"  // PrintModelRolesTable/ChooseModelId
#include "app/model_router.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 4:/model 渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "platform/console.hpp"  // GetScreenInfo:/model 的框宽同一把尺
#include "runtime/command_service.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;
using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

namespace frame = lubancode::cli::frame;

// TUI 排版批 4(/model 全族)的公共小件。渲染段只调 cli::frame::* 三助手
//(约定见 docs/development/tui_style.md);文案全是既有 tr()/trf() 键,
// i18n 不新增(单子合同第 4 条),句内冒号按 SentenceField 拆两列(批 1
// 裁量)。theme 空指针(单测/未递)按 /plugin 先例退 plain。

int ModelFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

lubancode::cli::Theme ResolveModelTheme(const lubancode::cli::Theme* theme) {
    return theme != nullptr ? *theme : lubancode::cli::Theme{};
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void PrintModelNotice(const lubancode::cli::Theme& theme, const std::vector<std::string>& sentences,
                      frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        const std::size_t colon = sentence.find(':');
        if (colon == std::string::npos) {
            fields.push_back(frame::Field{"", sentence, accent});
        } else {
            fields.push_back(frame::Field{TrimAscii(sentence.substr(0, colon)),
                                          TrimAscii(sentence.substr(colon + 1)), accent});
        }
    }
    for (const std::string& line :
         frame::RenderKeyValues({}, fields, theme, frame::Light(), ModelFrameWidth())) {
        TermOut() << line << "\n";
    }
}

// /model <role> <id> 的角色词归一:小写 + 去首尾空白(TrimAscii 只去空白
// 不动大小写——模型 id 走它,角色词走这里)。
std::string NormalizeRoleWord(std::string word) {
    std::string out = TrimAscii(std::move(word));
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::optional<ModelProviderHop> ModelProviderHopFor(const lubancode::config::ModelCatalog& catalog,
                                                    const std::vector<lubancode::config::ProviderConfig>& providers,
                                                    const std::string& active_provider,
                                                    const std::string& model_id) {
    // 判定序(跨家收口验收返件):同名模型多家目录都列是中转常态
    //(gateway/openrouter 家都摆着 openai 家的模型名),归属不能拿
    // FindBySlug 的"重名取先出现"一家独断。
    //
    // 第一优先:当前家目录条目里就有这个模型(含用户自写 models.json
    // 条目的全局覆盖——provider_id 空 = 归属不明,压过内置档案)。当前
    // 家有的模型就是本家切换,零提示零动作,绝不拿别家条目的归属说事。
    // 注意不能拿 FindByProviderAndSlug 一把梭:它带"全局兜底",兜底命中
    // 时恰恰是当前家没有、只有别家条目——那正是要往下判定的场面。
    for (const auto& entry : catalog.models) {
        if (entry.slug == model_id &&
            (entry.provider_id.empty() || entry.provider_id == active_provider)) {
            return std::nullopt;
        }
    }
    // 当前家没有:遍历列了这个模型名的各家条目。自动跳家只吃"权威且
    // 唯一"的映射(巡检单 P1)——配置里真有的那家恰只一家才切;多家都
    // 配了(中转常态:gateway 家与官方家都摆着同名模型)返回 ambiguous,
    // 由调用方提示并留在本家,不拿目录序独断。全是没配的家(比如只列在
    // 官方 openai 条目下)提示属谁未配置,模型照旧本家切,由调用方提示。
    const std::string* unconfigured = nullptr;
    std::vector<const std::string*> configured_ids;
    for (const auto& entry : catalog.models) {
        if (entry.slug != model_id || entry.provider_id.empty()) {
            continue;  // 用户条目上面已按本家接住,这里只看各家归属
        }
        if (lubancode::config::FindProvider(providers, entry.provider_id) != nullptr) {
            bool seen = false;
            for (const auto* id : configured_ids) {
                seen = seen || *id == entry.provider_id;
            }
            if (!seen) {
                configured_ids.push_back(&entry.provider_id);
            }
        } else if (unconfigured == nullptr) {
            unconfigured = &entry.provider_id;
        }
    }
    if (configured_ids.size() == 1) {
        return ModelProviderHop{*configured_ids.front(), /*configured=*/true};
    }
    if (configured_ids.size() > 1) {
        std::string joined;
        for (const auto* id : configured_ids) {
            if (!joined.empty()) {
                joined += "/";
            }
            joined += *id;
        }
        return ModelProviderHop{joined, /*configured=*/false, /*ambiguous=*/true};
    }
    if (unconfigured != nullptr) {
        return ModelProviderHop{*unconfigured, /*configured=*/false};
    }
    return std::nullopt;  // 目录没这个名:手敲的裸名,不猜归属
}

void HandleModelCommand(const ModelCommandContext& ctx, const std::string& args) {
    lubancode::config::Config& config = *ctx.config;
    const lubancode::config::ModelCatalog& model_catalog = *ctx.model_catalog;
    // 批 4:渲染段走 frame;theme 空指针按 /plugin 先例退 plain。
    const lubancode::cli::Theme theme = ResolveModelTheme(ctx.theme);
    if (args == "roles") {
        const std::optional<lubancode::agent::ModelRouteTable> roles_table =
            ctx.model_router != nullptr
                ? std::optional<lubancode::agent::ModelRouteTable>(ctx.model_router->Table())
                : std::nullopt;
        PrintModelRolesTable(roles_table.has_value() ? &*roles_table : nullptr, theme);
        return;
    }
    lubancode::runtime::CommandService::Options command_options;
    command_options.config = &config;
    command_options.model_catalog = &model_catalog;
    command_options.current_model = ctx.current_model;
    command_options.current_think = ctx.current_think;
    command_options.current_model_instructions = ctx.current_model_instructions;
    command_options.apply_context_window = ctx.apply_context_window;
    // 写回目标默认全局(2026-08-25 改):模型跟人走,不跟项目走,
    // 免得换个项目就冒出一份钉死的旧模型;项目级要钉,手编
    // <项目>/.lubancode/config.json。没有全局文件时退 merged
    // 路径(只剩项目级的情形)。
    command_options.config_file_path = ctx.config_file_path;
    // 问话/回显要与 service 实际写的同一份文件,先抄一份。
    const std::optional<std::string> write_target = command_options.config_file_path;
    command_options.fetch_models = ctx.fetch_models;
    lubancode::runtime::CommandService command_service(std::move(command_options));
    if (!args.empty()) {
        // 角色设置(/model <role> <id>):两段式一律走角色路,
        // 角色词是不是 normal/cheap/lao(plan 是 lao 的别名)由
        // SetRoleModel 认定,不认的如实报错——不然
        // "/model turbo x9" 会被当成模型名叫"turbo x9"的直切,
        // 垃圾名悄悄写进配置。单段(比如 /model cheap)仍当直切
        // 的模型名处理,不冒充角色命令。落盘与直切同一套:默认
        // 全局,问一句才写(write_target 见上),没有可写的就只
        // 活本会话。
        {
            const std::size_t space = args.find_first_of(" \t");
            if (space != std::string::npos) {
                // 角色词去空白转小写;模型名只去首尾空白,大小写
                // 原样保留——模型 id 区分大小写,MiniMax-M3 不许
                // 变 minimax-m3。
                const std::string role_word = NormalizeRoleWord(args.substr(0, space));
                const std::string rest = TrimAscii(args.substr(space + 1));
                if (!rest.empty()) {
                    bool write_config = false;
                    if (write_target.has_value()) {
                        const auto answer = lubancode::cli::ReadLine(trf("cmd.write_config_prompt", *write_target));
                        write_config = answer.has_value() && (*answer == "y" || *answer == "Y");
                    } else {
                        PrintModelNotice(theme, {tr("cmd.session_only")});
                    }
                    const auto result = command_service.SetRoleModel(role_word, rest, write_config);
                    if (result.switched) {
                        std::vector<std::string> notes{
                            trf("cmd.model.role_switched", result.role, result.model)};
                        if (write_config && result.config_written) {
                            notes.push_back(trf("cmd.write_config.updated", *write_target));
                        } else if (write_config && !result.error.empty()) {
                            notes.push_back(trf("cmd.write_config.failed", result.error));
                        }
                        PrintModelNotice(theme, notes);
                    } else if (result.error == "unknown_role") {
                        PrintModelNotice(theme, {trf("cmd.model.role_unknown", role_word)},
                                         frame::FieldAccent::Error);
                    } else {
                        PrintModelNotice(theme, {trf("cmd.model.fetch_failed", result.error)},
                                         frame::FieldAccent::Error);
                    }
                    return;
                }
            }
        }
    }
    // 选定模型 id:带参直切用参数;裸敲先拉清单,菜单(交互)或
    // 编号(管道)选一项。到这里两条输入路合流。
    std::string chosen = args;
    // 活列表证据(ccmoon 巡检单 P1):chosen 选自当前家真机 /models 的
    // 清单时为真——本轮不查跨家目录,先落痕再切,首选零误报。
    bool from_live_list = false;
    if (chosen.empty()) {
        const auto query = command_service.QueryModels();
        if (query.fetch_failed) {
            // 真机列表失败才退静态,并把"静态"明写出来(验收:不可装作
            // 真机单)。静态清单只列本家条目与用户条目,选中后照走静态
            // 跨家判定(有提示,口径软)。
            const auto static_query = [&]() -> std::optional<lubancode::runtime::ModelQueryResult> {
                lubancode::runtime::ModelQueryResult out;
                out.current_model = ctx.current_model ? *ctx.current_model : std::string();
                const std::string active = ctx.active_provider != nullptr ? *ctx.active_provider : std::string();
                for (const auto& entry : model_catalog.models) {
                    if (!entry.provider_id.empty() && entry.provider_id != active) {
                        continue;
                    }
                    bool duplicate = false;
                    for (const auto& listed : out.models) {
                        duplicate = duplicate || listed.id == entry.slug;
                    }
                    if (duplicate) {
                        continue;
                    }
                    lubancode::runtime::ModelListEntry item;
                    item.id = entry.slug;
                    item.display_name = entry.display_name.empty() ? entry.slug : entry.display_name;
                    item.current = item.id == out.current_model;
                    out.models.push_back(std::move(item));
                }
                return out;
            }();
            if (!static_query.has_value() || static_query->models.empty()) {
                PrintModelNotice(theme, {trf("cmd.model.fetch_failed", query.fetch_error)},
                                 frame::FieldAccent::Error);
                return;
            }
            PrintModelNotice(theme, {trf("cmd.model.static_list_header", query.fetch_error)});
            const std::optional<std::string> picked = ChooseModelId(*static_query, model_catalog, ctx.theme);
            if (!picked.has_value()) {
                return;
            }
            chosen = *picked;
        } else {
            if (query.models.empty()) {
                PrintModelNotice(theme, {tr("cmd.model.list_empty")});
                return;
            }
            // 成功的真机清单自带 provider 证据:先说清这一单是谁家的,
            // 从这张单选出的项本轮按本家算(下面先落痕再切,不查跨家)。
            if (ctx.active_provider != nullptr && !ctx.active_provider->empty()) {
                PrintModelNotice(
                    theme,
                    {trf("cmd.model.live_list_header", query.models.size(), *ctx.active_provider)});
            }
            const std::optional<std::string> picked = ChooseModelId(query, model_catalog, ctx.theme);
            if (!picked.has_value()) {
                return;  // 取消/编号作废,提示已就地打出。
            }
            chosen = *picked;
            from_live_list = true;
        }
    }
    // 端点相性(ccmoon 巡检单 P1):Realtime 模型混进菜单时,确认前说清
    // "多半不走当前 wire"。只提示不拦——真机证明的只是这家中转的
    // Responses 路由不通,不判模型死刑(音频模型同理,另跑真机再立结论)。
    if (ctx.config != nullptr && ctx.model_catalog != nullptr) {
        const auto* entry = model_catalog.FindBySlug(chosen);
        if (lubancode::config::ClassifyModelEndpoint(entry, chosen) ==
            lubancode::config::ModelEndpointKind::Realtime) {
            PrintModelNotice(
                theme, {trf("cmd.model.realtime_hint", chosen,
                            lubancode::config::ProviderWireName(ctx.config->wire))},
                frame::FieldAccent::Stats);
        }
    }
    // 活列表证据先行:选自本家真机清单的项,先把"这家确实用过这模型"
    // 的凭据落进 models.json(0.26.64 的落痕机制,次序倒过来:先落痕,
    // 再判定——落成了静态判定第一步自然认本家)。落不成也不碍事:
    // 本轮照样按本家切,不走跨家判定,首选零误报(不许靠选第二回
    // 自愈)。
    if (from_live_list && ctx.active_provider != nullptr && !ctx.active_provider->empty()) {
        const auto models_path = lubancode::config::ModelCatalogPath();
        if (models_path.has_value()) {
            const auto remembered = lubancode::config::RememberModelChoiceInCatalog(*models_path, *ctx.active_provider,
                                                                                    chosen, chosen);
            if (!remembered.has_value()) {
                PrintModelNotice(
                    theme, {trf("cmd.model.remember_choice_failed", remembered.error())},
                    frame::FieldAccent::Error);
            }
        }
    }
    // 统一提交:先跨家判定(/model 跨家收口)——所选模型目录条目声明了
    // 归属别家时,连 provider 一起切(base_url/鉴权/wire/目录声明全套,
    // 与 /provider switch 同一条 ExecuteProviderSwitch 路),再切换模型;
    // 否则旧行为,只切模型不动连接。活列表选出的项(from_live_list)整段
    // 跳过:那是最硬的证据,本轮按本家切(巡检单 P1)。
    // 该家没配时不切不拦,名字记下来等切成后以"某家目录也收录"的口吻
    // 说一句——不可断言中转站不认(中转家活列表常常正列着它);多家
    // 已配目录都列这名(ambiguous)留在本家,提示一句;缺密钥或没递
    // 切换能力照样如实拦,不留半切换。
    bool switched_provider = false;
    std::string unconfigured_provider;  // 非空 = 切成后补一句备注
    if (!from_live_list && ctx.model_catalog != nullptr && ctx.active_provider != nullptr) {
        const std::vector<lubancode::config::ProviderConfig> no_providers;
        const std::vector<lubancode::config::ProviderConfig>& providers =
            ctx.providers != nullptr ? *ctx.providers : no_providers;
        const auto hop = ModelProviderHopFor(*ctx.model_catalog, providers, *ctx.active_provider, chosen);
        if (hop.has_value()) {
            if (hop->ambiguous) {
                // 多家已配目录都列这名:不自动跳(只吃权威且唯一的映射),
                // 留在本家切,提示一句。
                PrintModelNotice(theme, {trf("cmd.model.hop_ambiguous", chosen, hop->provider_id)},
                                 frame::FieldAccent::Stats);
            } else if (!hop->configured) {
                unconfigured_provider = hop->provider_id;
            } else if (!ctx.switch_provider) {
                PrintModelNotice(
                    theme,
                    {trf("cmd.model.other_provider_unswitchable", chosen, hop->provider_id)},
                    frame::FieldAccent::Stats);
            } else if (ctx.switch_provider(hop->provider_id)) {
                switched_provider = true;
            } else {
                return;  // 切换失败已自打提示,连接未动;模型不切,不留半切换
            }
        }
    }
    // 统一提交:先切换 + 应用目录条目(回执就地排版),随后问一
    // 句是否写盘。写盘时 active_provider 在场,service 会把模型写
    // 进 provider 条目(每个 provider 各记各的,切走再切回来还是
    // 它);顶层 model 字段会被活跃端镜像压过,单写没用。落盘目标
    // 默认全局(write_target 见上):模型跟人走,不跟项目走——
    // 换个项目不该冒出一份钉死的旧模型,项目级要钉请手编
    // <项目>/.lubancode/config.json。
    const auto result = command_service.SetModel(chosen, /*write_config=*/false);
    if (!result.switched) {
        PrintModelNotice(theme, {trf("cmd.model.fetch_failed", result.error)},
                         frame::FieldAccent::Error);
        return;
    }
    // 活列表选择落痕(第三轮返件):切成的模型在当前家写一条用户条目进
    // models.json——"这家确实用过这模型"的真凭据,此后跨家判定第一步
    // 认它,再切同名零提示。失败只报一行,不拦切换。活列表路在判定前
    // 已落过一回(证据先行),这里只补直输路的。
    if (!from_live_list && ctx.active_provider != nullptr && !ctx.active_provider->empty()) {
        const auto models_path = lubancode::config::ModelCatalogPath();
        if (models_path.has_value()) {
            const auto remembered = lubancode::config::RememberModelChoiceInCatalog(
                *models_path, *ctx.active_provider, result.model, result.model);
            if (!remembered.has_value()) {
                PrintModelNotice(
                    theme, {trf("cmd.model.remember_choice_failed", remembered.error())},
                    frame::FieldAccent::Error);
            }
        }
    }
    // 五层后端退役(批四):/model 的即时生效改走皮上的 request
    // 档案与叠层(model/effort/目录指令/魂一并刷新),下一份
    // 请求带上新模型——前缀指纹从此看得见 model_changed,cache
    // epoch 的账不再瞎(换模型那一份本来就是断前缀)。
    if (ctx.sync_request_policy) {
        ctx.sync_request_policy();
    }
    // 切换回执一组进同一只键值对框(批 4):跨家帽子/目录收录备注/目录
    // 应用三行(apply_think/apply_window/apply_instructions)都是这一单
    // 的账,拆框反而碎。
    std::vector<std::string> switch_notes;
    if (switched_provider) {
        // 跨家切开的回执多带一顶帽子:明说模型连同 provider 一起换了家。
        switch_notes.push_back(
            trf("cmd.model.switched_with_provider", result.model, *ctx.active_provider));
    } else {
        switch_notes.push_back(trf("cmd.model.switched", result.model));
    }
    if (!unconfigured_provider.empty()) {
        // 直输路径的静态归属只说"某家目录也收录",不断言中转站不认
        //(巡检单 P1:自动跳家只吃权威且唯一的映射,提示口径同步放软)。
        switch_notes.push_back(
            trf("cmd.model.catalog_also_lists", unconfigured_provider, result.model));
    }
    if (result.think_from_catalog) {
        switch_notes.push_back(trf("catalog.apply_think", result.think));
    }
    if (result.applied_context_window.has_value()) {
        switch_notes.push_back(trf("catalog.apply_window", *result.applied_context_window));
    }
    if (result.instructions_replaced) {
        switch_notes.push_back(trf("catalog.apply_instructions", result.model));
    }
    PrintModelNotice(theme, switch_notes);
    if (write_target.has_value()) {
        const auto answer = lubancode::cli::ReadLine(trf("cmd.write_config_prompt", *write_target));
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto written = command_service.WriteModelToConfig(result.model);
            if (written.has_value()) {
                PrintModelNotice(theme, {trf("cmd.write_config.updated", *write_target)});
            } else {
                PrintModelNotice(theme, {trf("cmd.write_config.failed", written.error())},
                                 frame::FieldAccent::Error);
            }
        }
    } else {
        PrintModelNotice(theme, {tr("cmd.session_only")});
    }
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):/model 的分派位。HC-06(材料收窄,第二小批)
// 起只吃窄材料——装包段(跨家切换/活清单/同步三闭包)随材料在组合根
// (interactive_session_assembly 的 AssembleDispatchContext)折好,行为与
// 旧装包位一字不差。
// ---------------------------------------------------------------------------

CommandFlow HandleSlashModel(const ModelCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleModelCommand(ctx, parsed.args);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
