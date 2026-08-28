// 交互循环里,输入以 `/` 开头的走命令分发,不发给模型。这里只管"识别是哪个
// 命令、参数是什么"这一件纯逻辑的事(输入串 -> 命令枚举 + 参数),不管命令
// 具体怎么执行——执行逻辑留给 main.cpp(要碰 AgentLoop、要读用户主目录之类,
// 不是纯函数)。

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

enum class SlashCommand {
    NotSlash,  // 不是以 / 开头,交互循环不该拦截,原样发给模型
    Help,
    Model,
    Provider,  // /provider add|list|switch|remove:多端模型服务配置
    Config,
    Update,    // /update [check]:查 GitHub 最新 Release，不暗中改文件
    Init,      // /init:在项目根创建 AGENTS.md 并立刻载入
    Clear,
    Exit,
    Context,  // /context [档位]:看当前上下文占用,或临时改窗口大小
    Compact,  // /compact [重点说明]:手动触发一次历史压缩
    Think,    // /think [档位]:看/改推理强度
    Skills,   // /skills:列出扫描到的技能(M9)
    Skill,    // /skill install|list|update|remove:本地/远端技能分发
    Mcp,      // /mcp:列出挂载的 MCP 服务器状态、工具清单(M8)
    Lsp,      // /lsp:列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)
    Todos,    // /todos:查看当前待办清单(M11/0.10.0)
    Plugins,  // /plugins:列出插件三路(native/Lua/process)与加载警告(plugins 单第 8 步扩)
    Plugin,   // /plugin inspect|doctor|test|reload|enable|disable <id>:单插件管理面
    Tools,    // /tools:列工具三态——核心(恒在)/已加载/延迟未加载(tool_search)
    Memory,   // /memory:项目记忆开关、查看、显式记忆、遗忘与重建
    Sessions,  // /sessions:列最近的会话存档(本目录;/sessions all 列全部目录)
    Resume,    // /resume <编号或id>:载入某场存档历史续聊
    Archive,   // /archive:归档当前会话(刷盘关柄→搬 archive/→退出,第四步)
    Delete,    // /delete:永久删除当前会话(先确认;回合在跑拒绝,第五步)
    Export,    // /export [路径]:当前会话导出 Markdown
    Copy,      // /copy [plain]:复制上一段完整答话(默认原始 Markdown,plain 纯文本)
    Title,     // /title [标题]:看/设当前会话标题(追加 title 事件行,最后一条胜)
    Soul,      // /soul [内容|clear|名字|off|default]:魂(风格叠加层)查看/设置
    Prompt,    // /prompt [reset]:看法(系统提示词)的来源,或还原 system_prompt.md
    Language,  // /language [语言码]:列可选界面语言/切换(i18n)
    Image,     // /image <路径...>:附一张或多张本地图片
    Worktree,  // /worktree new|list|exit:隔离工作树会话
    Background,  // /background:列后台任务清单(run_command run_in_background 起的)
    Hooks,  // /hooks:hooks 来源/命令/信任/禁用/最近结果与运行记录
    Record,   // /record start|note|pause|resume|stop|cancel|status|list|install|discard:录一遍生成技能
    Peers,   // /peers:列同机可见的其它会话(跨会话传话),方向键菜单
    Send,    // /send <名字或 peer_id> <话>:给另一场会话递一张字条
    Peerperm,  // /peerperm auto|accept|hold|refuse:跨会话来信的权限档
    Doctor,  // /doctor effort|cache:本地兼容端 Effort/前缀缓存诊断(探针要发请求)
    Keymap,  // /keymap [set 动作 和弦|reset [动作|all]]:看/改键位(用户级落盘)
    Workflow,  // /workflow list|show|graph|validate|run|resume|cancel|history|...:自然语言编排的图
    Trace,     // /trace [errors|<execution_id>|toolu <id>|turn <id>]:工具逐枚追踪账(逐枚追踪单)
    Goal,      // /goal [objective|status|edit|pause|resume|clear]:持久目标(持久目标单)
    Loop,      // /loop [interval] [prompt]|list/status/pause/resume/stop/run:会话定时循环(loop 单)
    Plan,      // /plan [正文|status|off|review]:只读研究模式(Plan 模式单)
    Package,  // /package list|show|doctor:Package 只读面(统一 Package 封装单阶段 1)
    Evolve,   // /evolve status|list|show:自进化观察账只读面(自进化闭环阶段 1)
    WorkflowAlias,  // /<workflow-alias> <args>:直呼已装 Workflow(运行时查 catalog)
    Agents,   // /agents:列 Agent Catalog(自定义 Agent 单阶段 1,只读)
    Agent,    // /agent doctor <名字>:单 Agent 静态预检(阶段 1 只读骨架)
    Unknown,  // 以 / 开头,但不认得这个命令
};

