// /model 命令 presenter 实现(合同见 model_commands.hpp)。函数体自
// interactive_session 的 DispatchSlashCommand Model case 原文搬家(改道:
// 会话依赖走 ctx、输出走 TerminalPort、break 收成 return),行为一字
// 不差——注释一并随行。

#include "app/commands/model_commands.hpp"

#include "api/models.hpp"                      // ListModels(裸敲菜单的远端清单)
#include "app/commands/command_registry.hpp"   // SlashDispatchContext(分派注册制)
#include "config/provider_catalog.hpp"         // ResolveProviderHeaderTemplates

#include <cctype>

#include "app/commands/settings_commands.hpp"  // PrintModelRolesTable/ChooseModelId
#include "app/model_router.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "runtime/command_service.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;
using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
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
    // 当前家没有:遍历列了这个模型名的各家条目,归属只认配置里真有的
    // 那家(多家都配了取目录序第一);全是没配的家(比如只列在官方
    // openai 条目下)才提示属谁未配置,模型照旧本家切,由调用方提示。
    const std::string* unconfigured = nullptr;
    for (const auto& entry : catalog.models) {
        if (entry.slug != model_id || entry.provider_id.empty()) {
            continue;  // 用户条目上面已按本家接住,这里只看各家归属
        }
        if (lubancode::config::FindProvider(providers, entry.provider_id) != nullptr) {
            return ModelProviderHop{entry.provider_id, /*configured=*/true};
        }
        if (unconfigured == nullptr) {
            unconfigured = &entry.provider_id;
        }
    }
    if (unconfigured != nullptr) {
        return ModelProviderHop{*unconfigured, /*configured=*/false};
    }
    return std::nullopt;  // 目录没这个名:手敲的裸名,不猜归属
}

