// 设置类 slash 命令:/config 诊断、/update、/skills+/skill、/think、
// /model、/provider(含 provider add 向导)、/language。model/provider
// 会改活后端与会话状态,参数表都由调用方(InteractiveLoop)递进来。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 api/cli/config/tools。


#pragma once

#include "agent/model_router.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "cli/console_input.hpp"
#include "agent/prompts.hpp"
#include "app/backend_stack.hpp"
#include "platform/console.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/provider_wizard.hpp"
#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/settings_local.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "runtime/command_service.hpp"
#include "runtime/session_runtime.hpp"  // RecordThinkHistory:/think history 落会话账
#include "tools/skill_loader.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;
void PrintLubanIcon(const lubancode::cli::Theme& theme);


// 交互模式启动横幅:一眼看全版本、wire、当前模型、工作目录,两行,不啰嗦。
void PrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme);


// 换会话边界或 provider 后重开一张干净屏面。调用方先判定真控制台，
// 免得 ANSI 清屏序列混进管道输出。
void ClearAndPrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme);


// 给向导(初次配置 / provider add)造一份完整 WizardIO:print/read_line/fetch_models
// 接真实 IO,choose 接 ReadChoiceMenu(↑↓ 方向键,初始高亮落在默认项,回车即选中),
// interactive 让 ReadChoice 在管道/重定向时回落编号。两处向导共用,免得注入逻辑漂移。
lubancode::cli::WizardIO MakeInteractiveWizardIO(const lubancode::cli::Theme& theme);

bool HandleUpdateCommand(const std::string& args, int connect_timeout_ms, int request_timeout_secs);


// /skills 命令:列出扫描到的技能;一个都没有时打印两处目录路径,顺带说明
// 怎么造一份(SKILL.md 起手 frontmatter 的最小样例)。
void PrintSkillsCommand(const std::vector<lubancode::tools::SkillMeta>& skills, const std::string& project_dir,
                         const std::optional<std::string>& home_dir);


// /skill 的参数只认第一个单词作动词,余下整段留给 URL、本地路径或技能名。命令
// 本身住 main.cpp,文件落盘与网络都压进 config::skill_store,这里不碰细节。
std::pair<std::string, std::string> SplitSkillCommandArgs(const std::string& args);

std::string JoinSkillNames(const std::vector<std::string>& names);

// list 动词走 tools::EnumerateSkillLayers 的四层全量账(官方/agents 共享/
// 主目录级/项目级,被遮蔽条目带标注),与右下角计数(RefreshSkills 的
// LoadSkills)同一套加载法则;install/update/remove 照旧压
// config::skill_store。home_dir 供枚举 <主目录>/.agents 与
// <主目录>/.lubancode 两处根。
bool HandleSkillCommand(const std::string& args, const std::filesystem::path& global_skills_root,
                        const std::filesystem::path& project_skills_root,
                        const std::optional<std::string>& home_dir = std::nullopt);


// /think(/effort 同义)命令:不带参数看当前档位,带参数切档位(本会话
// 生效)。M10 把档位放开成任意字符串——不在这儿拦,认不认得留给发请求
// 那一刻(responses 原样递,anthropic 查映射表、映射不上打警告)去判断,
// 原样存,不强制转小写(anthropic 那张映射表自己做大小写不敏感匹配,
// responses 要"原样递",这里转了小写反而破坏这条承诺)。
// 档位声明按三层找(Effort 诊断单):模型目录条目(entry)的
// supported_think_levels 最准;没有条目再看 provider 配置声明
// (provider_levels);都没有就明说"未经能力验证"——绝不甩一句
// "以服务商为准"完事。think_param 是 provider 声明的请求参数名(空 =
// reasoning_effort),一并展示。
void HandleThinkCommand(const std::string& args, const std::shared_ptr<std::string>& current_think,
                         const lubancode::config::ModelCatalogEntry* entry = nullptr,
                         const std::vector<std::string>& provider_levels = {},
                         const std::string& think_param = {});

// ---------------------------------------------------------------------------
// /think history default|all(Kimi 保留式思考单 P1):跨轮保留式思考的
// 配置入口。它不偷塞进 high/max 档——档位管本轮想多深,history 管跨轮
// 保留,两笔账分开记。
// ---------------------------------------------------------------------------