struct ParsedSlashCommand {
    SlashCommand command = SlashCommand::NotSlash;
    std::string args;      // 命令词后面剩下的部分,已剥两端空白;没有就是空串
    std::string raw_word;  // 原始命令词(小写化之前),Unknown 时用来提示"XXX 不认得"
    // WorkflowAlias 时:命令词剥掉 '/' 后的原文(保留大小写,alias 是
    // Unicode 敏感的);具体查不查得进 catalog 由会话层定,parser 只认
    // "不是内建词"这一件事。
    std::string alias_word;
};

// 纯函数:识别一行输入是不是 slash 命令、是哪一个、参数是什么。
// 命令词大小写不敏感(/Model 和 /model 视为一样);命令词和参数之间按第一个
// 空白切开。输入前后空白先剥掉。
ParsedSlashCommand ParseSlashCommand(const std::string& input);

// /provider 的二级参数也收在 cli 层做纯解析，main.cpp 只接收已拆好的
// 字段、做写盘和切会话。model / window 选项都只认一个值；少参数、重复
// 选项、夹生子命令一律 Invalid，由调用方统一打印用法。
enum class ProviderCommandAction {
    Invalid,
    List,
    Refresh,
    Add,
    Switch,
    // 向导重排单:裸敲 /provider switch(TTY 开选择器;非 TTY 由调用方给
    // switch 专用短用法),不再落进 Invalid -> 总帮助那条死路。
    SwitchInteractive,
    Remove,
    Set,
    // 容错单:/provider edit <名字> 进向导面板改旧 provider;裸敲 edit(TTY
    // 开选择列表,选中进编辑;非 TTY 给 edit 专用短用法)。
    Edit,
    EditInteractive
};

struct ParsedProviderCommand {
    ProviderCommandAction action = ProviderCommandAction::Invalid;
    std::string name;
    std::string base_url;
    std::string wire;
    std::string key_env = "ANTHROPIC_AUTH_TOKEN";
    std::string key;     // --key:明文 api_key,可选,一行式旧用法的新增项
    std::string model;
    std::string effort;  // --effort:model_reasoning_effort,可选
    std::string window;
    // action == Add 且 wizard == true 时,说明这是"裸敲 /provider add"或
    // "/provider add 名字"这两种触发分步向导的写法(words.size() <= 2),
    // main.cpp 据此走 RunProviderAddWizard 而不是一行式解析结果。
    bool wizard = false;
    // action == Set 时才有意义:field 是要设的字段名(已小写化,认
    // native_web_search / extra_body / extra_header 三种)。
    //   - native_web_search:固定四个词,value 是第四个词(已小写化,on/
    //     off/true/false/1/0 这类写法留给 main.cpp 调
    //     config::ParseBoolToggle 去解读)。
    //   - extra_body:value 是"字段名之后、到这一行结尾"的原始文本(保留
    //     大小写、空白、花括号里的一切——一坨 JSON 不能按空格切词),合不
    //     合法留给 main.cpp/config 层解析。
    //   - extra_header:header_name 是紧跟 extra_body/extra_header 后面
    //     那一个词(保留原始大小写,HTTP 头名字大小写有意义,不替用户
    //     改掉);value 是 header_name 之后、到行尾的原始文本(可以有
    //     空格),空串表示删除这一条头。
    // 这层只管拆词,不管字段认不认得、值合不合法,跟 wire/window 那两个
    // 字段同一个分工路数。
    std::string field;
    std::string header_name;
    std::string value;
    // action == Invalid 时才有意义:第一个词的原始拼写(保留大小写)。容错
    // 单用它做编辑距离近邻——已知子命令敲错参(如 "refresh now")时它等于
    // 该子命令本身,调用方据此改打"参数不对 + 该子命令短用法"。
    std::string bad_word;
};