void HandleModelCommand(const ModelCommandContext& ctx, const std::string& args) {
    lubancode::config::Config& config = *ctx.config;
    const lubancode::config::ModelCatalog& model_catalog = *ctx.model_catalog;
    if (args == "roles") {
        const std::optional<lubancode::agent::ModelRouteTable> roles_table =
            ctx.model_router != nullptr
                ? std::optional<lubancode::agent::ModelRouteTable>(ctx.model_router->Table())
                : std::nullopt;
        PrintModelRolesTable(roles_table.has_value() ? &*roles_table : nullptr);
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
                        TermOut() << tr("cmd.session_only") << "\n";
                    }
                    const auto result = command_service.SetRoleModel(role_word, rest, write_config);
                    if (result.switched) {
                        TermOut() << trf("cmd.model.role_switched", result.role, result.model) << "\n";
                        if (write_config && result.config_written) {
                            TermOut() << trf("cmd.write_config.updated", *write_target) << "\n";
                        } else if (write_config && !result.error.empty()) {
                            TermOut() << trf("cmd.write_config.failed", result.error) << "\n";
                        }
                    } else if (result.error == "unknown_role") {
                        TermOut() << trf("cmd.model.role_unknown", role_word) << "\n";
                    } else {
                        TermOut() << trf("cmd.model.fetch_failed", result.error) << "\n";
                    }
                    return;
                }
            }
        }
    }
    // 选定模型 id:带参直切用参数;裸敲先拉清单,菜单(交互)或
    // 编号(管道)选一项。到这里两条输入路合流。
    std::string chosen = args;
    if (chosen.empty()) {
        const auto query = command_service.QueryModels();
        if (query.fetch_failed) {
            TermOut() << trf("cmd.model.fetch_failed", query.fetch_error) << "\n";
            return;
        }
        if (query.models.empty()) {
            TermOut() << tr("cmd.model.list_empty") << "\n";
            return;
        }
        const std::optional<std::string> picked = ChooseModelId(query, model_catalog);
        if (!picked.has_value()) {
            return;  // 取消/编号作废,提示已就地打出。
        }
        chosen = *picked;
    }
    // 统一提交:先跨家判定(/model 跨家收口)——所选模型目录条目声明了
    // 归属别家时,连 provider 一起切(base_url/鉴权/wire/目录声明全套,
    // 与 /provider switch 同一条 ExecuteProviderSwitch 路),再切换模型;
    // 否则旧行为,只切模型不动连接。该家没配时不切不拦,名字记下来等
    // 切成后以备注口吻说一句(第三轮返件:从"连接未换可能不认"的警告
    // 降为备注——中转家的模型进得了活列表、进不了本地目录,报警告就
    // 是唬人);缺密钥或没递切换能力照样如实拦,不留半切换。
    bool switched_provider = false;
    std::string unconfigured_provider;  // 非空 = 切成后补一句备注
    if (ctx.model_catalog != nullptr && ctx.active_provider != nullptr) {
        const std::vector<lubancode::config::ProviderConfig> no_providers;
        const std::vector<lubancode::config::ProviderConfig>& providers =
            ctx.providers != nullptr ? *ctx.providers : no_providers;
        const auto hop = ModelProviderHopFor(*ctx.model_catalog, providers, *ctx.active_provider, chosen);
        if (hop.has_value()) {
            if (!hop->configured) {
                unconfigured_provider = hop->provider_id;
            } else if (!ctx.switch_provider) {
                TermOut() << trf("cmd.model.other_provider_unswitchable", chosen, hop->provider_id) << "\n";
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
        TermOut() << trf("cmd.model.fetch_failed", result.error) << "\n";
        return;
    }
    // 活列表选择落痕(第三轮返件):切成的模型在当前家写一条用户条目进
    // models.json——"这家确实用过这模型"的真凭据,此后跨家判定第一步
    // 认它,再切同名零提示。失败只报一行,不拦切换。
    if (ctx.active_provider != nullptr && !ctx.active_provider->empty()) {
        const auto models_path = lubancode::config::ModelCatalogPath();
        if (models_path.has_value()) {
            const auto remembered = lubancode::config::RememberModelChoiceInCatalog(
                *models_path, *ctx.active_provider, result.model, result.model);
            if (!remembered.has_value()) {
                TermOut() << trf("cmd.model.remember_choice_failed", remembered.error()) << "\n";
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
    // 跨家切开的回执多带一顶帽子:明说模型连同 provider 一起换了家。
    if (switched_provider) {
        TermOut() << trf("cmd.model.switched_with_provider", result.model, *ctx.active_provider) << "\n";
    } else {
        TermOut() << trf("cmd.model.switched", result.model) << "\n";
    }
    if (!unconfigured_provider.empty()) {
        TermOut() << trf("cmd.model.other_provider_note", result.model, unconfigured_provider) << "\n";
    }
    if (result.think_from_catalog) {
        TermOut() << trf("catalog.apply_think", result.think) << "\n";
    }
    if (result.applied_context_window.has_value()) {
        TermOut() << trf("catalog.apply_window", *result.applied_context_window) << "\n";
    }
    if (result.instructions_replaced) {
        TermOut() << trf("catalog.apply_instructions", result.model) << "\n";
    }
    if (write_target.has_value()) {
        const auto answer = lubancode::cli::ReadLine(trf("cmd.write_config_prompt", *write_target));
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto written = command_service.WriteModelToConfig(result.model);
            if (written.has_value()) {
                TermOut() << trf("cmd.write_config.updated", *write_target) << "\n";
            } else {
                TermOut() << trf("cmd.write_config.failed", written.error()) << "\n";
            }
        }
    } else {
        TermOut() << tr("cmd.session_only") << "\n";
    }
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):/model 的分派位。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------

CommandFlow HandleSlashModel(SlashDispatchContext& dispatch, const lubancode::cli::ParsedSlashCommand& parsed) {
    lubancode::app::ModelCommandContext model_ctx;
    model_ctx.config = dispatch.config;
    model_ctx.model_catalog = dispatch.model_catalog;
    model_ctx.theme = dispatch.theme;
    model_ctx.context_tracker = dispatch.context_tracker;
    model_ctx.current_model = dispatch.current_model;
    model_ctx.current_think = dispatch.current_think;
    model_ctx.current_model_instructions = dispatch.current_model_instructions;
    model_ctx.model_router = dispatch.model_router;
    // /model 跨家收口:直切属别家的模型时连 provider 一起切。判定材料
    //(活跃端、已配清单)与切换执行(ExecuteProviderSwitch,与 /provider
    // switch 同一条路)都在这里装配;缺密钥如实提示并保持旧连接,不硬切。
    model_ctx.active_provider = dispatch.active_provider;
    model_ctx.providers = &dispatch.config->providers;
    model_ctx.switch_provider = [&dispatch](const std::string& name) -> bool {
        const lubancode::config::ProviderConfig* provider =
            lubancode::config::FindProvider(dispatch.config->providers, name);
        if (provider == nullptr) {
            TermOut() << trf("cmd.provider.not_found", name) << "\n";
            return false;
        }
        if (lubancode::config::ResolveProviderAuth(*provider).status ==
            lubancode::config::ProviderAuthResolution::Status::Missing) {
            TermOut() << trf("cmd.model.provider_key_missing", name) << "\n";
            return false;
        }
        return ExecuteProviderSwitch(name, "", *dispatch.config, *dispatch.active_provider,
                                     *dispatch.real_backend, *dispatch.wire_str, dispatch.current_model,
                                     dispatch.current_think, *dispatch.context_tracker,
                                     dispatch.current_model_instructions, *dispatch.model_catalog,
                                     *dispatch.prompt_options, dispatch.rebuild_loop, dispatch.spinner_enabled,
                                     *dispatch.theme, *dispatch.active_provider_write_path,
                                     dispatch.config_result->sources.active_provider);
    };
    // 写回目标默认全局,没有全局文件退 merged 路径(只剩项目级)。
    model_ctx.config_file_path = dispatch.config_result->global_config_file_path.has_value()
                                     ? dispatch.config_result->global_config_file_path
                                     : *dispatch.config_file_path;
    model_ctx.apply_context_window = [tracker = dispatch.context_tracker](std::size_t tokens) {
        tracker->set_window_tokens(tokens);
    };
    model_ctx.fetch_models = [config = dispatch.config]()
        -> std::expected<std::vector<std::pair<std::string, std::string>>, std::string> {
        const auto headers = lubancode::config::ResolveProviderHeaderTemplates(
            config->extra_headers, config->auth_token);
        auto listed = lubancode::api::ListModels(config->wire, config->base_url, config->auth_token,
                                                 config->connect_timeout_ms,
                                                 config->request_timeout_secs, headers);
        if (!listed.has_value()) {
            return std::unexpected(listed.error().message);
        }
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& info : *listed) {
            out.emplace_back(info.id, info.display_name);
        }
        return out;
    };
    model_ctx.sync_request_policy = dispatch.sync_request_policy;
    HandleModelCommand(model_ctx, parsed.args);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