// /think history 的切换裁决(纯函数,单测钉各案):按当前模型方言与档位
// 判 requested 档能不能落。applied=false 时 mode 原样退回(不落账),notes
// 带拒绝的明报;applied=true 时 mode 是落定档,notes 带切换后的形状说明。
// 不猜:冲突(关思考还要保留)与不支持(K2.5/无方言旧端)都当场明报拒绝。
struct ThinkHistorySwitch {
    bool applied = false;
    lubancode::api::ReasoningHistoryMode mode = lubancode::api::ReasoningHistoryMode::ProviderDefault;
    std::vector<std::string> notes;
};
ThinkHistorySwitch DecideThinkHistorySwitch(lubancode::api::ReasoningHistoryMode current,
                                             lubancode::api::ReasoningHistoryMode requested,
                                             const lubancode::api::ReasoningConfig& reasoning,
                                             const std::string& effort);

// 切模型/切 provider/resume 之后重校验当前 history 选择:模型不支持、或与
// 关思考冲突时回 ProviderDefault 并打一行明报(不把 K2.6 的 keep 状态硬带
// 给 K3/K2.5)。返回是否发生了回落。entry 为空(模型不在目录)按不支持
// 处理——没有方言的旧端不猜。
bool RevalidateThinkHistoryMode(const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                                const std::shared_ptr<std::string>& current_think,
                                const lubancode::config::ModelCatalogEntry* entry);

// /think history 的会话内执行:value 空 = 亮当前形状;default|all = 切换
// 并落会话账(think_history_v1 事件行,/resume 恢复)。认不得的值给用法行。
void HandleThinkHistoryCommand(const std::string& value,
                               const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                               const std::shared_ptr<std::string>& current_think,
                               const lubancode::config::ModelCatalogEntry* entry,
                               lubancode::runtime::SessionRuntime* session_runtime);


// 把模型目录条目应用到会话状态:/model 切换(两个 explicit 都传 false,
// 目录声明了就用)和交互模式启动(explicit 按 Source 判断,用户显式配过的
// 不动)共用这一段。改 current_think / 会话窗口 / base_instructions,干了
// 什么就打一行;模型不在目录时 ComputeCatalogApplication 给回一份"全空"
// 的应用——think/窗口不动,base_instructions 清空(旧模型的指令不再发),
// 一切回退现状,不打任何多余的话。
void ApplyModelCatalog(const lubancode::config::ModelCatalog& catalog, const std::string& slug,
                        bool think_explicit, bool window_explicit,
                        const std::shared_ptr<std::string>& current_think,
                        lubancode::cli::ContextTracker& context_tracker,
                        const std::shared_ptr<std::string>& current_model_instructions);


// /model roles:打三档模型角色路由短表(模型分工第一期)。roles_table 是
// 会话 ModelRouter 的路由表,空指针时打"路由未建(单发/测试路径)"——不装
// 没事发生。
void PrintModelRolesTable(const lubancode::agent::ModelRouteTable* roles_table);


// /model 裸敲的清单选择:拿 QueryModels 的结果(终端侧取数已由调用方做完),
// 交互菜单/非交互编号选一项,返回选中的模型 id;取消或编号作废返回空
// (提示语已就地打出)。只管选——不切换、不碰配置,提交统一走
// runtime::CommandService::SetModel,与带参直切同一条路。
// catalog:列表优先显示目录条目的 display_name(其次接口给的
// display_name,最后 id 兜底),选完切换用的仍是 API 模型名。
std::optional<std::string> ChooseModelId(const lubancode::runtime::ModelQueryResult& query,
                                         const lubancode::config::ModelCatalog& catalog);

void PrintProviderList(const std::vector<lubancode::config::ProviderConfig>& providers,
                       const lubancode::config::Config& current_config,
                       const std::string& active_provider);


// /provider add 向导:跟 RunInitialSetupWizard(初次配置向导)同一套 WizardIO
// 建法——接 std::cout / cli::ReadLine / api::ListModels,不碰真实 IO 之外的
// 任何东西。问出来的是一条 ProviderConfig,写盘复用一行式旧用法同一条路径
// (AddProviderToGlobalConfig)。用户中途 EOF、或者最后一问回答 n,都当"整个
// 添加动作被取消"处理:不改 config.providers、不写盘。
// 成功写入后返回新 provider 名；取消或保存失败返回 nullopt。调用方可据此
// 在“尚无可用连接”的会话里当场切过去。
std::optional<std::string> RunProviderAddWizardInteractive(const std::string& name_prefill,
                                                            lubancode::config::Config& config,
                                                            const lubancode::cli::Theme& theme);