ParsedProviderCommand ParseProviderCommand(const std::string& args);

// ---------------------------------------------------------------------------
// /provider 子命令容错(容错单):拼错的子命令(swtich/lst/remvoe)不再倒
// 13 行总表,给一句"是不是想敲 X?"+ X 的专用短用法。匹配是纯函数,单测
// 钉死。
// ---------------------------------------------------------------------------

// 已知子命令清单,按提示时的推荐序排列(近邻距离打平时取排前面的)。
std::vector<std::string> ProviderSubcommands();

// 编辑距离(Levenshtein,大小写不敏感)最近邻:距离 <= 2 的已知子命令;
// 没有近邻返回 std::nullopt。word 本身是已知子命令时原样返回(距离 0)。
// 只建议,不替用户执行——"不做模糊自动执行"是规格明令。
std::optional<std::string> NearestProviderSubcommand(const std::string& word);

// 某个子命令的专用短用法(一行)。认不得的子命令返回空串。i18n 走
// cmd.provider.usage_short.<子命令> 一族(switch 复用既有的
// cmd.provider.switch.usage_short)。
std::string ProviderSubcommandUsageLine(const std::string& subcommand);

// /record 的二级参数,同样收在 cli 层做纯解析:拆出动作、名字/编号、note
// 的原文(install|discard 的第三个词管装到哪一级)。缺参数、认不得的动作
// 一律 Invalid,由调用方统一打印用法。
enum class RecordCommandAction {
    Invalid, Status, Start, Note, Pause, Resume, Stop, Cancel, List, Install, Discard
};

struct ParsedRecordCommand {
    RecordCommandAction action = RecordCommandAction::Invalid;
    std::string name;   // start 的技能名 / install|discard 的录制件编号
    std::string text;   // note 的备注原文(保留空格)
    bool to_project = true;  // install 目标:默认项目级;第三词是 "home" 才装主目录级
};

ParsedRecordCommand ParseRecordCommand(const std::string& args);

// ---------------------------------------------------------------------------
// /goal 的二级参数(持久目标单)。纯解析:拆出动作与 objective 正文,不管
// 目标建不建、状态机怎么走。`--` 消歧("/goal -- pause all jobs" 里 pause
// 是正文不是子命令);objective 保留换行与大小写,不做 shell 拆词、不展开
// $VAR/反引号/@file。空 objective、认不得的子命令一律 Invalid。
// ---------------------------------------------------------------------------
enum class GoalCommandAction {
    Invalid,
    View,     // 裸 /goal:看账,不发模型
    Status,   // /goal status:同 View(结构化全账)
    Create,   // /goal <objective>:创建并启动
    Edit,     // /goal edit <objective>:改目标(revision+1)
    Pause,    // /goal pause:停排新 iteration
    Resume,   // /goal resume:从最后 checkpoint 续
    Clear,    // /goal clear:先确认再摘掉
};

struct ParsedGoalCommand {
    GoalCommandAction action = GoalCommandAction::Invalid;
    std::string objective;   // Create/Edit 的正文(保留多行);其余动作空
    bool dashdash = false;   // 用过 `--` 消歧
    std::string bad_word;    // Invalid 时第一词的原始拼写(容错提示用)
};

ParsedGoalCommand ParseGoalCommand(const std::string& args);