// provider 切换的正式执行(/provider switch 的 execute_switch 本体,与
// provider add 向导收尾自动切、/model 跨家连切同一条路):config.providers
// 里找名字、整套连接字段压进运行时 Config(wire/base_url/鉴权/model 一并
// 换)、记住 active_provider、重建 backend、重画横幅、应用目录与档位、
// rebuild_loop 重建皮。预检(鉴权齐备)由调用方做,这里不拦。找不到名字
// 打一行 not_found 返回 false;切换成了、"记住"没写进文件,照旧返回 true
// (与 /provider switch 同一口径:切换生效,记忆失败另说)。
bool ExecuteProviderSwitch(const std::string& switch_name, const std::string& switch_model,
                           lubancode::config::Config& config, std::string& active_provider,
                           RebuildableBackend& real_backend, std::string& session_wire,
                           const std::shared_ptr<std::string>& current_model,
                           const std::shared_ptr<std::string>& current_think,
                           const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                           lubancode::cli::ContextTracker& context_tracker,
                           const std::shared_ptr<std::string>& current_model_instructions,
                           const lubancode::config::ModelCatalog& catalog,
                           lubancode::agent::PromptOptions& prompt_options,
                           const std::function<void(bool)>& rebuild_loop, bool is_console,
                           const lubancode::cli::Theme& theme,
                           const std::optional<std::string>& active_provider_write_path,
                           lubancode::config::Source& active_provider_source);

// /provider:添端只写全局配置；项目级若自行写了 providers，加载时仍按既有
// "整段压过"规则优先。切端时换 client、提示词平台段与模型连接，旧历史
// 保留不动；成功后把端名写回配置，下次启动照旧选中。
void HandleProviderCommand(const std::string& args, lubancode::config::Config& config,
                           std::string& active_provider, RebuildableBackend& real_backend,
                           std::string& session_wire,
                           const std::shared_ptr<std::string>& current_model,
                           const std::shared_ptr<std::string>& current_think,
                           const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                           lubancode::cli::ContextTracker& context_tracker,
                           const std::shared_ptr<std::string>& current_model_instructions,
                           const lubancode::config::ModelCatalog& catalog,
                           lubancode::agent::PromptOptions& prompt_options,
                           const std::function<void(bool)>& rebuild_loop, bool is_console,
                           const lubancode::cli::Theme& theme,
                           const std::optional<std::string>& active_provider_write_path,
                           lubancode::config::Source& active_provider_source);


// /language 命令(i18n):裸敲列可选语言(内置两种 + 语言包)编号选;带参数
// 直接按语言码切。切换即时生效(会话级),有配置文件就问一句要不要写回
// (沿用 /model 那套 UpdateLanguageInConfigFile),没有就提示只在本会话生效。
void HandleLanguageCommand(const std::string& args, std::optional<std::string>& config_file_path);


// --config、/config 共用:打印最终生效的配置和每个字段的来源。session_model
// 有值时(/config 场景)额外打一行"本会话实际在用的 model"——/model 切换
// 只影响会话内存,不一定跟 config.model(四级合并出来的那份)一致。
// catalog 非空时(现在两个调用点都传)追加两行:模型目录路径 + 条目数,
// 以及"当前模型(会话在用的那个,没有就看 config.model)命没命中目录"。
void PrintConfigDiagnostics(const lubancode::config::ConfigResult& result,
                             const std::optional<std::string>& session_model = std::nullopt,
                             const lubancode::config::ModelCatalog* catalog = nullptr,
                             const lubancode::config::SettingsLocal* settings = nullptr);

// /keymap(终端接线收尾单自大类搬出):列/set/reset 三路,落盘用户级
// ~/.lubancode/keymap.json。home_lubancode 是用户级 .lubancode 目录
//(空 = 没主目录,改绑只活本进程)。
void HandleKeymapCommand(const std::string& raw_args, const std::optional<std::string>& home_lubancode,
                         const lubancode::cli::Theme& theme);

// /copy [plain](终端接线收尾单自大类搬出):复制上一段完整答话到剪贴板。
// history 是活对话账,倒着找最近一条有正文的 assistant 消息。
void HandleCopyCommand(const std::string& raw_args, const std::vector<lubancode::api::Message>& history,
                       const lubancode::cli::Theme& theme);

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):设置域的分派位(provider/model/config/
// update/language/think/skills/skill/keymap/copy)。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;

CommandFlow HandleSlashProvider(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashConfig(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashUpdate(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashLanguage(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashThink(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashSkills(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashSkill(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashKeymap(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashCopy(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