// ---------------------------------------------------------------------------
// /loop 的二级参数(loop 单)。纯解析:拆出动作、interval、prompt 正文,
// 不管 task 建不建、scheduler 怎么走。规矩(单子"命令解析"节):
//   - 第一个 token 严格等于 list/status/pause/resume/stop/run 时当子命令
//     (status 需带 id 或 all;pause/resume/stop/run 需带 id 或 all)。
//   - `--` 消歧:"/loop -- stop deployment if red" 里 stop 是正文;
//     "/loop 5m -- list open bugs" 的 -- 后全算 prompt,保留空白。
//   - 第一个 token 是 interval 形状(5m/2h/1d,大小写不敏感)才当间隔;
//     "5migrate" 这类普通词不当 interval。interval 后没正文 = 用
//     loop.md/内置 prompt(prompt_source=File 兜底,装配层现解析)。
//   - 没 interval 的正文按默认 10m 创建。
//   - inline prompt 以 '/' 开头拒绝(首版不许调度 slash 命令)。
//   - task id 收宿主全 id(loop-3)或裸数字(3);展示恒用全 id。
// ---------------------------------------------------------------------------
enum class LoopCommandAction {
    Invalid,
    List,     // /loop list:列全部 task
    Status,   // /loop status <id|all>:看一只/全部的账
    Pause,    // /loop pause <id|all>
    Resume,   // /loop resume <id|all>
    Stop,     // /loop stop <id|all>
    Run,      // /loop run <id>:立即补一拍
    Create,   // /loop [interval] [prompt]:建定时任务
};

struct ParsedLoopCommand {
    LoopCommandAction action = LoopCommandAction::Invalid;
    std::string task_ref;    // 子命令的目标 id("loop-3"/"3"/"all");Create 空
    std::string prompt;      // Create 的正文(保留多行);空 = 用 loop.md/内置
    std::string interval_text;  // 原始 interval token("5m");空 = 默认 10m
    bool dashdash = false;   // 用过 `--` 消歧
    std::string bad_word;    // Invalid 时第一词的原始拼写(容错提示用)
    std::string error_hint;  // Invalid 时的人话(给 usage 提示用)
};

ParsedLoopCommand ParseLoopCommand(const std::string& args);
// /plan 的二级参数(Plan 模式单):纯解析,语义分四路——
//   Enter          裸敲 /plan:切进 Plan;description 留空
//   EnterWithTask  /plan <正文>:切进 Plan 并把正文当规划请求(description)
//   Status         /plan status:看当前档、最近计划与审批状态
//   Off            /plan off:不批准计划,单纯退到 Default
//   Review         /plan review:重开最近计划的审阅框
//   Invalid        认不得的子词(usage 交调用方打)
// 命令只在空闲 composer 生效;任务跑着时不半腰切(会话层拦,这里只拆词)。
// ---------------------------------------------------------------------------
enum class PlanCommandAction { Invalid, Enter, EnterWithTask, Status, Off, Review };

struct ParsedPlanCommand {
    PlanCommandAction action = PlanCommandAction::Invalid;
    std::string description;  // EnterWithTask 时的规划请求正文(保留空白)
};

ParsedPlanCommand ParsePlanCommand(const std::string& args);

// /provider remove 的会话级护栏。拆成纯函数，命令处理与单测共用，免得
// "当前端不许删"这条规矩散在 main 的 IO 分支里。
bool CanRemoveProvider(const std::string& active_provider, const std::string& name);

// 一个命令一条:名字 + 一句话说明。/help 打印用的就是这份,line_editor 的
// Tab 补全、实时提示行也是从这份转过去的候选——统共这一份定义,不重复写。
// i18n:说明文字经 tr(slash.desc.*)取值,/language 切换后下一次调用重建,
// 返回引用在下一次语言切换之前有效。
struct SlashCommandInfo {
    std::string name;
    std::string description;
};
const std::vector<SlashCommandInfo>& AllSlashCommands();

// 把 AllSlashCommands 排成帮助清单(每命令一行:"  /名字<补白>一句话说明",
// 名字列按最长命令名对齐)。顶层 --help 的斜杠命令节与交互内 /help 的正文
// 都打这份(P3-2:三份名单——--help、/help、Tab 补全——同出 AllSlashCommands,
// 不许各列各的);行列格式在这收口一份,两边不再手抄。
std::vector<std::string> FormatSlashCommandListLines();

}  // namespace lubancode::cli
