#include "cli/i18n.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lubancode::cli {

namespace {

struct Entry {
    const char* key;
    const char* value;
};

// ---------------------------------------------------------------------------
// zh-CN:全量表。值与 0.19.0 的字面文案逐字节一致——管道回归靠这一条。
// ---------------------------------------------------------------------------
const Entry kZhCN[] = {
    {"language.name", "中文(zh-CN)"},

    // ---- 帮助(--help) ----
    {"help.title", "lubancode {0} - C++ AI 编程 CLI"},
    {"help.usage",
     "用法:\n"
     "  lubancode [选项]\n"
     "  lubancode \"问题\"          一次问答,能用工具就用工具\n"
     "  lubancode plugin init python [名字]\n"
     "                              生成 Python 插件脚手架(plugin.json + runner.py + 单测模板,\n"
     "                              落 ~/.lubancode/plugins/<名字>/)\n"
     "  lubancode                  不带参数则进入交互循环;首次运行缺配置会先走一遍初次配置\n"
     "                              向导,配完直接进入会话,不用重启。exit/quit 或 EOF(Ctrl+Z /\n"
     "                              管道读尽)退出;空行只是重新给提示符,不退出\n"
     "  lubancode archive <id>    归档一场会话(搬进 sessions/archive/,字节原样,想恢复用\n"
     "  lubancode unarchive <id>  unarchive;归了的场子不进默认列表)\n"
     "  lubancode delete <id>     永久删除一场会话,交互确认后才删;--force 跳过确认,只给\n"
     "                            脚本显式使用,不可恢复\n"},
    {"help.options",
     "选项:\n"
     "  --version              打印版本号\n"
     "  --check-update         检查 GitHub 最新 Release，打印结果后退出\n"
     "  --help                 打印本帮助\n"
     "  --yes                  自动确认所有需要确认的工具调用(比如 run_command),不再逐条询问\n"
     "  --continue             交互模式启动时自动恢复本目录最近一场会话存档(等价开场 /resume\n"
     "                         最近一场);本目录没有存档就正常开新会话\n"
     "  --config               打印最终生效的配置(api_key 打码)和每个字段来自哪一级,排查配置问题用\n"
     "  --system-prompt <文件> 用这个文件(.md/.txt,UTF-8)替换默认系统提示的人格段,工作目录、\n"
     "                         工具调用这些运行必需的上下文照样追加,不受影响。压过配置文件里的\n"
     "                         system_prompt_file 字段,也压过 ~/.lubancode/system_prompt.md\n"
     "  --reset-system-prompt  把 ~/.lubancode/system_prompt.md(法)还原成内置默认,旧文件挪成\n"
     "                         system_prompt.md.bak(同 /prompt reset,只是不进交互、打结果就退)\n"},
    {"help.scaffold",
     "魂法分家(首次启动自动生成,缺了每次启动补齐,已存在不覆盖):\n"
     "  ~/.lubancode/system_prompt.md  \"法\"——系统提示词人格段,改它定制行为;内容剥掉顶部注释后\n"
     "                                 全空白就回退内置默认\n"
     "  ~/.lubancode/SOUL.md           \"魂\"——风格叠加层,写点风格指令(如\"只用文言文答话\"),\n"
     "                                 注入在系统提示最后;留空 = 无效果\n"
     "  ~/.lubancode/souls/*.md        备选魂(自带 wenyan.md 文言示例),交互模式 /soul 名字 切换\n"},
    {"help.slash",
     "交互模式里,输入以 / 开头的一行走命令,不发给模型:\n"
     "  /help           列出所有命令\n"
     "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
     "  /model 名字     直接切到指定模型名,不用拉列表\n"
     "  /model roles    查看 normal/cheap/lao 三档任务路由与配置来源\n"
     "  /provider       列已配服务端;/provider add|switch|edit|remove|set|refresh 管多端模型(refresh 刷厂家目录)\n"
     "  /config         打印当前生效配置(复用 --config 的逻辑),外加本会话实际在用的 model\n"
     "  /update         检查 GitHub 最新 Release；升级安装时一并同步官方技能\n"
     "  /init           在项目根生成 AGENTS.md,并让主代理、子代理立即采用\n"
     "  /language       列可选界面语言并切换(内置 zh-CN/en,languages/*.json 可扩展)\n"
     "  /worktree       新建/列出/退出隔离工作树;/worktree new [名字] | list | exit keep|remove\n"
     "  /clear          清空对话历史\n"
     "  /context        看当前上下文占用分析(系统提示/工具定义/对话历史分类明细 + 条形图)\n"
     "  /context 512k   临时改窗口大小(256k/512k/1m/裸数字都认),只本会话生效\n"
     "  /compact [重点说明]  手动触发一次历史压缩,可选指定这次额外保留什么\n"
     "  /think          看当前推理强度(/effort 同义);不填是正式状态,请求里字段缺席\n"
     "  /think 档位     切推理强度;档位列模型目录/provider 声明,没声明就明说未经能力验证\n"
     "                  (anthropic 映射 none/low/medium/high/xhigh/max,responses 原样递)\n"
     "  /skills         列出扫描到的技能(官方 + 主目录级 + 项目级)\n"
     "  /skill          管技能;裸敲看安装网址、本地目录、更新与删除示例\n"
     "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
     "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
     "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
     "  /plugins        列出插件三路(native/Lua/process)的状态与加载警告\n"
     "  /plugin         管单枚插件:inspect 看详情 / doctor 查环境 / test 试跑\n"
     "                  (v1 以重启为 reload/enable/disable 的口径)\n"
     "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(工具总数超过配置文件\n"
     "                  tool_search_threshold(默认 20,0=永不延迟)时,MCP/插件等外挂工具\n"
     "                  延迟挂载,模型用 tool_search 检索后方可调用)\n"
     "  /memory         管项目记忆开关、召回、后台写入、列表、遗忘与索引重建\n"
     "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
     "  /resume         上下选择本目录会话并恢复历史;也可跟编号或 id\n"
     "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
     "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
     "  /soul           看当前魂;/soul 内容 写进 SOUL.md 并即时生效,/soul clear 清空还原默认\n"
     "                  /soul 名字 仍可切换已有备选魂,/soul off 关,/soul default 回 SOUL.md\n"
     "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
     "  /background     列后台命令任务清单(编号/状态/PID/命令/日志);/bg 同义\n"
     "  /record         录一遍生成技能;/record start 名字 开录,stop 出草稿,裸敲看全部子命令\n"
     "  /peers          列同机可见的其它 Lubancode 会话(名字/状态/目录);方向键选,Enter 看详情\n"
     "  /send           /send <名字或短id> <话>:给另一场会话递一张纯文本字条\n"
     "  /peerperm       /peerperm auto|accept|hold|refuse:设跨会话来信的收件档\n"
     "  /image 路径     附本地图片(也可在消息里写 @路径；支持 png/jpg/jpeg/gif/webp，每张不超过 5MB)\n"
     "  Shift+Enter     输入框里插一个换行,写多行消息(Alt+Enter 同义;注意 Windows Terminal\n"
     "                  默认把 Alt+Enter 绑成全屏切换、会吞掉这个键,用 Shift+Enter 最稳);\n"
     "                  Enter 把整段(多行拼换行)一次发出,空白内容按 Enter 原地不动\n"
     "  粘贴内容        1000 字符内直接显示;超过后折成 [粘贴内容 N 字符],提交时展开\n"
     "  ESC             流式回复期间按下:打断当前这轮回答,已出的半截话保留、下一轮能接着聊;\n"
     "                  空闲时按下:清空正在编辑的整段输入;确认提示 [y/a/N] 下按下:等同拒绝;\n"
     "                  聚焦查看(Ctrl+E)画面里按下:返回会话\n"
     "  / 后按 ↓↑       进入候选菜单直选(↓ 选中第一条,↓↑ 循环移动),Enter 执行选中命令\n"
     "                  (已敲的参数尾巴原样保留),继续打字/退格/ESC 回普通编辑\n"
     "  Ctrl+O          紧凑/详细全局切换:把全部工具条目按新档整块重打(详细 = 完整参数\n"
     "                  JSON + 完整输出/diff 全文),再按切回\n"
     "  Ctrl+E          聚焦查看当前焦点条目(无焦点则最近一条)全文;再按 Ctrl+E 或 ESC 返回\n"
     "  Tab             输入框有内容:补全/轮转 slash 命令(现职);输入框为空:进入焦点态\n"
     "                  并选中最近一条工具条目;焦点态内 Tab 往旧走、Shift+Tab 往新走,\n"
     "                  ESC/Enter 退出焦点态回编辑\n"
     "  Shift+Tab       循环切确认档(confirm/auto/yolo)——任何时候都是,跟状态行提示\n"
     "                  一致;只有焦点态内例外(那里是焦点往新走)。auto:文件与安全命令\n"
     "                  放行,危险命令与外挂工具确认;yolo:全部放行\n"
     "  流式期间打字回车  不会打断当前流,而是排进输入框上方的待发队列;当前工具收尾、\n"
     "                  结果入账后、下一次请求发出前,按排队顺序送进同一轮对话(队列区\n"
     "                  标题写明送达时机)。Esc 打断并立即送;Shift+← 把最新一条取回编辑\n"
     "                  (Enter 原位替换、Esc 还原、Del 再按一次删除;上键取回保留作别名)\n"
     "  /exit           退出(裸词 exit/quit 也认)\n"},
    {"help.config",
     "配置优先级(从高到低,按字段逐个决,不是整套配置一刀切):\n"
     "  1) LUBANCODE_ 专属环境变量\n"
     "       LUBANCODE_WIRE          协议选择,anthropic / responses / chat_completions\n"
     "       LUBANCODE_BASE_URL      API 地址\n"
     "       LUBANCODE_API_KEY       认证令牌\n"
     "       LUBANCODE_MODEL         模型名\n"
     "       LUBANCODE_MAX_CONTEXT   history 裁剪阈值(字符数,老的硬安全网)\n"
     "       LUBANCODE_THEME         终端配色主题,dark / light / plain\n"
     "       LUBANCODE_LANG          界面语言,zh-CN / en / languages/ 里的语言码;空 = 跟系统\n"
     "       LUBANCODE_SYSTEM_PROMPT_FILE  人格文件路径,同 --system-prompt(命令行参数压过这个)\n"
     "       LUBANCODE_CONTEXT_WINDOW      上下文窗口 token 数,256k/512k/1m/裸数字\n"
     "       LUBANCODE_COMPACT_MODEL       压缩用的模型,空 = 跟当前会话模型一致\n"
     "       LUBANCODE_THINK               推理强度,档位以服务商为准,空 = 不发这个参数\n"
     "       LUBANCODE_SOUL                魂的名字,default = SOUL.md,off = 不叠加,别的名字\n"
     "                                     = souls/<名字>.md\n"
     "  2) 配置文件(第一个找到的生效,查找顺序:cwd 的 .lubancode/config.json → 主目录的\n"
     "     .lubancode/config.json → cwd 的旧位置 .lubancode.json → 主目录的旧位置\n"
     "     .lubancode.json;读到旧位置会自动挪到新位置)。字段:wire / base_url / api_key / model /\n"
     "     max_context_chars / theme / language / system_prompt_file / context_window / compact_model /\n"
     "     think,全部可选。providers 也是配置文件整段:每项写 name / base_url / wire / key_env / model / context_window,\n"
     "     key_env 只记环境变量名,不存密钥;项目级 providers 整段压过全局。另有 hooks / mcpServers / search 三段(只从配置文件读,没有环境变量、没有内置默认值):\n"
     "       \"mcpServers\": {\"服务器名\": {\"command\": \"...\", \"args\": [...], \"env\": {...}}}\n"
     "       起进程握手成功后,工具以 mcp__服务器名__工具名 挂进工具表,/mcp 看状态\n"
     "       \"search\": {\"provider\": \"tavily|brave|serper\", \"api_key\": \"...\"}\n"
     "       配了这一段才会注册 web_search 工具;web_fetch 工具无须配置,始终可用\n"
     "     再有 lsp 一段(同样只从配置文件读;没配 = 不启用 = lsp 工具不注册):\n"
     "       \"lsp\": {\"cpp\": {\"command\": \"clangd\", \"args\": [...], \"extensions\": [\".cpp\", \".hpp\"],\n"
     "                \"idle_minutes\": 10}}\n"
     "       配了才注册 lsp 工具(definition/references/symbols/diagnostics 语义查询),懒启动、\n"
     "       闲置自动关停,/lsp 看各语言服务器状态\n"
     "  3) 通用环境变量(向后兼容旧用法,跟 Claude Code 等工具共用同名变量时容易撞车,\n"
     "     建议改用第 1 级的 LUBANCODE_* 专属变量):\n"
     "       wire=anthropic 时读 ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN / ANTHROPIC_MODEL\n"
     "       wire=responses 时读 OPENAI_BASE_URL / OPENAI_API_KEY / OPENAI_MODEL\n"
     "  4) 内置默认值:wire=anthropic、max_context_chars={0}、theme={1}、context_window={2}。\n"
     "     base_url/api_key/model/system_prompt_file/compact_model/think 不绑死任何一家模型服务,\n"
     "     没有内置默认值——四级都没配到,交互模式会自动走初次配置向导;单发模式/管道模式会直接\n"
     "     报错,提示三条配置途径。用 --config 能看到当前实际生效的配置和每个字段的来源。\n"},

    // ---- /help(交互内帮助) ----
    {"slash_help.body",
     "可用命令:\n"
     "  /help           列出所有命令\n"
     "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
     "  /model 名字     直接切到指定模型名,不用拉列表\n"
     "  /model roles    查看 normal/cheap/lao 三档任务路由与配置来源\n"
     "  /provider       列已配服务端;/provider add|switch|edit|remove|set|refresh 管多端模型(refresh 刷厂家目录)\n"
     "  /config         打印当前生效配置(api_key 打码),外加本会话实际在用的 model\n"
     "  /update         检查 GitHub 最新 Release；升级安装时一并同步官方技能\n"
     "  /init           在项目根生成 AGENTS.md,并让本会话立即采用\n"
     "  /language       列可选界面语言并切换;/language 语言码 直接切(会话级,可写回配置)\n"
     "  /worktree       新建/列出/退出隔离工作树;/worktree new [名字] | list | exit keep|remove\n"
     "  /clear          清空对话历史\n"
     "  /context        看当前上下文占用;/context 256k|512k|1m 临时改窗口大小\n"
     "  /compact        手动压缩历史;/compact 重点说明 可指定这次额外保留什么\n"
     "  /think          看当前推理强度;/think 档位 切档位(声明未知的档会如实标注,/effort 同义)\n"
     "  /skills         列出扫描到的技能(官方 + 主目录级 + 项目级)\n"
     "  /skill          管技能;裸敲看安装网址、本地目录、更新与删除示例\n"
     "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
     "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
     "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
     "  /plugins        list plugins of all three runtimes (native/Lua/process) with load warnings\n"
     "  /hooks          hooks 台账:来源/命令/信任/禁用/最近结果;trust|untrust|disable|enable <#id>、runs [N]\n"
     "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(tool_search 延迟挂载)\n"
     "  /memory         管项目记忆;/memory on|off|use|learn|list|remember|forget|rebuild\n"
     "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
     "  /resume         上下选择本目录会话并恢复历史;也可跟编号或 id,后续消息写回原文件\n"
     "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
     "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
     "  /soul           看当前魂;/soul 内容 写进 SOUL.md 并即时生效,/soul clear 清空还原默认\n"
     "                  /soul 名字 仍可切换已有备选魂,/soul off 关,/soul default 回 SOUL.md\n"
     "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
     "  /background     列后台命令任务清单(编号/状态/PID/命令/日志);/bg 同义\n"
     "  /record         录一遍生成技能;/record start 名字 开录,stop 出草稿,裸敲看全部子命令\n"
     "  /peers          列同机可见的其它 Lubancode 会话(名字/状态/目录);方向键选,Enter 看详情\n"
     "  /send           /send <名字或短id> <话>:给另一场会话递一张纯文本字条\n"
     "  /peerperm       /peerperm auto|accept|hold|refuse:设跨会话来信的收件档\n"
     "  /image 路径     附本地图片(也可在消息里写 @路径；支持 png/jpg/jpeg/gif/webp，每张不超过 5MB)\n"
     "  /exit           退出(裸词 exit/quit 也认)\n"
     "多行输入:Shift+Enter 插换行(Alt+Enter 同义,但 Windows Terminal 默认把它绑成全屏\n"
     "切换、会吞掉,推荐 Shift+Enter);Enter 发送整段;多行时首行的 / 是正文,不当命令。\n"
     "排队消息:流式期间打字回车不另开一轮,排进输入框上方的待发队列,当前工具收尾后\n"
     "送进同一轮(队列区标题写明时机);Esc 打断并立即送;Shift+← 取回编辑(Enter 原位\n"
     "替换、Esc 还原、Del 再按一次删除)。\n"
     "粘贴内容在 1000 字符内直接显示;超过后折成 [粘贴内容 N 字符],提交时展开原文。\n"
     "候选菜单:/ 开头时按 ↓ 进入直选(↓↑ 循环移动,Enter 执行选中命令、已敲的参数尾巴\n"
     "原样保留;打字/退格/ESC 回普通编辑);Tab 补全/轮转照旧。\n"
     "条目查看:Ctrl+O 紧凑/详细全局切换(详细 = 完整参数 + 输出/diff 全文,整块重打);\n"
     "Shift+Tab 任何时候都是切确认档(confirm/auto/yolo,状态行实时显示;auto 档文件与\n"
     "安全命令放行,危险命令与外挂工具确认);输入框为空时\n"
     "Tab 进入焦点态选最近一条,焦点态内 Tab 往旧走、Shift+Tab 往新走(这时不切档),\n"
     "ESC/Enter 退出焦点态回编辑,有内容时 Tab 维持补全现职;Ctrl+E 聚焦查看焦点条目\n"
     "全文(无焦点则最近一条),再按 Ctrl+E 或 ESC 返回;流式输出期间这些键不响应。\n"},

    // ---- 横幅 / 状态行 / 输入层 ----
    {"banner.hint", "输入问题回车发送,exit 退出,/help 看命令"},
    {"status.mode.confirm", "确认模式"},
    {"status.shift_tab_hint", "(shift+tab 切换)"},
    {"mode.confirm", "确认"},
    {"ui.menu_more", "  … 共 {0} 个命令"},
    {"input.interrupted", "[已打断]"},
    {"input.queued", "[已排队] "},
    {"input.queue_more", "另有 {0} 条"},
    // ---- 0.28.x 排队消息(工具边界送达 + Shift+左键取回编辑) ----
    {"queue.key_hint", "Shift+←"},
    {"queue.key_hint_fallback", "Shift+← / Ctrl+←"},
    {"queue.title.boundary", "待送消息:下一次工具调用后送出 · Esc 打断并立即送 · {0} 取回编辑"},
    {"queue.title.end_of_turn", "待送消息:本轮收尾后送出 · {0} 取回编辑"},
    {"queue.title.immediate", "正在打断并送达……"},
    {"queue.title.editing", "正在编辑排队消息 · Enter 原位替换 · Esc 还原 · Del 再按一次删除"},
    {"queue.mark.editing", "[编辑中] "},
    {"queue.mark.target", "[#{0}] "},
    {"queue.mark.target_gone", "[目标已结束] "},
    {"queue.mark.failed", "[发送失败] "},
    {"queue.commit_conflict", "这条消息已送达或已变动,修改未保存;编辑器里的正文保留为新消息。"},
    {"queue.edit_blocks_panel", "先 Enter/Esc 了结排队消息的编辑,再操作面板。"},
    {"queue.delete_armed", "再按一次 Del 删除这条排队消息(Esc/超时取消)"},
    {"queue.disposal_head", "未送出的排队消息 {0} 条,已随本次退出/清场一并丢弃:"},
    {"queue.disposal_preview", "  首条 [{0}] {1}"},
    {"queue.archive_head", "排队消息 {0} 条未送出,已随会话存档带走,/resume 可接回:"},
    {"queue.autosend_returned", "你排队的消息没送达,已回队(不再自动重发):{0}"},
    {"input.pasted_content", "[粘贴内容 {0} 字符]"},
    {"input.ctrlc_exit", "[已退出]"},
    {"stream.hint", "键入并回车 排队下一条 · Esc 打断"},
    {"stream.hint.plain", "键入并回车排队 · Esc 打断"},
    {"footer.repaint_unsupported",
     "[此终端不支持忙时重绘,流式期间不画输入框;键入照收(排队),Esc 照常打断]"},
    {"spinner.thinking", "思考中"},
    {"spinner.stopping", "正在停…"},

    // ---- 子代理状态条(#52,#三:凑齐工具次数/token/耗时三个数字) ----
    {"agent_status.state_running", "运行中"},
    {"agent_status.state_stopping", "停止中(等当前操作收口)"},
    {"agent_status.state_done", "完成"},
    {"agent_status.state_failed", "失败"},
    {"agent_status.state_failed_reason", "失败 · {0}"},
    {"agent_status.state_stopped_reason", "停下 · {0}"},
    {"agent_status.state_exhausted", "耗尽 · {0}/{1} 步"},
    {"agent_status.budget_suffix", " · {0}/{1} 步"},
    {"agent_status.reason_api_error", "接口报错"},
    {"agent_status.reason_step_limit", "步数耗尽"},
    {"agent_status.reason_max_context", "上下文满"},
    {"agent_status.reason_no_final_text", "未交结论"},
    {"agent_status.reason_tool_error", "工具出错"},
    {"agent_status.reason_user_stop", "用户中止"},
    {"agent_status.reason_wall_clock", "墙钟超时"},
    {"agent_status.reason_protocol_error", "会话异常"},
    {"agent_status.reason_unknown", "未注明原因"},
    {"agent_status.summary", "{0}({1} 次工具调用 · {2} tokens · {3})"},
    {"agent_status.tokens_not_reported", "tokens 未报告"},
    // 实时活跃信号(规格"子代理活跃度不可见"):坞行/查看态按阶段换这一条
    // 文案,别堆三段;只报计数,思考与正文本身不进 dock 行。
    {"agent_activity.thinking", "思考中 · {0} 字"},
    {"agent_activity.text", "正文 {0} 字"},
    {"agent_activity.tool", "工具 {0} · {1}s"},
    {"agent_activity.waiting", "等首字节 · {0}s"},
    {"agent_activity.first_byte", "首字节 {0}ms"},
    // 墙钟兜底(规格三)的收场文案:超时原因明写,检查点照常带回。
    {"agent_outcome.wall_clock",
     "子代理整轮墙钟超时(≥ {0}s,subagent.wall_clock_timeout_secs):已强制收口;超时前取得的检查点见下。"},
    {"agent_outcome.wall_clock_force",
     "子代理墙钟超时且未在宽限期内响应停止信号,已强制收账(上限 {0}s,subagent.wall_clock_timeout_secs)。"},
    {"agent_outcome.wall_clock_late", "任务线程在强制收账后才返回;台账保持强制收账那份。"},
    // 输出预算耗尽的结构化失败页(规格根因四):main 与子代理共用同一组
    // 键,中英成对。
    {"agent_outcome.output_budget.head",
     "输出预算耗尽(stop_reason=max_tokens):自动续跑 {0} 次后仍无正文。已收到的思考与工具结果都保留在会话里。"},
    {"agent_outcome.output_budget.limit", "本场输出上限: {0} tokens(来源见 /config)"},
    {"agent_outcome.output_budget.limit_unset", "本场输出上限: unset——请求未带字段,墙在服务端默认"},
    {"agent_outcome.output_budget.continuations", "已自动续跑: {0} 次(上限 agent.length_continuations)"},
    {"agent_outcome.output_budget.usage_reported", "usage: 服务端已报告,token 账见回合统计"},
    {"agent_outcome.output_budget.usage_not_reported",
     "usage: 未报告——服务端报了 length 但没回 usage,token 数不可知,不按 0 算(chat 端常见原因是 stream_usage "
     "没开,/doctor cache usage 可探)"},
    {"agent_outcome.output_budget.escapes",
     "去路: 1) 输入\"继续\"让它接着收束; 2) 提高本场输出上限(config 的 agent.max_output_tokens); 3) 降低或关闭 "
     "thinking(/think none); 4) 拆小任务或换非推理模型"},
    {"error.length_empty_reasoning_bytes", "已收到 {0} 字节思考(末段已留检查点,见会话记录)"},
    {"agent_status.expand_hint", "(ctrl+o 展开明细)"},
    {"agent_tool.title_missing",
     "缺少必填参数 title:给任务一个语义短标题(中文 4~16 字、英文 2~6 个词,名词短语,不照抄 prompt 首句),补上后重试。"},
    {"agent_tool.title_bad",
     "title 不得含换行/制表符,且不得超过 40 显示列;请换一个简短的语义标题后重试(不要塞路径清单或任务全文)。"},
    {"agent_message.queued", "已排给子代理 #{0},将在它下一处安全轮次送达。"},
    {"agent_message.finished",
     "子代理 #{0} 已结束,不收插话(不改投 main、不自动复活;需要续跑请另派新任务)。"},
    {"agent_message.not_found", "没有任务号 #{0} 的子代理(可能已被清理或从未存在);请核对运行中子代理名册里的 task id。"},
    {"agent_message.invalid", "message 不能为空(且必须是字符串);只传增量要求,先逐字引用户原话。"},
    {"agent_message.task_id_invalid", "task_id 必须是整数(运行中子代理名册里列出的任务号)。"},
    {"agent_message.unavailable", "当前会话没有可用的子代理运行时,无法投递。"},
    {"agent_panel.untitled", "未命名子代理 #{0}(旧任务)"},
    {"agent_panel.stream_hint", "↑/↓ 选择 · Enter 查看 · x 停止/清除 · Esc 逐层退出"},
    {"agent_panel.source_foreground", "前台"},
    {"agent_panel.source_background", "后台"},
    {"agent_panel.hint", "↑/↓ 选择 · Enter 查看 · x 停止/清除 · Ctrl+X Ctrl+K 停止全部代理"},
    {"agent_panel.hint_armed", "再按 Ctrl+K 确认停止全部 · Esc/超时取消"},
    {"agent_panel.window_note", "共 {0} 只 · 上方未展示 {1} 只 · 下方未展示 {2} 只"},
    {"agent_panel.stop_all_notice", "已请求停止 {0} 只运行中的子代理"},
    {"agent_panel.stop_notice", "#{0} 已请求停止,正在收尾(停止中…)"},
    {"agent_panel.stop_not_running", "#{0} 已不在运行,未发停止信号"},
    {"agent_panel.pending_note", "待送达消息 {0} 条"},
    {"agent_panel.detail_gone", "该任务已被清理。"},
    {"agent_panel.view_header", "── 查看 {0} · {1} · Esc 回 main ──"},
    {"agent_panel.back_to_main", "已回主会话。"},
    {"agent_panel.completion_notice", "[后台子代理完成,结果交回主会话继续]"},
    {"agent_panel.reflow_toast", "子代理 {0} 已完成,结果已回流 main(查看态静默收货)"},
    {"agent_panel.denial_notice_title", "[后台子代理权限未放行,已被拒]"},
    {"keymap.override_warning", "键位覆盖未生效一项:{0}"},
    {"search.header", "历史搜索"},
    {"search.scope.session", "本会话"},
    {"search.scope.project", "本项目"},
    {"search.scope.all", "全部项目"},
    {"search.key.older", "更早"},
    {"search.key.scope", "换范围"},
    {"search.key.accept", "取回"},
    {"search.key.accept_submit", "取回并发送"},
    {"search.key.cancel", "取消"},
    {"search.query", "查询"},
    {"search.no_match", "没有命中的提问"},
    {"cmd.copy.usage", "用法:/copy(复制原始 Markdown)或 /copy plain(复制纯文本)"},
    {"cmd.copy.done", "已复制上一段答话({0} 字节)"},
    {"cmd.copy.no_assistant", "还没有可复制的答话。"},
    {"cmd.copy.unsupported", "此环境没有剪贴板通道:{0}"},
    {"cmd.copy.failed", "复制失败:{0}"},
    {"slash.desc.copy", "复制上一段完整答话到剪贴板(/copy plain 复制纯文本)"},
    {"stash.stashed", "草稿已收起(再按一次取回;随收件目标与目录分账)"},
    {"stash.restored", "暂存草稿已取回。"},
    {"stash.restore_refused", "暂存时的收件目标或目录与现在不一致,先回到原处再取回。"},
    {"stash.empty", "没有草稿可收起,也没有暂存可取回。"},
    {"stash.still_there", "还有一份收起的草稿(只存内存、不落盘,进程退出即弃)。"},
    {"editor.no_temp", "拿不到临时目录,编辑器没起。"},
    {"editor.write_failed", "临时文件写不进去,编辑器没起。"},
    {"editor.nonzero", "编辑器退出码 {0},原草稿没动。"},
    {"editor.file_gone", "编辑器没留下临时文件,原草稿没动。"},
    {"editor.bad_utf8", "编辑器写回的不是合法 UTF-8,原草稿没动。"},
    {"editor.done", "已从 {0} 读回草稿。"},
    {"mention.header", "@ 提及文件/目录"},
    {"mention.keys_hint", "↑/↓ 选择 · Enter/Tab 插入 · Esc 收起"},
    {"mention.no_match", "没有命中的文件或目录"},
    {"mention.dir_icon", "▸"},
    {"mention.file_icon", "·"},
    {"mention.missing", "提及的 {0} 不存在,这一轮没有发送(检查路径或重新 @ 一次)。"},
    {"mention.outside_root", "提及的 {0} 跑出了项目根,这一轮没有发送。"},
    {"mention.ledger_header", "[用户提及的文件/目录(已校验存在,相对根解析)]"},
    {"help.scene_header", "当前场景按键(? 再按收起;固定键是编辑器安全所需,不可改绑)"},
    {"help.scene_footer", "键位可用 /keymap set <动作> <和弦> 改绑;/keymap reset all 复位。"},
    {"help.fixed_suffix", "(固定)"},
    {"help.unbound_suffix", "(未绑键,/keymap set 可绑)"},
    {"hint.keys.help", "键位"},
    {"hint.keys.search_history", "搜历史"},
    {"hint.keys.expand", "展开/收起"},
    {"hint.keys.editor", "编辑器"},
    {"ui.turn_nav", "第 {0}/{1} 轮"},
    {"ui.to_scrollback", "转录已写入回滚区,用终端自带搜索查找。"},
    {"ui.view_in_editor", "转录已写临时文件,交 {0} 查看(退出后回 composer)。"},
    {"notify.state_busy", "跑着"},
    {"notify.state_idle", "等输入"},
    {"keymap.list_header", "键位表(作用域 · 当前键 · 动作名):"},
    {"keymap.usage", "用法:/keymap 看表 · /keymap set <动作> <和弦> 改绑 · /keymap reset <动作>|all 复位"},
    {"keymap.unknown_action", "不认得动作 {0}(/keymap 看全部动作名)。"},
    {"keymap.bad_chord", "和弦 {0} 解析不动(写法如 Ctrl+R、Alt+V、?、Shift+Tab)。"},
    {"keymap.bind_failed", "改绑被拒:{0}"},
    {"keymap.bound", "{0} 已绑到 {1}。"},
    {"keymap.save_failed", "键位没落盘:{0}"},
    {"keymap.reset_all", "全部键位已复位。"},
    {"keymap.reset_one", "{0} 已复位到出厂默认。"},
    {"keymap.reset_failed", "{0} 复位不成(固定键或未知动作)。"},
    {"keymap.fixed_suffix", "(固定)"},
    {"keymap.unbound_suffix", "(未绑键)"},
    {"slash.desc.keymap", "看/改键位(/keymap set 动作 和弦,用户级落盘)"},
    {"slash.desc.workflow", "可复用 Workflow 图:list/show/graph/validate/run/resume/cancel"},
    {"slash.desc.goal", "持久目标:goal/status/edit/pause/resume/clear(跨轮续跑到可验终点)"},
    {"slash.desc.loop", "会话定时循环:/loop [间隔] [正文] 建任务,list 列出,pause/resume/stop 管,run 立即补一拍"},
    {"image.pasted", "剪贴板图片已备好({0} KB,路径已插入,提交时随消息附上)"},
    {"image.paste_failed", "贴图不成:{0}"},
    {"clipboard.paste_text_failed", "没贴上:{0}"},
    {"transcript.assistant_bg_title", "后台回流 · 分析"},
    {"error.step_limit", "已达 step 上限(max_steps_per_turn;旧名 max_turns),本轮实际跑了 {0} 步,可调大上限或设 0 解除。"},
    {"error.length_empty_output", "输出预算耗尽(finish_reason=length):共输出 {0} tokens,正文一个字都没落。"},
    {"error.length_empty_reasoning", "usage 拆账:其中 reasoning {0} tokens——输出预算全被思考吃掉。"},
    {"error.length_empty_no_split", "usage 未拆 reasoning 账(服务端没回 reasoning_tokens),预算去向无法核对。"},
    {"error.length_empty_hint", "可 /think 降档(或 /think unset 不发参数)后重试,或调大输出预算。"},
    {"agent_panel.detail_prompt", "任务说明: "},
    {"agent_panel.detail_pending_head", "已排队、尚未送达的介入消息 {0} 条: "},
    {"agent_panel.detail_tools_head", "工具调用流水(共 {0} 次): "},
    {"agent_panel.detail_result_head", "结论/输出: "},
    {"agent_panel.target_queued", "已排给子代理 #{0},将在它当前工具收尾后送达;Esc 退出查看态即回 main。"},
    {"agent_panel.target_rejected", "子代理 #{0} 已结束,消息未投递(不改投 main)。Esc 返回或 x 清掉该条目。"},
    {"agent_panel.main", "主会话"},
    {"agent_panel.hint_short", "↑/↓ 选择 · Enter 查看"},
    {"agent_panel.hint_focused", "Enter 查看 · x 停止/清除 · Esc 返回 · Ctrl+X Ctrl+K 停止全部代理"},
    {"agent_panel.hint_focused_short", "Enter 查看 · x 停止/清除 · Esc 返回"},
    {"agent_panel.hint_idle_expanded", "Enter 查看 · Esc 收起"},
    {"agent_panel.stream_hint_short", "↑/↓ 选择 · Enter 查看"},
    {"agent_panel.stream_hint_focused", "Enter 查看 · x 停止/清除 · Esc 逐层退出 · Ctrl+X Ctrl+K 停止全部代理"},
    {"agent_panel.stream_hint_focused_short", "Enter 查看 · x 停止/清除 · Esc 逐层退出"},
    {"agent_panel.stream_hint_idle_expanded", "Enter 查看 · Esc 收起"},
    {"agent_panel.idle_summary", "另有 {0} 只闲置代理 · Enter 展开"},
    {"agent_panel.event_steering", "介入"},
    {"agent_panel.event_thinking", "思考"},
    {"agent_panel.event_failed", "任务终止"},
    {"agent_panel.events_unavailable", "该任务没有消息账(旧版派出的任务),仅有结论:"},
    {"agent_panel.main_header", "── 查看 main · 主会话 ──"},

    // ---- 确认提示 ----
    {"confirm.prompt", "[y] 本次允许  [a] 本会话总是允许(该工具)  [N] 拒绝: "},
    {"confirm.opt.allow_once", "本次允许"},
    {"confirm.opt.always", "本会话总是允许"},
    {"confirm.opt.deny", "拒绝"},
    {"confirm.menu.hint", "↑/↓ 选择 · Enter 确认 · Esc 拒绝"},
    {"confirm.persist.yes", "是,写进项目设置"},
    {"confirm.persist.no", "否,仅本会话"},
    {"confirm.persist.menu.hint", "↑/↓ 选择 · Enter 确认"},
    {"confirm.detail.path", "    路径: {0}"},
    {"confirm.detail.replace_all", "  (replace_all=true,全部替换)"},
    {"confirm.detail.content", "    内容({0} 字节),前几行:"},
    {"confirm.detail.old", "    - 旧文本:"},
    {"confirm.detail.new", "    + 新文本:"},
    {"confirm.detail.command", "    命令({0}): {1}"},
    {"confirm.detail.workdir", "    工作目录: {0}"},
    {"confirm.detail.background", "    (后台运行:spawn 后立刻返回,输出写日志文件)"},
    {"confirm.detail.args", "    参数: {0}"},
    {"confirm.detail.omitted", "      ...(共 {0} 行,已省略其余)"},

    // ---- 向导 ----
    {"wizard.title", "=== lubancode 初次配置向导 ==="},
    {"wizard.subtitle", "(base_url / api_key 没读到,先配一遍,配完直接进入会话)"},
    {"wizard.lang.title", "界面语言 / Language:"},
    {"wizard.lang.prompt", "选择 / Select [{0}]: "},
    {"wizard.wire.title", "接口格式:"},
    {"wizard.wire.opt1", "anthropic (Claude 系)"},
    {"wizard.wire.opt2", "responses (OpenAI 系)"},
    {"wizard.wire.opt3", "chat_completions (OpenAI 兼容)"},
    {"wizard.choose_prompt", "选择 [1]: "},
    {"wizard.choose_prompt_n", "选择 [{0}]: "},
    {"wizard.choose.hint", "↑/↓ 选择 · Enter 确认 · Esc 取消"},
    {"wizard.base_url.title", "base_url(必填),例如:"},
    {"wizard.base_url.empty", "base_url 不能为空,再输一遍。"},
    {"wizard.api_key.prompt", "api_key(必填): "},
    {"wizard.api_key.empty", "api_key 不能为空,再输一遍。"},
    {"wizard.model.hint", "model:回车自动从接口获取列表,或者直接输入模型名。"},
    {"wizard.model.fetch_failed", "拉取模型列表失败: {0}"},
    {"wizard.model.manual", "改成直接输入模型名:"},
    {"wizard.model.empty", "model 不能为空,再输一遍。"},
    {"wizard.model.list_empty", "接口返回的模型列表是空的。"},
    {"wizard.model.choose", "选择模型编号 [1]: "},
    {"wizard.choice.bad_range", "编号不对,重新选一个。"},
    {"wizard.choice.not_number", "没听懂,输个编号(直接回车用默认值)。"},
    {"wizard.summary.title", "配置汇总:"},
    {"wizard.save_prompt", "保存到 {0}? [Y/n]: "},
    {"wizard.saved", "已保存到 {0}"},
    {"wizard.save_failed", "保存失败: {0}(不影响本次继续用,只是这份配置这次没记住)"},
    {"setup.entry.title", "开始使用 lubancode"},
    {"setup.entry.language_progress", "第 1 / 2 步"},
    {"setup.entry.language_body", "先选界面语言。进主界面后仍可用 /language 更改。"},
    {"setup.entry.method_progress", "第 2 / 2 步"},
    {"setup.entry.method_body", "还没有可用的模型连接。"},
    {"setup.entry.method_hint", "现在可以添加 Provider，也可以先进入主界面。"},
    {"setup.entry.add", "添加 Provider"},
    {"setup.entry.add_desc", "从服务商目录选择，填好密钥后立即使用"},
    {"setup.entry.skip", "暂时跳过"},
    {"setup.entry.skip_desc", "稍后用 /provider 或 /provider add 配置"},
    {"setup.session.hint", "输入 /provider add 添加，或用 /provider 管理已有配置。"},
    {"setup.turn.blocked", "还没有可用的 Provider。先用 /provider add 添加，或用 /provider 切换已有配置。"},
    {"banner.not_connected", "尚未连接"},

    // ---- /provider add 向导(裸敲 /provider add 或 /provider add 名字 触发;
    //      向导重排单后八步可回退,鉴权三态) ----
    {"provider_wizard.title", "添 provider"},
    {"provider_wizard.progress", "第 {0}/{1} 步"},
    {"provider_wizard.footer.first", "Esc 退出向导  Ctrl+C 取消"},
    {"provider_wizard.footer.back", "Esc 上一步  Ctrl+C 取消"},
    {"provider_wizard.current_value", "当前: {0}"},
    {"provider_wizard.exit_confirm.body", "这是第一步,再退就退出向导。已填的内容不会写盘。"},
    {"provider_wizard.exit_confirm.prompt", "退出向导? [y/N]: "},
    {"provider_catalog.choose.title", "选一家模型服务（目录来自 LubanCode 仓库）:"},
    {"provider_catalog.choose.custom", "自定义（全手填）"},
    {"provider_catalog.selected", "已选 {0}；协议 {1}，默认模型 {2}。"},
    {"provider_catalog.refreshing", "正在更新 provider 目录……"},
    {"provider_catalog.refresh_failed", "provider 目录更新失败，改用本地快照：{0}"},
    {"provider_catalog.refresh_current", "provider 目录已是最新。"},
    {"provider_catalog.refresh_ok", "provider 目录已更新到 {0}，缓存写在 {1}。"},
    {"provider_catalog.warning", "[provider 目录警告] {0}"},
    {"provider_wizard.name.prompt", "名字: "},
    {"provider_wizard.name.empty", "名字不能为空。"},
    {"provider_wizard.name.hint", "名字可用字母、数字、下划线、点、短横线。"},
    {"provider_wizard.name.slug_hint", "可以试试: {0}"},
    {"provider_wizard.name.prefill_invalid", "命令行给的名字 {0} 不能用: {1},改问一遍。"},
    {"provider_wizard.wire.hint", "选接口格式——决定请求路径、默认环境变量与探测地址。"},
    {"provider_wizard.wire.opt1", "Anthropic Messages"},
    {"provider_wizard.wire.desc1", "POST {base}/v1/messages · GET {base}/v1/models"},
    {"provider_wizard.wire.opt2", "OpenAI Responses"},
    {"provider_wizard.wire.desc2", "POST {base}/responses · GET {base}/models"},
    {"provider_wizard.wire.opt3", "OpenAI Chat Completions"},
    {"provider_wizard.wire.desc3", "POST {base}/chat/completions · GET {base}/models"},
    {"provider_wizard.base_url.prompt", "base_url: "},
    {"provider_wizard.base_url.empty", "base_url 不能为空。"},
    {"provider_wizard.base_url.bad_scheme", "base_url 得以 http:// 或 https:// 开头,再输一遍。"},
    {"provider_wizard.base_url.hint",
     "须带 http:// 或 https://。OpenAI 兼容接口(Responses/Chat Completions)一般还要带到 /v1,"
     "本地示例: http://127.0.0.1:8000/v1"},
    {"provider_wizard.base_url.probe", "将读取 {0}"},
    {"provider_wizard.base_url.v1_offer", "这个服务通常还要 /v1。"},
    {"provider_wizard.base_url.v1_opt_use", "采用 {0}"},
    {"provider_wizard.base_url.v1_opt_keep", "保持 {0}"},
    {"provider_wizard.auth.hint", "自建服务常常不要 key;要 key 的选环境变量或直接贴。"},
    {"provider_wizard.auth.opt_none", "无需鉴权"},
    {"provider_wizard.auth.desc_none", "请求彻底不带鉴权头"},
    {"provider_wizard.auth.opt_env", "从环境变量读取"},
    {"provider_wizard.auth.desc_env", "默认 {0}"},
    {"provider_wizard.auth.opt_inline", "贴入 API key"},
    {"provider_wizard.auth.desc_inline", "明文落盘,展示打码"},
    {"provider_wizard.auth.env.prompt", "环境变量名(回车用默认 {0})。"},
    {"provider_wizard.auth.env.note_set", "环境变量 {0} 已设置。"},
    {"provider_wizard.auth.env.note_unset", "环境变量 {0} 当前没读到值——拉模型前得先设好。"},
    {"provider_wizard.auth.env.input", "环境变量名: "},
    {"provider_wizard.auth.inline.hint", "明文 key 落盘到 api_key,展示一律打码。"},
    {"provider_wizard.auth.inline.keep", "已设置明文密钥({0}),回车保留;重新输入即更换。"},
    {"provider_wizard.auth.inline.input", "API key: "},
    {"provider_wizard.auth.inline.empty", "key 不能为空;不想用密钥就返回选\"无需鉴权\"。"},
    {"provider_wizard.auth.summary_env", "环境变量 {0}"},
    {"provider_wizard.auth.summary_inline", "明文 key {0}"},
    {"provider_wizard.model.prompt", "model: "},
    {"provider_wizard.model.probe", "模型列表将从 {0} 读取[{1}]。"},
    {"provider_wizard.model.hint", "回车拉取列表,或直接输入模型名。"},
    {"provider_wizard.model.manual_hint", "手动输入模型名。"},
    {"provider_wizard.model.empty", "model 不能为空。"},
    {"provider_wizard.model.list_empty", "接口返回的模型列表是空的。"},
    {"provider_wizard.model.fetch_failed", "未能读取模型: {0}"},
    {"provider_wizard.model.fetch_404_hint", "404 多半该检查地址或接口格式。"},
    {"provider_wizard.model.fetch_401_hint", "401/403 指向密钥。"},
    {"provider_wizard.model.fetch_network_hint", "连接失败,先看看服务起没起。"},
    {"provider_wizard.model.fetch_other_hint", "看看服务端返回了什么。"},
    {"provider_wizard.model.err_network", "连接失败"},
    {"provider_wizard.model.err_other", "读取失败"},
    {"provider_wizard.model.opt_manual", "手动输入模型名"},
    {"provider_wizard.model.opt_back_wire", "返回检查接口格式"},
    {"provider_wizard.model.opt_back_url", "返回检查 base_url"},
    {"provider_wizard.model.opt_retry", "重试"},
    {"provider_wizard.model.opt_add_v1", "加上 /v1 后重试(读取 {0})"},
    {"provider_wizard.effort.hint",
     "model_reasoning_effort(可选,切到这个 provider 时自动应用的推理档位,候选跟 /think 一致: "
     "none/low/medium/high/xhigh/max,留空跳过):"},
    {"provider_wizard.effort.prompt", "effort: "},
    {"provider_wizard.effort.unset", "(未设置)"},
    {"provider_wizard.extra_body.hint",
     "额外请求参数(JSON object,直接回车跳过；只放目录未建模的厂商私有字段，例如:"
     " {\"temperature\":0.2}):"},
    {"provider_wizard.extra_body.prompt", "extra_body: "},
    {"provider_wizard.extra_body.invalid_json", "不是合法 JSON: {0},再输一遍(直接回车跳过)。"},
    {"provider_wizard.extra_body.not_object", "得是一个 JSON object(花括号包着的键值对),再输一遍(直接回车跳过)。"},
    {"provider_wizard.extra_body.unset", "(未设置)"},
    {"provider_wizard.extra_body.summary", "{0}键"},
    {"provider_wizard.summary.name", "1) name       = {0}"},
    {"provider_wizard.summary.wire", "2) wire       = {0}"},
    {"provider_wizard.summary.base_url", "3) base_url   = {0}"},
    {"provider_wizard.summary.auth", "4) auth       = {0}"},
    {"provider_wizard.summary.model", "5) model      = {0}"},
    {"provider_wizard.summary.effort", "6) effort     = {0}"},
    {"provider_wizard.summary.extra_body", "7) extra_body = {0}"},
    {"provider_wizard.summary.window", "   window     = {0}"},
    {"provider_wizard.confirm.hint", "Enter 直接保存当前配置 · 1-7 修改对应项 · n 放弃"},
    {"provider_wizard.confirm.prompt", "选择（直接回车保存）: "},
    {"provider_wizard.confirm.bad_number", "请输入 1-7;不修改就直接回车保存,输入 n 放弃。"},

    // ---- /provider edit 向导(容错单):同一套八步面板,全字段预填 ----
    {"provider_wizard.edit.title", "编辑 provider"},
    {"provider_wizard.edit.name_locked", "1) name       = {0}(不支持改名)"},
    {"provider_wizard.edit.no_rename", "edit 不改名字;要换名字,先删了再添。"},
    {"provider_wizard.edit.name_prompt", "回车返回汇总: "},
    {"provider_wizard.edit.diff.wire", "2) wire       = {0} → {1}"},
    {"provider_wizard.edit.diff.base_url", "3) base_url   = {0} → {1}"},
    {"provider_wizard.edit.diff.auth", "4) auth       = {0} → {1}"},
    {"provider_wizard.edit.diff.model", "5) model      = {0} → {1}"},
    {"provider_wizard.edit.diff.effort", "6) effort     = {0} → {1}"},
    {"provider_wizard.edit.diff.extra_body", "7) extra_body = {0} → {1}"},
    {"provider_wizard.edit.diff_none", "本次没有字段改动;回车原样写回,n 放弃。"},
    {"provider_wizard.edit.model.hint", "输入新模型名,回车保留当前值(编辑模式不拉列表)。"},
    {"provider_wizard.cancelled", "已取消,没有写入任何配置。"},

    // ---- slash 命令描述表 ----
    {"slash.desc.help", "列出所有命令"},
    {"slash.desc.model", "拉模型列表选;/model 名字 直接切;/model cheap 名字 设后台档;/model roles 看任务路由"},
    {"slash.desc.provider", "列、添、切、删、改、刷模型服务端;/provider add|list|switch|remove|set|refresh"},
    {"slash.desc.config", "打印当前生效配置和本会话在用的 model"},
    {"slash.desc.update", "检查 GitHub 最新 Release；升级时同步程序与官方技能"},
    {"slash.desc.init", "在项目根生成 AGENTS.md，并让本会话立即采用"},
    {"slash.desc.language", "列可选界面语言并切换;/language 语言码 直接切"},
    {"slash.desc.image", "附本地图片;/image 路径 或在消息里写 @路径"},
    {"slash.desc.worktree", "新建、列出或退出隔离工作树;/worktree new [名字] | list | exit keep|remove"},
    {"slash.desc.clear", "清空对话历史"},
    {"slash.desc.exit", "退出(裸词 exit/quit 也认)"},
    {"slash.desc.context", "看当前上下文占用;/context 256k|512k|1m 临时改窗口大小"},
    {"slash.desc.compact", "手动压缩历史;/compact 重点说明 可指定这次额外保留什么"},
    {"slash.desc.think", "看当前推理强度;/think 档位 切档位,声明未知会如实标注(/effort 同义)"},
    {"slash.desc.effort", "同 /think(推理强度别名)"},
    {"slash.desc.skills", "列出扫描到的技能(官方 + 主目录级 + 项目级)"},
    {"slash.desc.skill", "管理技能(裸敲查看安装网址、本地目录等完整示例)"},
    {"slash.desc.mcp", "列出挂载的 MCP 服务器状态和工具清单"},
    {"slash.desc.lsp", "列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)"},
    {"slash.desc.todos", "查看当前待办清单"},
    {"slash.desc.plugins", "列出插件三路(native/Lua/process)的状态与加载警告"},
    {"slash.desc.plugin",
     "管单枚插件:inspect 看详情 / doctor 查环境 / test 试跑 / reload 重载 / enable|disable 开关"},
    {"slash.desc.tools", "列工具三态:核心(恒在)/已加载/延迟未加载(tool_search 延迟挂载)"},
    {"slash.desc.memory",
     "管理项目记忆;/memory on|off|use|learn|review|accept|edit|reject|list|remember|forget|rebuild|why"},
    {"slash.desc.sessions", "列本目录最近 20 场会话存档,倒序编号;/sessions all 列全部目录;/sessions archived 看归档"},
    {"slash.desc.archive", "归档当前会话(搬进 sessions/archive/ 后退出;想恢复先 lubancode unarchive <id>)"},
    {"slash.desc.delete", "永久删除当前会话(先确认;回合在跑/审批悬着时拒绝)"},
    {"slash.desc.resume", "打开会话台账:搜索/筛选/排序后续聊(也可跟编号或 id)"},
    {"slash.desc.export", "当前会话导出 Markdown;/export 路径 可指定输出文件"},
    {"slash.desc.title", "看当前会话标题;/title 标题 给本场起名,/sessions 列表和导出都用它"},
    {"slash.desc.soul", "看当前魂;/soul 内容 写进 SOUL.md,/soul clear 还原默认；名字仍可切换备选魂"},
    {"slash.desc.prompt", "看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md"},
    {"slash.desc.background", "列后台命令任务清单(状态/PID/命令);run_command run_in_background 起的那些"},
    {"slash.desc.record", "录一遍生成技能;/record start 名字 开录,stop 出草稿,确认后安装"},
    {"slash.desc.plan", "只读研究并提交计划;/plan 正文 开始规划,status 看状态,off 退出,review 重开审阅"},

    // ---- /plan(Plan 模式:只读研究硬闸单) ----
    {"plan.entered", "已进入 Plan 模式(只读研究)。写盘/未知外挂工具会被硬闸拒绝;/plan off 退出,交计划后弹审阅。"},
    {"plan.exited", "已退出 Plan 模式,未批准任何计划。"},
    {"plan.already_in", "已在 Plan 模式。"},
    {"plan.not_in", "当前不在 Plan 模式(无需 /plan off)。"},
    {"plan.busy", "回合正在跑,不半腰切模式:先 Esc 打断,或把 /plan 排进下一轮。"},
    {"plan.bad_sub", "认不得的子命令:{0}。用法:/plan [正文] | status | off | review"},
    {"plan.mode_label", "plan"},
    {"plan.review.title", "计划审阅({0} · 第 {1} 稿 · sha {2})"},
    {"plan.review.hint", "↑/↓ 选择 · Enter 确认 · Esc 关框(留在 Plan,/plan review 再开)"},
    {"plan.review.opt.approve_confirm", "批准,并以 Confirm 档执行(每步工具照常问)"},
    {"plan.review.opt.approve_auto", "批准,并以 Auto 档执行(安全命令免问,危险仍问)"},
    {"plan.review.opt.stay", "留在 Plan,继续修改计划"},
    {"plan.review.opt.exit", "退出 Plan,不执行"},
    {"plan.review.approved", "计划已批准(第 {0} 稿)。切回 Default 模式,另起执行轮。"},
    {"plan.review.stayed", "留在 Plan 模式:可继续向模型提出修改要求。"},
    {"plan.review.exited", "已退出 Plan 模式,未执行计划。"},
    {"plan.review.stale", "这稿已被新稿顶替(或 hash 对不上),回答作废;最新稿再审。"},
    {"plan.review.no_plan", "还没有可审的计划:先在 Plan 模式里让模型交一份 <proposed_plan>。"},
    {"plan.review.cancelled", "已关审阅框(仍留 Plan 模式);/plan review 可重开。"},
    {"plan.status.in_plan", "当前档:Plan(只读研究)。"},
    {"plan.status.in_default", "当前档:Default(实施)。"},
    {"plan.status.no_plan", "最近计划:无。"},
    {"plan.status.plan_line", "最近计划:{0} · 第 {1} 稿 · 状态 {2}。"},
    {"plan.turn.task_prefix", "[Plan 模式规划请求] "},
    {"plan.turn.handoff", "[已批准计划 · {0} 第 {1} 稿]\n\n按下列已批准的计划实施。执行期用 todo_write 拆施工清单,再逐步动手;计划中的验证步骤照做。\n\n"},
    {"plan.ambiguous", "本轮正文里出现多份 <proposed_plan>(嵌套或两稿),按普通回答处理,不弹审批;请让模型重交一份完整替换稿。"},
    {"plan.truncated", "计划标签未闭合(流式中途或半截),按普通回答处理;下一轮说完整再交。"},
    {"plan.recorded", "收到计划:{0} 第 {1} 稿({2} 字节)。"},
    {"plan.resume.approved_pending", "上场的计划已批准但执行没启动:自己续上(/plan review 看稿,或直接派活)。"},
    {"plan.env.bad_mode", "LUBANCODE_COLLABORATION_MODE 认不得 \"{0}\":只认 plan 或 default,本次按 default 启动。"},
    {"plan.settings.bad_mode", "settings.local.json 的 default_collaboration_mode 认不得 \"{0}\":只认 plan 或 default,本次按 default 启动。"},

    // ---- /record(录一遍生成技能) ----
    {"record.usage",
     "用法:\n"
     "  /record start <名字>      开录(先问目标、可变输入、成事标准三句)\n"
     "  /record note <为何这样做>  补一条用户备注\n"
     "  /record pause             暂停(状态栏挂 REC 已停)\n"
     "  /record resume            续录\n"
     "  /record stop              停止并起草 SKILL.md,预览后可选择安装\n"
     "  /record cancel            取消,删掉本场录制件(已装好的技能不动)\n"
     "  /record status            看当前录制状态\n"
     "  /record list              列录制件(含崩溃留下的半截件)\n"
     "  /record install <编号> [project|home]  安装某场录制件的草稿\n"
     "  /record discard <编号>    丢弃一场录制件\n"},
    {"record.unavailable", "[record] 找不到主目录,录制功能不可用。"},
    {"record.status.idle", "当前没有在录。/record start <名字> 开录。"},
    {"record.status.recording", "录制中({0}):{1}\n录制件:{2}"},
    {"record.status.recording_word", "进行中"},
    {"record.status.paused_word", "已暂停"},
    {"record.status.paused_marker", "REC 已停"},
    {"record.already_active", "[record] 已在录制中:{0}。先 /record stop 或 /record cancel。"},
    {"record.not_active", "[record] 没有在录。/record start <名字> 开录。"},
    {"record.ask.goal", "这桩活最后要得什么?"},
    {"record.ask.variables", "哪些值每回都会变?(没有就回车跳过)"},
    {"record.ask.acceptance", "看见什么才算做成?"},
    {"record.ask.verification", "最后一次验证结果?(没有就回车跳过)"},
    {"record.started", "[record] 开录:{0}\n录制件目录:{1}\n状态栏挂 REC 标记;/record stop 起草。"},
    {"record.start.failed", "[record] 开录失败: {0}"},
    {"record.op_failed", "[record] {0}"},
    {"record.note_saved", "[record] 备注已记下。"},
    {"record.paused_msg", "[record] 已暂停,状态栏挂 REC 已停;/record resume 续录。"},
    {"record.resumed_msg", "[record] 已续录。"},
    {"record.stop_done", "[record] 已停止。录制件: {0} ({1})"},
    {"record.stop.draft_failed", "[record] 起草失败: {0}"},
    {"record.draft.header", "草稿已生成({0} 个文件),全文如下: ----"},
    {"record.install.prompt", "装到哪一级? [p]项目级 / [h]主目录级 / 其余不装: "},
    {"record.install.files", "确认后将写入 {0}:"},
    {"record.skill_name_placeholder", "技能名"},
    {"record.install.confirm", "确认安装? [y/N]: "},
    {"record.install.cancelled", "[record] 未安装。草稿留在录制件里,之后 /record install <编号> 仍可装。"},
    {"record.install.done", "[record] 已安装技能 {0} 到 {1},本场技能清单已刷新,立刻可用。"},
    {"record.install.failed", "[record] 安装失败: {0}"},
    {"record.install.not_found", "[record] 找不到录制件 {0}。/record list 看编号。"},
    {"record.install.no_draft", "[record] 录制件 {0} 没有草稿(半截录制件装不进 skills;先 /record stop 生成草稿)。"},
    {"record.list.header", "录制件(倒序):"},
    {"record.list.empty", "还没有录制件。/record start <名字> 开录。"},
    {"record.list.entry", "[{0}] {1}  {2}  {3}  {4}"},
    {"record.list.finished", "已停止"},
    {"record.list.unfinished", "未完成(崩溃或未 stop,装不进 skills)"},
    {"record.list.has_draft", "有草稿"},
    {"record.list.no_draft", "无草稿"},
    {"record.discard_done", "[record] 已丢弃 {0}。"},
    {"slash.desc.peers", "列同机可见的其它 Lubancode 会话(名字/状态/目录);方向键选,Enter 看详情"},
    {"slash.desc.send", "/send <名字或短id> <话>:给另一场会话递一张字条"},
    {"slash.desc.peerperm", "/peerperm auto|accept|hold|refuse:跨会话来信的收件档"},
    {"slash.desc.doctor", "/doctor effort|cache:本地兼容端 Effort 档位与前缀缓存诊断(探针要发请求)"},

    // ---- /update ----
    {"cmd.update.usage", "用法: /update 或 /update check"},
    {"cmd.update.checking", "正在检查 GitHub 最新 Release……"},
    {"cmd.update.failed", "检查更新失败: {0}"},
    {"cmd.update.current", "没有发现更新。当前 {0}，远端 {1}。"},
    {"cmd.update.available", "有新版可用。当前 {0}，最新 {1}。"},
    {"cmd.update.release", "发布页: {0}"},
    {"cmd.update.install_hint", "下载新版发行包并运行包内安装脚本；程序与官方 skills 会一并更新，用户技能不动。"},

    // ---- /memory ----
    {"cmd.memory.usage",
     "用法:\n"
     "  /memory                         看本场状态\n"
     "  /memory on|off                  开关本场项目记忆(须先全局授权)\n"
     "  /memory use on|off              开关同步召回\n"
     "  /memory learn off|review|auto   学习档位(auto 须全局配置授权)\n"
     "  /memory review                  看待审候选\n"
     "  /memory accept <id>             接受候选入库\n"
     "  /memory edit <id> 标题 [:: 正文] 改候选\n"
     "  /memory reject <id> [理由]      拒绝候选(同主题不再重提)\n"
     "  /memory list                     列出项目记忆\n"
     "  /memory remember fact|preference|feedback 标题 [:: 正文]\n"
     "  /memory remember user preference|feedback 标题 [:: 正文]  (须全局授权)\n"
     "  /memory forget <id>              归档一条记忆\n"
     "  /memory rebuild                  后台重建索引\n"
     "  /memory stale                    看指纹漂移与已过期的记忆\n"
     "  /memory verify <id>              核验后续命(原 id 复活)\n"
     "  /memory refresh <id>             核验并把 status 回炉为 active\n"
     "  /memory migrate                  旧格式主题批迁 front matter(先列账再确认)\n"
     "  /memory show <id>                看一份主题的 front matter 与正文\n"
     "  /memory open [id]                用 $VISUAL/$EDITOR 编辑主题或索引\n"
     "  /memory why [id]                 看上一轮召回为何命中/落选\n"},
    {"cmd.memory.unavailable", "[memory] 找不到主目录，项目记忆不可用。"},
    {"cmd.memory.on", "开"},
    {"cmd.memory.off", "关"},
    {"cmd.memory.global", "全局授权: {0}"},
    {"cmd.memory.denied",
     "[memory] 全局配置未授权开启项目记忆，本场命令开不了。"
     "请在 <主目录>/.lubancode/config.json 里写 \"memory\": {\"enabled\": true} 后重启 lubancode。"},
    {"cmd.memory.status", "项目记忆: {0}；召回 {1}；写入 {2}"},
    {"cmd.memory.learn_status", "学习档位: {0}(off/review/auto)"},
    {"cmd.memory.candidates", "待审候选: {0}(/memory review)"},
    {"cmd.memory.learn_denied", "[memory] {0}"},
    {"cmd.memory.learn_set", "[memory] 学习档位已设为 {0}。"},
    {"cmd.memory.review.empty", "[memory] 没有待审候选。"},
    {"cmd.memory.review.header", "待审候选:"},
    {"cmd.memory.review.hint",
     "用 /memory accept <id> 接受、/memory edit <id> 标题::正文 修改、/memory reject <id> [理由] 拒绝。"},
    {"cmd.memory.reject.done", "[memory] 候选已拒绝,同主题不会再自动重提。"},
    {"cmd.memory.edit.done", "[memory] 候选已改,仍在待审区。"},
    {"cmd.memory.project", "项目: {0}"},
    {"cmd.memory.directory", "目录: {0}"},
    {"cmd.memory.counts", "条目: {0}；待办: {1}"},
    {"cmd.memory.master", "[memory] 本场已{0}。"},
    {"cmd.memory.toggle", "[memory] {0}子开关已{1}。"},
    {"cmd.memory.retrieval", "召回"},
    {"cmd.memory.catalog_warning", "[memory] 索引有误，已改扫主题文件: {0}"},
    {"cmd.memory.empty", "项目记忆还是空的。"},
    {"cmd.memory.queued", "[memory] 已排进后台队列: {0}"},
    {"cmd.memory.queue_failed", "[memory] 排队失败: {0}"},
    {"cmd.memory.worker_failed", "[memory] 后台任务暂未启动: {0}"},
    {"cmd.memory.project_failed", "[memory] 项目身份解析失败: {0}"},
    {"cmd.memory.switch_failed", "[memory] 切换项目失败: {0}"},
    {"memory.extract.running", "[memory] 回合总结({0})…"},
    {"memory.extract.failed", "[memory] 回合总结失败,本轮跳过: {0}"},
    {"memory.extract.done", "[memory] 新候选 {0} 条待审(/memory review);自动入库 {1} 条。"},
    {"cmd.memory.stale.empty", "[memory] 没有指纹漂移或已过期的记忆。"},
    {"cmd.memory.stale.header", "陈旧清单(fingerprint=文件已变,expired=已过期):"},
    {"cmd.memory.stale.fingerprint", "相关文件已变化"},
    {"cmd.memory.stale.expired", "已过期"},
    {"cmd.memory.stale.hint",
     "核验后仍有效就 /memory verify <id> 续命;过期规约可改 expires_at 或 /memory forget 归档。"},
    {"cmd.memory.why.expired", "已过 expires_at,等续期或归档"},
    {"cmd.memory.why.scope", "scope 不符当前工作目录"},
    {"cmd.memory.why.none", "[memory] 本场还没有召回记录。"},
    {"cmd.memory.why.header", "[memory] 上一轮召回({0}):"},
    {"cmd.memory.why.origin", "  请求来源: {0}"},
    {"cmd.memory.why.skipped_turn", "  合成控制消息,本轮未跑检索,检索词为空。"},
    {"cmd.memory.why.terms", "  检索词: {0}"},
    {"cmd.memory.why.hit", "  {0}  分数 {1}(硬命中 {2}，词项 {3}) — 已注入 {4} 字节"},
    {"cmd.memory.why.miss", "  {0}  分数 {1}(硬命中 {2}，词项 {3}) — 未注入: {4}"},
    {"cmd.memory.why.stale", "相关文件已变化，只提示不注正文"},
    {"cmd.memory.why.duplicate", "同一事实/相同证据已注入,去重让位"},
    {"cmd.memory.why.superseded", "项目层同主题已注入,用户层让位"},
    {"cmd.memory.why.layer_user", "(用户层)"},
    {"cmd.memory.user_layer", "用户层"},
    {"cmd.memory.user_status", "用户级记忆: {0} 条;目录: {1}(授权在全局 memory.user_enabled)"},
    {"cmd.memory.why.below_threshold", "分数未过最低门槛"},
    {"cmd.memory.why.budget", "条数/字节预算已满"},
    {"cmd.memory.why.skipped", "未取到正文"},
    {"cmd.memory.why.total", "  合计注入 {0} 条 · {1} 字节"},
    {"cmd.memory.why.missing", "[memory] 上一轮召回里没有 {0}。"},
    {"cmd.memory.migrate.none",
     "[memory] 没有要迁的旧格式主题(已跳过 {0} 份,警告 {1} 份)。"},
    {"cmd.memory.migrate.plan",
     "[memory] 迁移计划:将改 {0} 份,跳过 {1} 份,警告 {2} 份。原件会备进 .state/migration-backup/。"},
    {"cmd.memory.migrate.confirm", "照此迁移? [y/N]: "},
    {"cmd.memory.migrate.cancelled", "不迁,旧主题原样保留。"},
    {"cmd.memory.migrate.done", "[memory] 已迁 {0} 份为 front matter;备份在 {1}。"},
    {"cmd.memory.show.header", "[memory] {0}(住 {1}):"},
    {"cmd.memory.open.done", "[memory] 编辑收妥,已校验并重建索引。"},

    // ---- /language ----
    {"cmd.language.list_header", "可选语言(内置 zh-CN/en + <主目录>/.lubancode/languages/*.json):"},
    {"cmd.language.current_mark", "(当前)"},
    {"cmd.language.choose", "选择语言编号 [{0}]: "},
    {"cmd.language.switched", "已切换语言: {0}(本会话即时生效)"},
    {"cmd.language.unknown", "不认得语言 {0}(/language 裸敲看可选列表)。"},
    {"cmd.language.bad_number", "编号不对,取消切换。"},

    // ---- /worktree ----
    {"cmd.worktree.usage", "用法:/worktree new [名字] | list | exit keep|remove"},
    {"cmd.worktree.created", "已建隔离工作树:{0}\n分支:{1}\n会话目录已切过去。"},
    {"cmd.worktree.list_header", "本仓库的 worktree:"},
    {"cmd.worktree.current", "(当前)"},
    {"cmd.worktree.detached", "(游离 HEAD)"},
    {"cmd.worktree.kept", "已回原目录，工作树留下:{0}"},
    {"cmd.worktree.removed", "已删工作树与分支:{0}"},
    {"cmd.worktree.dirty", "工作树有未提交改动:{0}"},
    {"cmd.worktree.remove_confirm", "仍要强删这棵工作树和分支? [y/N]: "},
    {"cmd.worktree.remove_cancelled", "不删，仍留在这棵工作树。"},
    {"cmd.worktree.not_repo", "这里不在 Git 仓库里，/worktree 用不了。"},
    {"cmd.worktree.invalid_name", "名字只收字母、数字、-、_，且不超过 64 个字符。"},
    {"cmd.worktree.already_active", "这场会话已在一棵新工作树里，先 /worktree exit keep|remove。"},
    {"cmd.worktree.no_active", "这场会话没有可退出的 /worktree 新树。"},
    {"cmd.worktree.git_failed", "Git 没办成:{0}"},
    {"cmd.worktree.filesystem_failed", "目录没办成:{0}"},
    {"cmd.worktree.outside_confirm", "要进的房在 .lubancode/worktrees 之外:{0}\n需先经你确认(y)才进。"},
    {"cmd.worktree.outside_prompt", "进这间园外的房?(会话目录、写权限与项目配置都会搬过去) [y/N]: "},
    {"cmd.worktree.verify_failed", "验明正身没过,拒绝进房:{0}"},
    {"cmd.worktree.cleaned", "顺手清扫了 {0} 间隔离子代理的陈工作树(只清 agent- 前缀且有活已跳过的)。"},

    // ---- /config 诊断 ----
    {"config.header", "lubancode 最终生效的配置:"},
    {"config.not_set", "(未设置)"},
    {"config.language.follow_system", "(未设置,跟系统: {0})"},
    {"config.compact_model.unset", "(未设置,跟会话模型一致)"},
    {"config.think.unset", "未发送参数(请求里无此字段)"},
    {"config.soul.unset", "(未设置,用主目录 SOUL.md)"},
    {"config.threshold.never", "(永不延迟)"},
    {"config.steps.unlimited", "(无上限)"},
    // 输出预算(规格"子代理与 MainAgent 同级"根因一):unset 说破,不装 0
    // 也不藏魔数;来源四级句与 /context 的输出上限行共用。
    {"config.output.unset", "unset(请求不带字段,交服务端/模型默认;anthropic 必填时落公开兜底 8192)"},
    {"config.output.tokens", "{0} tokens"},
    {"config.output_source.config", "配置 agent.max_output_tokens"},
    {"config.output_source.config_subagent", "配置 subagent.max_output_tokens(显式覆盖)"},
    {"config.output_source.provider", "provider 声明"},
    {"config.output_source.catalog", "模型目录声明"},
    {"config.output_source.unset", "unset(三级都未声明)"},
    {"cmd.context.output_budget", "输出上限 {0} tokens[{1}]——thinking 与正文共用这笔预算,已计入 projected 评估"},
    {"cmd.context.output_budget_unset", "输出上限 unset[三级都未声明]——请求不带字段,交服务端/模型默认;projected 评估按 8192 保守估计"},
    {"config.label.file", "  配置文件           = {0}"},
    {"config.hooks.none", "(未配置)"},
    {"config.mcp.count", "{0} 个服务器"},
    {"config.search.none", "(未配置,web_search 工具不注册)"},
    {"config.label.catalog", "  模型目录           = "},
    {"config.catalog.none", "(未配置,{0} 不存在)"},
    {"config.catalog.entries", "{0}({1} 个条目)"},
    {"config.label.catalog_hit", "  当前模型命中目录   = "},
    {"config.catalog.model_unset", "(model 未设置)"},
    {"config.catalog.hit", "是({0})"},
    {"config.catalog.display_name", ",display_name: {0}"},
    {"config.catalog.miss", "否({0} 不在目录里,一切按现状)"},
    {"config.session_model", "  本会话实际在用的 model = {0}"},
    {"config.session_model.note", "  (只在本会话生效,尚未写入配置文件)"},
    {"path.no_home", "<找不到主目录>"},

    // ---- 配置来源 / 常见错误(config 层) ----
    {"config.source.lubancode_env", "LUBANCODE_ 专属环境变量"},
    {"config.source.config_file", "配置文件(.lubancode/config.json)"},
    {"config.source.project_config_file", "项目级配置(.lubancode/config.json)"},
    {"config.source.global_config_file", "全局配置(~/.lubancode/config.json)"},
    {"config.source.generic_env", "通用环境变量(ANTHROPIC_*/OPENAI_*)"},
    {"config.source.default", "内置默认值"},
    {"config.source.unknown", "未知来源"},
    {"error.api_key_missing",
     "缺少 API Key,没有它没法跟模型对话。按优先级从高到低找了这些地方,都没找到:\n"
     "  1) 环境变量 LUBANCODE_API_KEY\n"
     "  2) 配置文件(cwd 或用户主目录的 .lubancode/config.json,旧位置 .lubancode.json 也认,\n"
     "     读到会自动迁移)里的 api_key 字段\n"
     "  3) 通用环境变量 {0}\n"
     "  4) 内置默认值(api_key 没有内置默认值,必须自己配一个)\n"
     "挑一种配上,再重新运行 lubancode。用 --config 能看到当前每个字段实际读到了什么。"},
    {"error.not_configured",
     "缺少配置: {0},没法跟模型对话(lubancode 不内置哪一家的地址/模型,得自己配)。三条途径挑一种:\n"
     "  1) 不带位置参数运行 lubancode,进入交互模式,会自动走初次配置向导\n"
     "  2) 在用户主目录放一份 .lubancode/config.json(旧版 .lubancode.json 也认,读到会自动迁移),把 "
     "{0} 写进去(字段全部可选)\n"
     "  3) 设置对应的环境变量: {1}\n"
     "配好之后用 --config 能看到当前每个字段实际读到了什么、来自哪一级。"},

    // ---- 通用错误 ----
    {"error.prefix", "[错误] "},
    {"error.unexpected", "未预料的异常: {0}"},
    {"error.unknown_command", "不认得命令 {0},试试 /help"},
    {"error.wizard_incomplete", "配置向导未完成,退出。"},
    {"error.system_prompt_arg", "--system-prompt 后面要跟一个文件路径"},
    {"image.attached", "[图片] 已附 {0} ({1}x{2})"},
    {"error.image.missing_path", "图片路径空着。用 /image <路径>，或在消息里写 @路径。"},
    {"error.image.not_found", "找不到图片文件: {0}"},
    {"error.image.not_regular", "这不是普通文件，不能当图片传: {0}"},
    {"error.image.unsupported", "不认得图片格式: {0}(只收 png/jpg/jpeg/gif/webp)。"},
    {"error.image.too_large", "图片太大: {0}(上限 5MB，请先压缩再传)。"},
    {"error.image.read_failed", "图片没读出来: {0}"},
    {"error.image.invalid", "图片内容不对，没法读取尺寸: {0}"},
    {"i18n.pack_warning", "[语言包警告] {0}"},

    // ---- M11:网络超时报错(api 层用,client.cpp/models.cpp 调 cli::trf) ----
    {"error.network.connect_timeout",
     "连接超时:{0} 秒内没能连上服务器,请检查网络、代理设置,或者 base_url 是否正确"},
    {"error.network.stream_idle_timeout",
     "网络读超时:连续 {0} 秒没收到新数据,连接可能已经断了,请重试"},
    {"error.network.request_timeout", "请求超时:{0} 秒内没有完成,请检查网络后重试"},
    {"error.network.connect_failed", "连接失败: {0}"},
    // 流式请求硬墙钟(cpr 并发挂死单):连接/空闲两道闸都不触发的挂死绝境,
    // 由这面墙兜底掐断。文案要点:不是网络慢,是挂死;配置键写明,好让人调。
    {"error.network.hard_timeout",
     "请求硬超时:整枚请求超过 {0} 秒被强制掐断(request_hard_timeout_secs)。多半是连接被代理/TUN "
     "截胡或服务端彻底无响应;重试前先排查网络,长任务可调大此值"},

    // ---- transcript 摘要词(彩色主题;plain 的 [RUNNING] 等不进表) ----
    {"transcript.pending", "待确认"},
    {"transcript.checking_hook", "检查钩子…"},
    {"transcript.hook_blocked", "被钩子拦下(未执行)"},
    {"transcript.read_lines", "读取 {0} 行"},
    {"transcript.exit_code", "退出码 {0}"},
    {"transcript.added", "新增 {0} 行"},
    {"transcript.added_removed", "新增 {0} 行,删除 {1} 行"},
    {"transcript.hits", "命中 {0} 处"},
    {"transcript.agent", "子代理 {0} 步 · {1} 次工具"},
    {"transcript.error_no_output", "Error: (无输出)"},
    {"transcript.error_exit_code", "Error: 退出码 {0}"},
    {"transcript.error_truncated", "(共 {0} 行,Ctrl+E 查看完整)"},
    {"transcript.params_prefix", "参数: "},
    {"transcript.no_full_output", "(无完整输出)"},
    {"transcript.full_output_header", "── 完整输出({0} 行)──"},
    {"transcript.todo_count", "{0} 项"},
    {"transcript.thinking_running", "思考中…"},
    {"transcript.thinking_done", "思考 {0}"},
    {"transcript.thinking_chars", " · {0} 字"},
    {"transcript.thinking_stream_more", "……共 {0} 行,思考结束后 Ctrl+O 看全文"},
    {"transcript.batch_pending", "(本拍排队中)"},
    {"transcript.batch_skipped", "本拍未执行(已打断)"},
    {"transcript.more_lines", " +{0} lines"},
    {"todo.empty", "没有待办。"},

    // ---- 统计行 ----
    {"stats.line", "[tokens] 输入 {0}{1} · 输出 {2} · 请求 {3} 次 · context {4}%"},
    {"stats.cache", "(缓存命中 {0},{1}%)"},
    {"stats.cache_not_reported", "(usage 未报告)"},
    {"stats.cache_disabled", "(服务端未启用前缀缓存)"},
    {"stats.cache_no_hit_enabled", "(缓存已启用,本场未命中)"},
    {"stats.cache_no_hit_unverified", "(缓存 0 命中,服务端是否启用未验证)"},
    {"status.cache_note_hit", "缓存命中 {0}({1}%)"},
    {"status.cache_note_not_reported", "缓存未报告"},
    {"status.cache_note_disabled", "服务端未启用缓存"},
    {"status.cache_note_zero", "缓存 0 命中"},

    // ---- 管道模式稳定输出 ----
    {"pipe.tool_start", "[工具] "},
    {"pipe.tool_done", "[工具完成] "},
    {"pipe.subtool_start", "  [子代理·工具] "},
    {"pipe.todo_updated", "已更新待办清单({0} 项)"},

    // =======================================================================
    // 以下 P1:各命令详细输出、少见提示。en 表暂缺这些键,回退 zh-CN。
    // =======================================================================

    // ---- mcp/插件/tool_search 挂载 ----
    {"mcp.start_failed", "[mcp] {0}: 启动失败 - {1}"},
    {"mcp.init_failed", "[mcp] {0}: 握手失败 - {1}"},
    {"mcp.list_failed", "[mcp] {0}: 获取工具清单失败 - {1}"},
    {"mcp.mounted", "[mcp] {0}: {1} 个工具已挂载"},
    {"plugin.mounted_line", "[plugin] {0}: {1} 个工具"},
    {"ptc.fallback_line", "[ptc] programmatic 工具调用未启用,回落 JSON: {0}"},
    {"ptc.probe_failed", "[ptc] Python 探测失败: {0}"},
    {"tool_search.enabled", "[tool_search] 工具超过阈值 {0},MCP/插件等外挂工具改为延迟挂载(/tools 看三态)"},
    {"catalog.warning", "[models.json 警告] {0}"},
    {"settings.local.warning", "[settings.local.json 警告] {0}"},
    {"settings.local.persist_prompt", "也永久写进项目 settings.local.json?[y/N] "},
    {"settings.local.persisted", "已永久允许 {0}(项目级)"},
    {"settings.local.persist_failed", "写 settings.local.json 失败:{0}"},
    {"ask_user.other", "自己填写"},
    {"ask_user.discuss", "聊聊这个问题"},
    {"ask_user.panel_title", "需要你决定"},
    {"ask_user.select_prompt", "请选择编号(Esc 取消): "},
    {"ask_user.multi_prompt", "请选择编号,多项用逗号分隔(Esc 取消): "},
    {"ask_user.menu_hint", "Enter 选择 · ↑/↓ 移动 · 直接键入可自填 · Esc 取消"},
    {"ask_user.menu_multi_hint", "空格勾选 · Enter 提交 · ↑/↓ 移动 · Esc 取消"},
    {"ask_user.menu_select_one", "请至少勾选一项"},
    {"ask_user.menu_edit_hint", "直接输入答案 · Backspace 删除 · Enter 提交 · Esc 取消"},
    {"ask_user.custom_prompt", "请输入你的答案: "},
    {"ask_user.cancelled", "用户取消了选择"},
    {"ask_user.declined", "用户选择不回答"},
    {"ask_user.discuss_prompt", "你想补充什么: "},
    {"ask_user.discuss_empty", "补充内容不能为空。"},
    {"ask_user.discussion_recorded", "补充:"},
    {"ask_user.invalid", "选项无效,请重新输入。"},
    {"ask_user.custom_empty", "答案不能为空。"},
    {"ask_user.recorded", "已选择:"},

    // ---- /tools ----
    {"cmd.tools.no_deferral", "工具共 {0} 个,{1},全量直挂,tool_search 延迟机制未启用。"},
    {"cmd.tools.threshold_zero", "阈值 0(永不延迟)"},
    {"cmd.tools.below_threshold", "低于阈值 {0}"},
    {"cmd.tools.enabled", "tool_search 延迟挂载已启用(阈值 {0},loaded 集合会话级,/clear 不清)。"},
    {"cmd.tools.core", "核心工具(恒在){0} 个:"},
    {"cmd.tools.loaded", "已加载的延迟工具 {0} 个:"},
    {"cmd.tools.none_loaded", "  (还没有,模型用 tool_search 命中后会出现在这里)"},
    {"cmd.tools.pending", "延迟未加载 {0} 个(在系统提示索引段里,检索后挂载):"},

    // ---- /plugins ----
    {"cmd.plugins.empty",
     "没有挂载任何插件工具。\n\n"
     "插件目录约定(放进去,下次启动即挂载):\n"
     "  process:   {0}/<插件id>/plugin.json(Python/Rust/任意可执行程序;\n"
     "      起步用 `lubancode plugin init python <名字>` 生成三件套,示例在\n"
     "      examples/plugins/local_math/)\n"
     "  Lua:       {0}/*.lua\n"
     "      每个文件 return { name=..., description=..., input_schema=...,\n"
     "      execute=function(input) ... end } 一张表(缺省 pure 画像,关 io/\n"
     "      os.execute;死循环有指令预算落锤),示例在 examples/plugins/word_count.lua\n"
     "  native:    {0}/*.dll(Windows)/*.so(Linux)/*.dylib(macOS)\n"
     "      导出 luban_plugin_entry(ABI v2,见 include/luban_plugin.h),示例在\n"
     "      examples/plugins/hello_plugin/。库跟宿主同进程,插件里崩了整个程序\n"
     "      一起完蛋,装谁的插件风险自担。"},
    {"cmd.plugins.mounted", "已挂载 {0} 个插件工具:"},
    {"cmd.plugins.warnings", "加载警告(这些没挂上):"},

    // ---- /plugin 子命令(plugins 单第 8 步) ----
    {"cmd.plugin.usage",
     "用法: /plugin inspect <id> | doctor <id> | reload <id> | enable <id> | disable <id>。裸 /plugin <id> "
     "视同 inspect。"},
    {"cmd.plugin.not_found", "找不到插件 {0}(/plugins 看看挂载账)。"},
    {"cmd.plugin.inspect.header", "插件 {0} v{1}(runtime={2}, language={3})"},
    {"cmd.plugin.inspect.legacy_header", "插件 {0}(legacy {1} 插件,无 plugin.json,详情看文件本体):"},
    {"cmd.plugin.inspect.dir", "目录: {0}"},
    {"cmd.plugin.inspect.argv", "命令: {0}"},
    {"cmd.plugin.inspect.timeout", "超时: {0}ms"},
    {"cmd.plugin.inspect.env", "环境变量 allowlist: {0}"},
    {"cmd.plugin.inspect.tools", "工具 {0} 件:"},
    {"cmd.plugin.doctor.command_ok", "解释器可用: {0}({1})"},
    {"cmd.plugin.doctor.command_bad", "解释器起不来: {0}({1})——检查 command 或装好解释器。"},
    {"cmd.plugin.doctor.not_process", "这不是 process 插件,doctor 只查 process 的解释器环境。"},
    {"cmd.plugin.doctor.legacy_ok", "{0} 插件在挂载账上(内嵌运行时,无外部环境依赖)。"},
    {"cmd.plugin.test.hint",
     "test 与模型调用同一条链(schema 验参、确认、超时),命令层不开无防护捷径——直接让模型调这件工具,"
     "或用插件自带的测试脚本(如 python test_runner.py)离线自测。"},
    {"cmd.plugin.reload.hint",
     "v1 的 reload 以重启为口径:改完插件重启 LubanCode 即生效。Lua/process 的会话内热重载是后续批次,"
     "不在这硬造半套。"},
    {"cmd.plugin.toggle.hint",
     "enable/disable 的持久账(逐插件开关,落 settings)是后续批次;v1 想临时停用,把插件目录挪出 "
     "plugins/ 再重启即可。"},
    {"cmd.plugin.unknown_sub", "不认得的子命令: {0}"},

    // ---- plugin init 子命令(plugins 单第 3 步) ----
    {"plugininit.no_home", "找不到用户主目录,无法定位插件目录。"},
    {"plugininit.failed", "生成插件脚手架失败: {0}"},
    {"plugininit.done", "已生成 Python 插件脚手架 {0}({1}):"},
    {"plugininit.doctor_note", "提示: {0}"},
    {"plugininit.next",
     "下一步:改 runner.py 里的 HANDLERS 与 plugin.json 里的 tools,本地先跑 python test_runner.py "
     "自测;重启 LubanCode 后 /plugins 可见。"},
    {"plugininit.lua_hint",
     "Lua 插件不需要脚手架:把 return {{ name=..., execute=function(input) ... end }} 的 .lua 文件"
     "放进 {0} 即可,示例见 examples/plugins/word_count.lua。"},
    {"plugininit.unknown_template", "不认得的插件模板: {0}(v1 只有 python)"},

    // ---- /mcp、/lsp ----
    {"cmd.mcp.empty", "没有挂载任何 MCP 服务器(config.json 里没写 mcpServers,或者配了但全部启动失败)。"},
    {"mcp.state.alive", "运行中"},
    {"mcp.state.dead", "已退出"},
    {"cmd.mcp.line", "  - {0}: {1}, {2} 个工具"},
    {"cmd.lsp.empty", "没有配置任何 LSP 服务器(config.json 里没写 lsp 段,lsp 工具也没注册)。"},
    {"cmd.lsp.header", "已配置 {0} 个 LSP 服务器:"},

    // ---- /skills ----
    {"cmd.skills.empty",
     "还没有扫描到任何技能。\n\n"
     "技能目录约定(先建目录,再放一份 <技能名>/SKILL.md):\n"
     "  项目级: {0}/.lubancode/skills/<技能名>/SKILL.md\n"
     "  主目录级: {1}/.lubancode/skills/<技能名>/SKILL.md\n\n"
     "SKILL.md 起手要有 YAML frontmatter(name/description 两个字段,后面跟正文):\n"
     "  ---\n"
     "  name: 技能名\n"
     "  description: 一句话说明这个技能是干什么的、什么时候该用\n"
     "  ---\n"
     "  正文写具体怎么做。"},
    {"cmd.skills.header", "已扫描到 {0} 个技能:"},
    {"cmd.skills.no_desc", "(没写说明)"},

    // ---- /skill ----
    {"cmd.skill.usage",
     "技能管理(用户级安装后,本会话立即可用):\n"
     "  /skill list\n"
     "      列出用户级与项目级技能,并标明本地/远端来源。\n"
     "  /skill install https://example.com/my-skill.md\n"
     "  /skill install https://github.com/owner/repo\n"
     "      从 Markdown 直链或 GitHub 技能仓库安装。\n"
     "  /skill install C:\\path\\to\\my-skill\n"
     "  /skill install C:\\path\\to\\my-skill\\SKILL.md\n"
     "      从本地目录或 SKILL.md 安装;路径带空格也认。\n"
     "  /skill update [名字]\n"
     "      更新装过且记有网址来源的技能;不写名字则更新全部。\n"
     "  /skill remove <名字>\n"
     "      删除用户级技能。\n"
     "用户级落盘: ~/.lubancode/skills/<名字>/SKILL.md\n"
     "项目级手工放置: <cwd>/.lubancode/skills/<名字>/SKILL.md"},
    {"cmd.skill.no_home", "找不到用户主目录，技能没处安放。"},
    {"cmd.skill.list_empty", "这里还没有技能。用 /skill install <网址或本地路径> 装一份。"},
    {"cmd.skill.list_header", "本机技能:"},
    {"cmd.skill.local", "本地自建"},
    {"cmd.skill.remote", "远端 {0}，安装于 {1}"},
    {"cmd.skill.scope_global", "主目录级"},
    {"cmd.skill.scope_project", "项目级"},
    {"cmd.skill.install_done", "已安装: {0}"},
    {"cmd.skill.update_done", "已更新: {0}"},
    {"cmd.skill.update_none", "没有记过来源的远端技能可更新。"},
    {"cmd.skill.remove_done", "已删除: {0}"},
    {"cmd.skill.error", "{0}失败: {1}"},
    {"cmd.skill.refreshed", "技能清单已刷新，本会话后续对话也能用了。"},

    // ---- /context、/compact、/think ----
    {"cmd.context.usage", "上下文占用: {0} / {1} tokens ({2}%)"},
    {"cmd.context.compact_hint", "  —— 接近上限了,建议 /compact 一下"},
    {"cmd.context.window_changed", "上下文窗口已改成 {0} tokens(只本会话生效,没改配置文件)。"},
    // /context 裸敲的分组标题(第四期起分组卡片式布局)。
    {"cmd.context.group.usage", "占用"},
    {"cmd.context.group.cache", "缓存"},
    {"cmd.context.group.structure", "结构与回收"},
    {"cmd.context.group.budget", "预算与角色账"},
    // /context 裸敲的分类占用分析(拼装规则见 FormatContextBreakdown)。
    {"cmd.context.bd.header", "上下文占用分析(窗口 {0})"},
    {"cmd.context.bd.system", "系统提示"},
    {"cmd.context.bd.tools", "工具定义"},
    {"cmd.context.bd.history", "对话历史"},
    {"cmd.context.bd.used", "已用"},
    {"cmd.context.bd.threshold", "自动压缩线"},
    {"cmd.context.bd.remaining", "剩余"},
    {"cmd.context.bd.cache", "(缓存命中 {0},{1}%)"},
    {"cmd.context.bd.cache_no_ratio", "(缓存命中 {0})"},
    {"cmd.context.epoch", "前缀 epoch {0}:命中 {1} / 总输入 {2}({3}%)"},
    {"cmd.context.cache_session", "会话累计:命中 {0} / 总输入 {1}({2}%)"},
    {"cmd.context.cache_history_header", "逐轮命中(最近 {0} 轮,最旧在前):"},    {"cmd.context.cache_history_row", "输入 {0} / 命中 {1}({2}%)"},
    {"cmd.context.bd.measured", "(实测)"},
    {"cmd.context.bd.history_derived", "(=实测总量−系统−工具)"},
    {"cmd.context.bd.note.measured", "(总量为上一轮实测 token;系统提示/工具为字符估,历史为实测总量反推)"},
    {"cmd.context.bd.note.est", "(尚无实测,启动估算:统一口径,ASCII 4 字符约 1 token、非 ASCII 每字约 1.5 token,实际以模型返回为准)"},
    {"cmd.context.note.semantics", "(context = 主会话最近一次请求的占用,不是累计花销,不含独立子代理的 token)"},
    {"cmd.context.note.stale", "(最近一次请求未返回 usage,以上为再上一次的实测值;状态栏同款数字带 ~ 前缀)"},
    {"cmd.compact.empty", "当前没有对话历史,不用压缩。"},
    {"cmd.compact.failed", "压缩失败: {0}"},
    {"cmd.compact.result", "压缩前 ~{0} tokens → 压缩后 ~{1} tokens(统一估算口径)"},
    {"cmd.compact.window_unknown", "(压缩模型窗口未知,本次未做窗口校验)"},
    {"cmd.compact.hierarchical", "历史装不进单次压缩:按任务阶段分了 {0} 块(map)归并成终稿(reduce 轮次 {1})。"},
    {"cmd.compact.manifest", "manifest 守恒校验通过:约束 {0} 条 / 待办 {1} 条"},
    {"cmd.compact.dryrun.header", "/compact --dry-run:只算不动手,历史与请求都没改。"},
    {"cmd.compact.dryrun.reclaim", "结构压缩可回收约 {0} 字节(精确重复 {1} 处 · 旧版读取被覆盖 {2} 项 · 长结果外置 {3} 项;每轮请求已自动生效)"},
    {"cmd.compact.dryrun.pinned", "钉住不压:最近热区 ~{0} tokens · 活动待办 {1} 条(压缩时逐字守恒)"},
    {"compact.auto_start", "[compact] 上下文接近上限,自动压缩中..."},
    {"compact.auto_done", "[compact] 自动压缩完成。"},
    {"compact.auto_failed", "[compact] 自动压缩失败: {0}"},
    {"compact.auto_failed_tail", "(继续按原历史发送,字符数安全网仍会兜底)"},
    {"compact.midturn_start", "[compact] 工具循环中途,预计下一次请求将超出窗口,先收一次历史..."},
    {"compact.midturn_done", "[compact] mid-turn 压缩完成,工具循环继续。"},
    {"compact.done_stats", "[compact] 历史 ~{0} tokens;manifest 守住约束 {1} 条 / 待办 {2} 条"},
    {"compact.hard_trim_turns", "[警告] 上下文发生有损硬裁剪:中间 {0} 条消息被丢弃(字符安全网兜底,不是语义压缩)。模型已看不到那段原文;完整流水仍在会话存档,可 /export 查看、/compact 重建摘要。"},
    {"compact.hard_trim_results", "[警告] 上下文发生有损硬裁剪:超大工具结果被截尾(字符安全网兜底,不是语义压缩)。模型已看不到被截内容;完整流水仍在会话存档,可 /export 查看。"},

    // ---- 模型路由(cheap/normal/lao 分工第一期):状态栏短闪与回退留痕 ----
    {"router.compact_flash", "压缩 {0} → {1} · {2}"},
    {"router.task_flash", "{0} · {1}"},
    {"router.fallback_flash", "{0} 不可用,已回落 {1}"},
    {"router.usage.header", "模型调用分角色账(本会话累计):"},
    {"router.usage.fallback_header", "回退记录:"},

    // ---- 可追回 artifact(渐进式上下文仓第二期) ----
    {"artifact.store_open_failed", "[artifact] 上下文仓开不了({0}),超长结果退回内存全文,不产生假引用。"},
    {"cmd.context.artifacts", "artifact 层:{0} 枚落盘 · 全文共 {1} 字节可追回(context_search/context_read 按 id 检索)"},
    {"cmd.context.artifacts_none", "artifact 层:本会话尚无落盘的超长工具结果。"},

    // ---- ContextBudgetPlan 与分层占用(第四期,/context 展示) ----
    {"cmd.context.layers", "分层占用:inline 全文 {0} 枚 · artifact 预览(L1){1} 枚"},
    {"cmd.context.reclaimable", "结构压缩最近一次请求回收 ~{0} 字节(重复收敛 + 长结果外置)"},
    {"cmd.context.budget", "预算总账:窗口 {0} · 开销 {2} · 可压缩历史 {1}(统一估算口径)"},
    {"cmd.context.budget_detail", "  开销明细:system+模型指令 {0} · 工具声明 {1} · 热区 {2} · 输出预留 {3} · 压缩指令+协议 {4} · 估算误差边 {5}"},
    {"cmd.context.compact_budget", "压缩预算:单次压缩请求输入上限 {0} · 摘要产出目标 {1}(两只数不混用)"},
    {"cmd.context.next_line", "下一触发线:{0}(窗口 80%) · 当前 {1} · {2}"},
    {"cmd.context.next_line_over", "已越线,下一轮发送前会自动压缩"},
    {"cmd.context.last_compact", "最近一次 compact:{0}"},
    {"cmd.think.current", "当前推理强度: {0}"},
    {"cmd.think.catalog_header", "模型目录声明的档位({0}):"},
    {"cmd.think.provider_header", "provider 声明的档位(请求参数 {0}):"},
    {"cmd.think.unverified", "当前模型与 provider 都没声明档位——未经能力验证。"},
    {"cmd.think.unverified_send", "(未经能力验证,仍会按原样发送)"},
    {"cmd.think.provider_declared", "(在 provider 声明表内)"},
    {"cmd.think.provider_undeclared", "(不在 provider 声明表内,仍会发送)"},
    {"cmd.think.doctor_hint", "提示: /doctor effort [档位|unset] 发极小探针,可实测服务端是否采纳该档。"},
    {"cmd.think.provider", "支持哪些档位以服务商为准。"},
    {"cmd.think.switched", "推理强度已切到 {0}(本会话生效)。"},
    {"cmd.think.undeclared", "提示: 模型目录未声明该档,仍会发送。"},

    // ---- /doctor:本地兼容端 Effort 与前缀缓存诊断(2026-08 单) ----
    {"doctor.usage.usage_line", "用法: /doctor effort [档位|unset] | /doctor cache [probe|usage] | /doctor agents | /doctor shell"},
    {"doctor.overview.header", "诊断概览(不发请求,只看当前声明与结论):"},
    {"doctor.overview.effort", "  当前档位: "},
    {"doctor.overview.declared", "  档位声明: "},
    {"doctor.overview.unverified", "未经能力验证(模型目录与 provider 都没声明)"},
    {"doctor.overview.levels", "已声明 {0} 档(/think 裸敲可列)"},
    {"doctor.overview.cache", "  前缀缓存: "},
    {"doctor.overview.usage_hint", "探针要发请求: /doctor effort [档位|unset] 实测档位; /doctor cache 读指标、probe 对账、usage 探 stream_usage。"},
    {"doctor.level.unset", "未发送参数"},
    {"doctor.startup.stream_usage_hint", "提示: 当前端未声明 stream_usage,token/缓存统计可能恒为 0;/doctor cache usage 可探测并写回配置。"},
    {"doctor.effort.probe_header", "Effort 探针:模型 {0},档位 {1}。"},
    {"doctor.effort.request_field", "请求侧实际发送值:"},
    {"doctor.effort.field_absent", "未发送参数(请求体无 {0} 字段)"},
    {"doctor.effort.mapped_suffix", " 档映射)"},
    {"doctor.probe.sending", "发送探针(极小请求;密钥与正文不进报告)…"},
    {"doctor.effort.http_ok", "HTTP 2xx:请求被服务端接受。"},
    {"doctor.effort.http_error", "HTTP {0}:请求被拒或出错。"},
    {"doctor.effort.finish", "finish_reason:"},
    {"doctor.value.absent", "(未报告)"},
    {"doctor.effort.body", "正文 {0} 字符 · 思考 {1} 字符。"},
    {"doctor.effort.usage", "usage:输入 {0} · 输出 {1} · 缓存命中 {2}"},
    {"doctor.effort.usage_reasoning", "usage 拆账:输出里 reasoning {0} tokens。"},
    {"doctor.effort.usage_no_split", "usage 未拆 reasoning 账(服务端没回 reasoning_tokens)。"},
    {"doctor.effort.usage_not_reported", "usage:未报告(服务端没回 usage——chat 端常见原因是 stream_usage 没开,/doctor cache usage 可探)。"},
    // /doctor agents:main 与各 agent type 的差异矩阵(规格"架构落点")。
    {"doctor.agents.header", "main 与各 agent type 的能力矩阵(默认同级;差异来自角色或显式配置)"},
    {"doctor.agents.budget",
     "共用运行策略:输出上限 {0}(0 = unset,交服务端默认) · 步数 {1} · length 续跑 {2} 次"},
    {"doctor.agents.governance", "派工治理:并发槽 ≤ {0}(subagent.max_active) · 深度 ≤ {1}(subagent.max_depth)"},
    {"doctor.agents.row_main", "main        :{0} 枚工具(含 agent/todo/ask_user)"},
    {"doctor.agents.row_sub",
     "general-purpose:{0} 枚工具(与 main 同能力;todo 为每任务私有实例,可再派 agent)"},
    {"doctor.agents.row_explore", "Explore     :{0} 枚工具(只读白名单,角色限制——不是子代理无权限)"},
    {"doctor.agents.note",
     "注:输出上限/步数/续跑/并发/深度 main 与子代理同一份(runtime profile);仅 Explore 按角色收窄工具。"},
    {"doctor.agents.subagent_debug_log",
     "子代理流诊断:设 LUBANCODE_DEBUG_SUBAGENT=1 后,每个子代理任务逐流事件一行落 "
     "~/.lubancode/logs/subagent-<任务号>.log(只记事件类型与字节数,不记正文与思考;也可设成别的目录)。"},
    {"doctor.cache.no_metrics", "未配 metrics_url,读不到服务端指标。本地兼容端可在 provider 配置里写 metrics_url(如 http://127.0.0.1:8000/metrics)后重试;不擅自拿 base_url 猜端点去探。"},
    {"doctor.cache.metrics_header", "服务端指标({0}):"},
    {"doctor.cache.metrics_enabled", "enable_prefix_caching = True:服务端已启用前缀缓存。"},
    {"doctor.cache.metrics_disabled", "enable_prefix_caching = False:服务端未启用前缀缓存——\"缓存利用率 0%\" 是服务端没开,不是提示词没守住前缀。"},
    {"doctor.cache.metrics_enabled_unknown", "指标里没有 enable_prefix_caching 标签,启用状态未知。"},
    {"doctor.cache.metrics_counters", "prefix_cache_queries_total = {0} · prefix_cache_hits_total = {1} · prompt_tokens_cached_total = {2}"},
    {"doctor.cache.metrics_read_failed", "{0}"},
    {"doctor.cache.state.unverified", "未验证(没读过服务端指标)"},
    {"doctor.cache.probe_gate", "当前端不是本机地址且未明配 metrics_url,不发探针——公网 provider 不擅自发请求。确属自有端,请在 provider 配置里写 metrics_url 后重试。"},
    {"doctor.cache.probe_round", "第 {0} 轮(同 system、同历史前缀,只换最后一句):"},
    {"doctor.cache.probe_usage", "  usage:非缓存输入 {0} · cached_tokens 命中 {1}"},
    {"doctor.cache.probe_prefix", "前缀字节:两轮请求体公共前缀 {0} 字节(设计前缀 {1} 字节)"},
    {"doctor.cache.probe_prefix_stable", " —— 稳定"},
    {"doctor.cache.probe_prefix_broken", " —— 不稳(公共前缀比设计短,序列化在改写前缀)"},
    {"doctor.cache.probe_delta", "服务端增量:queries {0} · hits {1} · cached tokens {2}"},
    {"doctor.error.truncated", "…(截断)"},
    {"doctor.usage.not_chat", "stream_usage 探针只对 chat_completions 端有意义;anthropic/responses 的 usage 本来就在流末回。"},
    {"doctor.usage.no_provider", "当前会话没走 providers[] 条目(单 provider 顶层写法),探针结论没有落盘的地方——改用 providers 配置后可用。"},
    {"doctor.usage.probing", "发一只带 stream_options.include_usage 的极小请求,看服务端认不认…"},
    {"doctor.usage.supported", "服务端认 stream_options.include_usage:流末回了 usage。"},
    {"doctor.usage.unsupported", "服务端没回 usage:大概率不认 stream_options(或该端不回报 usage)。"},
    {"doctor.usage.written", "已写回 provider {0} 的 stream_usage(配置文件 {1}),后续请求生效。"},
    {"doctor.usage.write_failed", "写回失败: {0}"},

    // ---- 模型目录应用 / /model ----
    {"catalog.apply_think", "think→{0}(目录默认)"},
    {"catalog.apply_window", "上下文窗口→{0} tokens(目录声明)"},
    {"catalog.apply_instructions", "base_instructions 已注入系统提示(目录条目 {0},下一轮请求生效)"},
    {"cmd.model.fetch_failed", "拉取模型列表失败: {0}"},
    {"cmd.model.list_empty", "接口返回的模型列表是空的。"},
    {"cmd.model.current", "  ← 当前"},
    {"cmd.model.choose", "选择模型编号 [{0}]（Esc 取消）: "},
    {"cmd.model.cancelled", "已取消模型切换。"},
    {"cmd.model.bad_number", "编号不对,取消切换。"},
    {"cmd.model.not_number", "没听懂,取消切换。"},
    {"cmd.model.switched", "已切换到模型: {0}(本会话生效)"},
    {"cmd.model.switched_with_provider", "已切换到模型: {0}(provider {1},本会话生效)"},
    {"cmd.model.other_provider_note", "备注:目录里 {0} 这名字属 {1} 家(未配置);当前家若不认它,可用 /provider 切换。"},
    // ccmoon 真机巡检单 P1(归属误报与端点能力):键名带 model. 前缀,与
    // 思考流一族的键不沾边,单独一笔提交,冲突上游手解。
    {"cmd.model.catalog_also_lists", "备注:{0} 家目录也收录 {1};当前家认不认以实测为准,不行再用 /provider 切换。"},
    {"cmd.model.hop_ambiguous", "备注:{0} 在多家已配目录里都有({1}),已留在当前家,不自动跳。"},
    {"cmd.model.live_list_header", "以下 {0} 项由 {1} 真机列出;从这张单选出的模型按 {1} 本家切换。"},
    {"cmd.model.static_list_header", "真机列表拉取失败({0});改列本地目录(静态缓存,可能与真机不一致):"},
    {"cmd.model.realtime_hint", "提醒:{0} 是 Realtime 端点模型,当前连接走 {1},选下去大概率报错;这只说明这家中转的 {1} 路由多半不通,不判模型死刑。"},
    {"cmd.model.remember_choice_failed", "本次选择没落痕: {0}(不影响切换)"},
    {"cmd.model.other_provider_unswitchable", "{0} 属 {1} 家;此处切不动 provider,连接未换。"},
    {"cmd.model.provider_key_missing", "{0} 家缺 API key,连接未换;可用 /provider switch {0} 先补齐密钥。"},
    {"cmd.model.role_switched", "{0} 角色 → {1}(本会话生效)"},
    {"cmd.model.role_unknown", "不认得模型角色: {0}(只认 normal/cheap/lao,plan 是 lao 的别名)。"},
    {"cmd.model.roles_header", "三档模型角色(按任务路由;子代理当前随会话模型;未配置的角色回落 normal):"},
    {"cmd.model.roles_unavailable", "模型路由未建(单发/测试路径),/model roles 只在交互会话可用。"},
    {"cmd.write_config_prompt", "写进配置文件 {0}? [y/N]: "},
    {"cmd.write_config.updated", "已更新 {0}"},
    {"cmd.write_config.failed", "更新失败: {0}"},
    {"cmd.session_only", "当前没有生效的配置文件,只在本会话生效。"},

    // ---- /provider ----
    {"cmd.provider.usage",
     "用法:\n"
     "  /provider list\n"
     "  /provider refresh                       从 LubanCode 仓库更新常见厂家目录\n"
     "  /provider add                          进分步向导(裸敲)\n"
     "  /provider add <名字>                    进分步向导(名字先给上,跳过第一问)\n"
     "  /provider add <名字> <base_url> <anthropic|responses|chat_completions> [--key-env 环境变量名] [--key 明文key] "
     "[--model 默认模型] [--effort 推理档位] [--window 大小]\n"
     "  /provider switch <名字> [模型]\n"
     "  /provider remove <名字>\n"
     "  /provider set <名字> native_web_search on|off   开关服务端原生联网搜索(也认 true/false、1/0)\n"
     "  /provider set <名字> extra_body <JSON object>   设置该端每次请求要附带的额外顶层字段(浅合并,"
     "覆盖内置字段;传 {} 或空清掉)\n"
     "  /provider set <名字> extra_header <头名> <值>    设置该端每次请求要附带的额外 HTTP 头(同名覆盖"
     "内置头;值留空删掉这条)"},
    {"cmd.provider.empty", "还没有配 provider。用 /provider add 添一个。"},
    {"cmd.provider.header", "已配 provider:"},
    {"cmd.provider.line", "  - {0} [{1}] {2}; model={3}; window={4}; {5}{6}{7}"},
    {"cmd.provider.current", " (当前)"},
    {"cmd.provider.model_unset", "(未设置)"},
    {"cmd.provider.extra_api_key", "; api_key={0}"},
    {"cmd.provider.extra_effort", "; effort={0}"},
    {"cmd.provider.extra_web_search", "; native_web_search=on"},
    {"cmd.provider.extra_body_hint", "; extra_body={0}键"},
    {"cmd.provider.extra_headers_hint", "; extra_headers={0}条"},
    {"cmd.provider.added", "已添 provider {0},写进全局配置 {1}。"},
    {"cmd.provider.add_cancelled", "已取消,没有添加 provider。"},
    {"cmd.provider.add_failed", "添 provider 失败: {0}"},
    {"cmd.provider.add_kept_connection", "已保存 {0},但它没配默认模型,本次未切换;当前会话仍用原连接,配好模型后可用 /provider switch {0} 再切。"},
    {"cmd.provider.exists", "provider 已存在: {0}"},
    {"cmd.provider.switched", "已切到 provider {0},后续请求走 {1}。"},
    {"cmd.provider.remembered", "已记住 provider {0},下次启动仍用它。"},
    {"cmd.provider.remember_failed", "provider 已切换,但没能记住:{0}"},
    {"cmd.provider.effort_applied", "已按 provider {0} 的配置,把推理档位设为 {1}。"},
    {"cmd.provider.not_found", "找不着 provider: {0}"},
    {"cmd.provider.key_missing", "provider {0} 要环境变量 {1},眼下没取到值。"},
    {"cmd.provider.key_missing_inline", "provider {0} 配的是明文 key(auth=inline),但 api_key 是空的,先用 /provider set {0} auth env|none 换个模式,或补上 key。"},
    {"cmd.provider.auth_none", "无需鉴权"},
    {"cmd.provider.auth_env_prompt", "环境变量名(读 key 用): "},
    {"cmd.provider.auth_inline_prompt", "API key(明文落盘,展示会打码): "},
    {"cmd.provider.auth_aborted", "已取消,配置没改。"},
    {"cmd.provider.switch.usage_short", "用法: /provider switch <名字> [模型]"},

    // ---- /provider 子命令容错(容错单) ----
    {"cmd.provider.typo_hint", "没认得 `{0}`,是不是想敲 `{1}`?"},
    {"cmd.provider.bad_args", "参数不对。"},
    {"cmd.provider.unknown_sub.tty",
     "没认得的子命令: {0}。常用的:\n"
     "  /provider add       添一家\n"
     "  /provider switch    换一家\n"
     "  /provider list      看已配"},
    {"cmd.provider.unknown_sub.pipe", "用法: /provider <子命令>;敲 /provider list 看已配的 provider。"},
    {"cmd.provider.usage_short.list", "用法: /provider list"},
    {"cmd.provider.usage_short.refresh", "用法: /provider refresh"},
    {"cmd.provider.usage_short.add",
     "用法: /provider add [名字](进向导),或 /provider add <名字> <base_url> <anthropic|responses|chat_"
     "completions> [--key-env 变量名] [--key 明文key] [--model 模型] [--effort 档位] [--window 大小]"},
    {"cmd.provider.usage_short.remove", "用法: /provider remove <名字>"},
    {"cmd.provider.usage_short.set", "用法: /provider set <名字> <字段> <值>(字段: auth、native_web_search、extra_body、extra_header)"},
    {"cmd.provider.usage_short.edit", "用法: /provider edit <名字>(裸敲开选择列表)"},

    // ---- /provider edit(容错单) ----
    {"cmd.provider.edit.saved", "已保存 provider {0} 的改动,写进全局配置 {1}。"},
    {"cmd.provider.edit.save_failed", "存 provider 改动失败: {0}"},
    {"cmd.provider.edit.cancelled", "已取消,配置没改。"},

    // ---- /provider switch 选择器(向导重排单) ----
    {"provider_switch.title", "切换 provider"},
    {"provider_switch.footer", "↑↓ 选择  Enter 切换  Esc 取消  输入文字筛选(筛选词为空时按 e 编辑选中项)"},
    {"provider_switch.edit_title", "编辑哪个 provider?"},
    {"provider_switch.footer_edit", "↑↓ 选择  Enter 编辑  Esc 取消  输入文字筛选"},
    {"provider_switch.filter_line", "筛选: {0}"},
    {"provider_switch.filter_empty", "(未输入)"},
    {"provider_switch.empty_hint", "还没有 provider。"},
    {"provider_switch.no_match_hint", "没有匹配的 provider。"},
    {"provider_switch.opt_add", "添加 provider"},
    {"provider_switch.opt_cancel", "取消"},
    {"provider_switch.auth_ready", "可用"},
    {"provider_switch.auth_env_missing", "缺密钥(需要 {0})"},
    {"provider_switch.auth_inline_missing", "缺明文 key"},

    // ---- /provider switch 缺密钥补救页 ----
    {"provider_remedy.title", "还不能切到 {0}"},
    {"provider_remedy.body_env", "所需环境变量:{0}\n当前进程没有读到值。"},
    {"provider_remedy.body_inline", "{0} 配的是明文 key(auth=inline),但 api_key 是空的。"},
    {"provider_remedy.footer", "↑↓ 选择  Enter 确认  Esc 返回列表  Ctrl+C 取消"},
    {"provider_remedy.opt_input_key", "现在输入 API key"},
    {"provider_remedy.hint_env", "先选保存去处:只供本次会话,或写入用户配置"},
    {"provider_remedy.hint_inline", "贴一枚 key 落到 api_key"},
    {"provider_remedy.opt_change_env", "改用另一个环境变量"},
    {"provider_remedy.opt_no_auth", "设为无需鉴权"},
    {"provider_remedy.opt_howto", "查看设置方法"},
    {"provider_remedy.opt_back", "返回 provider 列表"},
    {"provider_remedy.key_session", "只供本次会话"},
    {"provider_remedy.key_session_desc", "不写盘,重启后仍按原配置"},
    {"provider_remedy.key_persist", "写入用户配置"},
    {"provider_remedy.key_persist_desc", "明文落盘,展示一律打码,风险自担"},
    {"provider_remedy.key_saved", "已把 {0} 的 key({1})写进 {2}。"},
    {"provider_remedy.key_session_only", "key({0})只供本次会话,不落盘。"},
    {"provider_remedy.none_saved", "已把 {0} 设为无需鉴权,写进 {1}。"},
    {"provider_remedy.howto_powershell", "PowerShell: $env:{0} = \"你的key\""},
    {"provider_remedy.howto_cmd", "cmd: setx {0} \"你的key\""},
    {"provider_remedy.howto_posix", "POSIX shell: export {0}=你的key"},
    {"provider_remedy.howto_restart", "设完后要重启 LubanCode(或重开这个终端)才读得到。"},
    {"cmd.provider.removed", "已删 provider {0},全局配置在 {1}。"},
    {"cmd.provider.remove_active", "provider {0} 正在用，先切到别处再删。"},
    {"cmd.provider.remove_failed", "删 provider 失败: {0}"},
    {"cmd.provider.set_ok", "已把 provider {0} 的 {1} 设为 {2},写进全局配置 {3}。"},
    {"cmd.provider.set_failed", "设置 provider 失败: {0}"},
    {"cmd.provider.set_unknown_field", "不认得的字段: {0}(眼下只认 native_web_search、extra_body、extra_header、auth)"},
    {"cmd.provider.set_active_applied", "provider {0} 正在用，已立即生效，不用再 /provider switch。"},
    {"cmd.provider.extra_body_invalid_json", "extra_body 不是合法 JSON: {0}"},
    {"cmd.provider.extra_body_not_object", "extra_body 得是一个 JSON object(花括号包着的键值对),不是别的类型。"},
    {"cmd.provider.extra_header_name_missing", "extra_header 得跟一个头名字,不能光给值。"},

    // ---- /soul、/prompt ----
    {"soul.unavailable", "[soul] 无法读取 {0},已按无魂运行。"},
    {"cmd.soul.no_home", "找不到用户主目录,魂文件没处安身,/soul 用不了。"},
    {"cmd.soul.available_header", "可选旧魂(输入名字切换):"},
    {"cmd.soul.default_item", "  - default(主目录 SOUL.md)"},
    {"cmd.soul.current", "当前生效: {0}"},
    {"cmd.soul.empty_note", "(内容空白,无效果)"},
    {"cmd.soul.usage", "用法:/soul 看当前;/soul 内容 写进 SOUL.md;/soul clear 还原默认;/soul 名字 切备选魂;/soul off 关;/soul default 回 SOUL.md。"},
    {"cmd.soul.off", "魂已关(本会话生效,下一轮请求换新系统提示)。"},
    {"cmd.soul.back_default", "已切回 SOUL.md"},
    {"cmd.soul.switched", "已切换魂: {0}(本会话即时生效,下一轮请求换新系统提示)"},
    {"cmd.soul.saved", "魂已写进 SOUL.md,本会话和下次启动都生效。"},
    {"cmd.soul.cleared", "SOUL.md 已还原默认,魂已清空。"},
    {"cmd.soul.write_failed", "写 SOUL.md 失败: {0}"},
    {"cmd.soul.default_config_failed", "SOUL.md 已写好,但没能把配置切回 default: {0}"},
    {"cmd.soul.switch_hint", "提示:历史里的旧风格回答可能带偏几轮,/clear 立净。"},
    {"cmd.soul.write_prompt", "写进配置? [y/N]: "},
    {"cmd.prompt.info",
     "当前的法(系统提示词人格段):\n"
     "  来源: {0}\n"
     "  字数: {1}\n"
     "用法:/prompt reset 把 system_prompt.md 还原成内置默认(旧文件留 .bak)。"},
    {"cmd.prompt.usage", "用法:/prompt 看当前法的来源;/prompt reset 还原 system_prompt.md。"},
    {"cmd.prompt.confirm", "确定还原? [y/N]: "},
    {"cmd.prompt.cancelled", "取消还原。"},
    {"cmd.prompt.no_home", "找不到用户主目录,没法还原。"},
    {"cmd.prompt.reset_failed", "还原失败: {0}"},
    {"cmd.prompt.reset_done", "已把 {0} 还原成内置默认"},
    {"cmd.prompt.old_file", ",旧文件在 {0}"},
    {"cmd.prompt.reset_tail", "本会话的法不变,下次启动按新文件生效。"},
    {"cmd.prompt.modules_header", "提示词模块({0};用户改过 {1}/{2} 个,改完开新会话生效):"},
    {"cmd.prompt.module_user_modified", "用户文件·已改"},
    {"cmd.prompt.module_user_same", "用户文件·同内置"},
    {"cmd.prompt.module_builtin", "内置"},
    {"law.builtin", "内置默认(core 模块拼装;~/.lubancode/prompts/core/ 可运行时覆盖)"},
    {"law.cli_arg", "CLI 参数 --system-prompt({0})"},
    {"law.config_file", "配置指定的人格文件({0})"},
    {"law.file", "文件 {0}"},
    {"resetprompt.no_home", "找不到用户主目录(Windows 下是 %USERPROFILE%),没法还原 system_prompt.md"},

    // ---- 会话存档:/sessions //resume //export //title //clear ----
    {"session.no_home", "找不到用户主目录,会话存档不可用。"},
    {"cmd.sessions.usage", "用法:/sessions(本目录)或 /sessions all(全部目录)"},
    {"cmd.sessions.none_all", "还没有会话存档({0} 下没有 .jsonl)。"},
    {"cmd.sessions.none_here", "本目录还没有会话存档(/sessions all 看全部目录)。"},
    {"cmd.sessions.header", "最近 {0} 场会话({1};时间倒序,/resume 编号或id 续聊):"},
    {"cmd.sessions.scope_all", "全部目录"},
    {"cmd.sessions.scope_here", "本目录,/sessions all 看全部"},
    {"cmd.sessions.unknown_time", "(开始时间未知)"},
    {"cmd.sessions.entry", "      {0} · {1} 条 · {2}"},
    {"cmd.sessions.no_text", "(没有用户文本)"},
    {"cmd.sessions.dir_line", "      目录: {0}"},
    {"cmd.sessions.dir_unknown", "(未知)"},
    {"cmd.sessions.archived_none", "还没有归档的会话(/archive 归档,归了的场子不进默认列表)。"},
    {"cmd.sessions.archived_header", "已归档 {0} 场(只读列表;想续聊先 lubancode unarchive <id>):"},
    {"cmd.sessions.archived_hint", "这些场子在 ~/.lubancode/sessions/archive/ 下,字节原样保留。"},
    {"cmd.resume.usage", "用法:/resume(全屏选择器) | /resume 编号 | /resume id"},
    {"cmd.resume.cancelled", "已取消恢复。"},
    {"cmd.resume.none", "本目录还没有会话存档,没什么可恢复(/sessions all 看全部目录)。"},
    {"cmd.resume.out_of_range", "编号 {0} 超出范围(本目录现有 {1} 场,/sessions 看列表)。"},
    {"cmd.resume.read_failed", "读不到存档 {0}。"},
    {"cmd.resume.bad_meta", "存档 {0} 首行不是合法 meta,认不得这个格式。"},
    {"cmd.resume.takeover_failed", "[会话存档] 接管 {0} 失败,恢复的历史只在内存里,本场不再落盘。"},
    {"cmd.resume.restored_compact", "已恢复 {0},有效 {1} 条(全量 {2} 条,经 {3} 次压缩)"},
    {"cmd.resume.restored", "已恢复 {0},{1} 条消息"},
    {"cmd.resume.repaired", "(补了 {0} 条缺失的工具结果)"},
    {"cmd.resume.skipped", "(跳过 {0} 行解析不动的存档)"},
    {"cmd.resume.queue_restored", "(排队消息 {0} 条也回来了,收尾后自动送出)"},
    {"cmd.resume.estimate", "上下文占用(按字符粗估): ~{0} tokens,首轮请求后以真实用量为准。"},
    {"cmd.resume.worktree_gone", "会话原先住的 worktree 已不在:{0}。回落到当前目录继续,与房的绑定解除。"},
    {"cmd.resume.worktree_refused", "拒绝进 {0}:验明正身没过({1})。留在当前目录;可检查该路径的 .git 指针后再试。"},
    {"cmd.resume.worktree_back", "已搬回会话原来的 worktree:{0}"},
    {"cmd.resume.model_mismatch", "[提醒] 存档时用的 model 是 {0},当前是 {1},继续聊没问题,风格可能有差。"},
    {"cmd.resume.wire_mismatch", "[提醒] 存档时用的 wire 是 {0},当前是 {1}。"},
    {"cmd.resume.history.header", "恢复历史 · {0}"},
    {"cmd.resume.history.end", "── 历史到此,可接着聊 ──"},
    // ---- 归档与永久删除(会话管理器单第四、五步) ----
    {"cmd.session.archive.usage", "用法:lubancode archive <id|标题> · lubancode unarchive <id>"},
    {"cmd.session.delete.usage",
     "用法:lubancode delete <id|标题> [--force]。永久删除,不可恢复;--force 跳过确认,只给脚本。"},
    {"cmd.session.ref_not_found", "找不到会话 {0}(按 id 或标题解;先 /sessions 看一眼)。"},
    {"cmd.session.ref_ambiguous", "引用 {0} 命中多场,请用完整 id 点明: {1}"},
    {"cmd.session.archive.done", "已归档 {0}(搬进 sessions/archive/,字节原样)。"},
    {"cmd.session.archive.failed", "归档 {0} 失败:文件没动,原账可用。"},
    {"cmd.session.unarchive.done", "已取消归档 {0}(搬回 sessions/ 根,可 /resume 续聊)。"},
    {"cmd.session.unarchive.failed", "取消归档 {0} 失败:文件没动,原账可用。"},
    {"cmd.session.delete.confirm_header", "永久删除确认"},
    {"cmd.session.delete.confirm_title", "  标题: {0}"},
    {"cmd.session.delete.confirm_id", "  id: {0}"},
    {"cmd.session.delete.confirm_cwd", "  目录: {0}"},
    {"cmd.session.delete.confirm_prompt", "这是永久删除,不可恢复。确认请输 y,缺省取消: "},
    {"cmd.session.delete.cancelled", "已取消,什么都没删。"},
    {"cmd.session.delete.done", "已永久删除 {0}(只删这一场;artifact blob 按内容寻址,别的会话还可能引用)。"},
    {"cmd.session.delete.failed", "删除 {0} 失败:文件没动。"},
    {"cmd.archive.not_active", "当前会话还没落盘(没建过档),没什么可归档。"},
    {"cmd.delete.not_active", "当前会话还没落盘(没建过档),没什么可删。"},
    {"cmd.archive.exiting", "已归档当前会话,退出。"},
    {"cmd.delete.exiting", "已删除当前会话,退出。"},
    {"cmd.archive.usage", "用法:/archive(不带参数;归档的是当前会话,别的场子用 lubancode archive <id>)"},
    {"cmd.archive.busy", "还有后台子代理在跑,先等它们收尾(或 /agents 面板停掉)再归档。"},
    {"cmd.delete.usage", "用法:/delete(不带参数;删的是当前会话,别的场子用 lubancode delete <id>)"},
    {"cmd.delete.busy", "还有后台子代理在跑,先等它们收尾(或 /agents 面板停掉)再删。"},
    {"cmd.resume.history.user", "你"},
    {"cmd.resume.history.assistant", "助手"},
    {"cmd.resume.history.image", "[图片] {0} ({1}x{2})"},
    {"cmd.resume.history.compact", "── 此处发生过一次上下文压缩 ──"},
    {"cmd.resume.history.tool_missing", "恢复时找不到工具结果"},
    {"cmd.resume.history.tool_error", "工具执行出错"},
    {"cmd.resume.history.tool_done", "工具执行完成"},
    {"cmd.resume.history.tool_more", " · 另有 {0} 行"},
    {"cmd.export.empty", "当前会话还没有内容,没什么可导出。"},
    {"cmd.export.need_path", "找不到用户主目录,请显式给个路径:/export 路径"},
    {"cmd.export.write_failed", "写不进 {0}。"},
    {"cmd.export.done", "已导出 Markdown: {0}"},
    {"cmd.title.none", "本场还没设标题(/title 标题 起一个)。"},
    {"cmd.title.current", "当前标题: {0}"},
    {"cmd.title.set", "标题已设为: {0}"},
    {"cmd.title.set_pending", "标题已设为: {0}(首条消息落盘后写入存档)"},
    {"cmd.title.write_failed", "[会话存档] 标题写入失败,只在本次会话内存里生效。"},
    {"cmd.peers.start_failed", "[跨会话] 传话未启用: {0}"},
    {"cmd.peers.off", "跨会话传话在本场未启用(只有交互会话才有)。"},
    {"cmd.peers.empty", "当前没有其它可见的会话。"},
    {"cmd.peers.hint", "↑/↓ 选择 · Enter 详情 · Esc 收起"},
    {"cmd.peers.status.idle", "空闲"},
    {"cmd.peers.status.busy", "忙"},
    {"cmd.peers.status.waiting", "等确认"},
    {"cmd.peers.status.closing", "退出中"},
    {"cmd.peers.held_notice", "[跨会话] {0}({1})递来消息: {2}"},
    {"cmd.peers.held_prompt", "交给模型处理? [y/N]: "},
    {"cmd.peers.held_dropped", "已忽略,未交给模型。"},
    {"cmd.peers.incoming_notice", "[来自 {0}({1}) 的消息]"},
    {"cmd.send.usage", "用法:/send <名字或短id> <话>"},
    {"cmd.send.unknown_target", "找不到会话 {0};/peers 可查当前可见的会话。"},
    {"cmd.send.result", "已向 {0}({1})递话: {2}"},
    {"cmd.send.label.delivered", "已送达"},
    {"cmd.send.label.held", "对方扣住了,等它的用户点头"},
    {"cmd.send.label.refused", "对方回绝"},
    {"cmd.send.label.expired", "对方限速或队列已满,没有收下"},
    {"cmd.send.label.unavailable", "对方不在(已退出或不可达)"},
    {"cmd.peerperm.usage", "用法:/peerperm auto|accept|hold|refuse"},
    {"cmd.peerperm.current", "当前跨会话来信档: {0}(auto=按两边模式与目录距离自动定)"},
    {"cmd.peerperm.set", "跨会话来信档已设为 {0}。"},
    {"cmd.clear.done", "已清空对话历史。"},
    {"cmd.init.created", "已生成 {0}，本会话已载入。"},
    {"cmd.init.exists", "已有项目指令 {0}，没有覆盖；本会话已重新载入。"},
    {"cmd.init.failed", "生成 AGENTS.md 失败：{0}（{1}）"},
    {"session.create_failed", "[会话存档] 在 {0} 建档失败,本场对话不落盘(不影响继续聊)。"},
    {"session.append_failed", "[会话存档] 追加写入失败,后续不再落盘(不影响继续聊)。"},
    {"session.compact_event_failed", "[会话存档] 存档事件写盘失败,/resume 将回放到压缩前状态。"},

    // ---- 会话选择器(SessionPicker,/resume 裸敲的全屏台账) ----
    {"picker.title", "恢复哪一场会话?"},
    {"picker.search.placeholder", "输入即搜(标题/首句/id/目录)…"},
    {"picker.filter.label", "Filter"},
    {"picker.filter.cwd", "Cwd"},
    {"picker.filter.all", "All"},
    {"picker.sort.label", "Sort"},
    {"picker.sort.updated", "Updated"},
    {"picker.sort.created", "Created"},
    {"picker.empty.none", "本目录还没有会话存档(Tab 切 All 看全部目录)。"},
    {"picker.empty.search", "没有命中: {0}"},
    {"picker.no_text", "(没有用户文本)"},
    {"picker.damaged", "damaged"},
    {"picker.unknown_dir", "(目录未知)"},
    {"picker.unknown_model", "(模型未知)"},
    {"picker.unknown_time", "(时间未知)"},
    {"picker.expand.title", "标题:"},
    {"picker.expand.cwd", "目录:"},
    {"picker.expand.id", "id:"},
    {"picker.expand.model", "模型:"},
    {"picker.expand.messages", "{0} 条消息"},
    {"picker.expand.created", "创建:"},
    {"picker.expand.updated", "更新:"},
    {"picker.transcript.title", "转录 · {0}"},
    {"picker.transcript.empty", "(这场会话还没有可显示的转录)"},
    {"picker.transcript.footer",
     "esc/ctrl+t 收起回列表 · enter resume · 上下不动选中,看完回原行"},
    {"picker.footer",
     "enter 接回 · esc 退出 · tab 换焦点 · </> 改选项 · ↑↓ 浏览 · pgup/pgdn 翻页 · "
     "home/end 跳首尾 · ctrl+o 详细/紧凑 · ctrl+t 看转录 · ctrl+e 看全文"},
    {"picker.status", "{0} / {1} · {2}%"},
    {"picker.status.empty", "0 / 0 · 0%"},
    {"picker.ago.now", "just now"},
    {"picker.ago.minutes", "{0}m ago"},
    {"picker.ago.hours", "{0}h ago"},
    {"picker.ago.days", "{0}d ago"},

    // ---- UI-D 画面提示 ----
    {"ui.expanded", "—— 详细模式(Ctrl+O 切回紧凑)——"},
    {"ui.compact", "—— 紧凑模式 ——"},
    {"ui.no_items", "(本会话还没有工具条目)"},
    {"ui.focus", "[焦点 {0}/{1}] Tab 往旧 · Shift+Tab 往新 · Ctrl+E 查看全文"},
    {"ui.back", "—— 返回会话 ——"},
    {"ui.focus_view", "—— 聚焦查看 条目 {0}/{1},Ctrl+E 或 ESC 返回 ——"},

    // ---- diff 预览 ----
    {"diff.path", "路径: {0}"},
    {"diff.plain", "diff:"},
    {"diff.not_located", "diff(old_string 在文件里没找到,只对比新旧两段):"},
    {"diff.replace_all", "diff(replace_all,替换 {0} 处):"},
    {"diff.overwrite", "diff(覆盖已有文件):"},
    {"diff.new_file", "diff(新文件,全部新增):"},
};

// ---------------------------------------------------------------------------
// en:P0 全量;P1 暂缺(TODO:P1 键渐进补齐,缺键回退 zh-CN,见 README)。
// ---------------------------------------------------------------------------
const Entry kEn[] = {
    {"language.name", "English (en)"},

    // ---- help (--help) ----
    {"help.title", "lubancode {0} - C++ AI coding CLI"},
    {"help.usage",
     "Usage:\n"
     "  lubancode [options]\n"
     "  lubancode \"question\"      one-shot Q&A; tools are used when helpful\n"
     "  lubancode                  with no arguments, enters the interactive loop; on first run with\n"
     "                              missing config the setup wizard runs once, then the session starts\n"
     "                              without a restart. exit/quit or EOF (Ctrl+Z / pipe drained) quits;\n"
     "                              an empty line just re-prompts\n"
     "  lubancode archive <id>    archive a session (moved into sessions/archive/, bytes untouched;\n"
     "  lubancode unarchive <id>  unarchive brings it back; archived ones stay out of the default list)\n"
     "  lubancode delete <id>     permanently delete a session after an interactive confirmation;\n"
     "                            --force skips the confirmation (scripts only), unrecoverable\n"},
    {"help.options",
     "Options:\n"
     "  --version              print the version\n"
     "  --check-update         check the latest GitHub Release, print the result, and exit\n"
     "  --help                 print this help\n"
     "  --yes                  auto-approve all tool calls that need confirmation (e.g. run_command)\n"
     "  --continue             on interactive startup, resume the most recent session archive of this\n"
     "                         directory (like an opening /resume); starts fresh if none exists\n"
     "  --config               print the effective configuration (api_key masked) and the source tier\n"
     "                         of each field; useful for troubleshooting\n"
     "  --system-prompt <file> replace the persona segment of the default system prompt with this file\n"
     "                         (.md/.txt, UTF-8); required runtime context (cwd, tool calls) is still\n"
     "                         appended. Overrides the config field system_prompt_file and\n"
     "                         ~/.lubancode/system_prompt.md\n"
     "  --reset-system-prompt  restore ~/.lubancode/system_prompt.md to the built-in default; the old\n"
     "                         file is kept as system_prompt.md.bak (same as /prompt reset, but\n"
     "                         non-interactive: prints the result and exits)\n"},
    {"help.scaffold",
     "Prompt scaffold (auto-generated on first launch, re-created if missing, never overwritten):\n"
     "  ~/.lubancode/system_prompt.md  the \"law\" - persona segment of the system prompt; edit it to\n"
     "                                 customize behavior; if blank after stripping the header comment,\n"
     "                                 the built-in default is used\n"
     "  ~/.lubancode/SOUL.md           the \"soul\" - style overlay appended at the very end of the\n"
     "                                 system prompt; empty = no effect\n"
     "  ~/.lubancode/souls/*.md        alternative souls (wenyan.md sample included); switch with\n"
     "                                 /soul <name> in interactive mode\n"},
    {"help.slash",
     "In interactive mode, a line starting with / runs a command instead of being sent to the model:\n"
     "  /help           list all commands\n"
     "  /model          fetch the model list and switch by number (default: first)\n"
     "  /model <name>   switch directly to a model name without fetching the list\n"
     "  /model roles    show the normal/cheap/lao task routes and their config sources\n"
     "  /provider       list configured providers; /provider add|switch|edit|remove|set|refresh manages endpoints (refresh updates the catalog)\n"
     "  /config         print the effective configuration plus the model in use this session\n"
     "  /update         check the latest GitHub Release; installs also sync official skills\n"
     "  /init           create AGENTS.md at the project root and load it for main/sub-agents now\n"
     "  /language       list available UI languages and switch (built-in zh-CN/en, extendable via\n"
     "                  languages/*.json)\n"
     "  /worktree       create, list, or leave isolated trees; /worktree new [name] | list | exit keep|remove\n"
     "  /clear          clear the conversation history\n"
     "  /context        show context usage breakdown (system prompt / tools / history + bars)\n"
     "  /context 512k   temporarily change the window size (256k/512k/1m or a plain number)\n"
     "  /compact [note] manually compact the history; the note tells what to keep extra\n"
     "  /think          show the reasoning effort (/effort is an alias)\n"
     "  /think <level>  set the reasoning effort; levels are provider-defined (anthropic maps\n"
     "                  none/low/medium/high/xhigh/max; responses passes it through)\n"
     "  /skills         list discovered skills (official + home-level + project-level)\n"
     "  /skill          manage skills; run it bare for URL/local install, update, and remove examples\n"
     "  /mcp            list mounted MCP servers and their tools\n"
     "  /lsp            list LSP server status per language\n"
     "  /todos          show the current todo list (maintained by the todo_write tool)\n"
     "  /plugins        list mounted plugin tools (*.dll and *.lua under .lubancode/plugins)\n"
     "  /tools          list tool states: core / loaded / deferred (tool_search deferral kicks in\n"
     "                  when the tool count exceeds tool_search_threshold, default 20, 0 = never)\n"
     "  /memory         manage project memory, retrieval, background writes, forgetting and rebuilds\n"
     "  /sessions       list the 20 most recent session archives of this directory; /sessions all\n"
     "                  lists every directory\n"
     "  /resume         choose a local session with arrow keys and replay it; also accepts a number or id\n"
     "  /export [path]  export the current session as Markdown (default sessions/<id>.md)\n"
     "  /title [title]  show/set the session title, used by /sessions and /export\n"
     "  /soul           show the current soul; /soul <text> writes SOUL.md and takes effect now;\n"
     "                  /soul clear restores its default; an existing soul name still switches it\n"
     "  /prompt         show the source and length of the current system prompt persona;\n"
     "                  /prompt reset restores system_prompt.md\n"
     "  /background     list background command tasks (id/status/PID/command/log); /bg is an alias\n"
     "  /record         record a workflow into a skill; /record start <name> begins, stop drafts,\n"
     "                  run it bare for all subcommands\n"
     "  /peers          list other Lubancode sessions on this machine (name/status/cwd); arrow keys,\n"
     "                  Enter for details\n"
     "  /send           /send <name-or-id> <text>: pass a plain-text note to another session\n"
     "  /peerperm       /peerperm auto|accept|hold|refuse: set how incoming peer messages are received\n"
     "  /image <path>   attach a local image (or use @path in a message; png/jpg/jpeg/gif/webp, 5MB each)\n"
     "  Shift+Enter     insert a newline in the input box (Alt+Enter works too, but Windows Terminal\n"
     "                  binds it to fullscreen by default; Shift+Enter is safest); Enter sends the\n"
     "                  whole message; Enter on blank input does nothing\n"
     "  Paste content up to 1000 chars stays visible; larger pastes collapse and expand on submit\n"
     "  ESC             during streaming: interrupt this turn (partial output is kept); when idle:\n"
     "                  clear the input; at a [y/a/N] confirm prompt: deny; in focus view (Ctrl+E):\n"
     "                  return to the session\n"
     "  Down/Up after / enter the candidate menu (Down selects the first entry, Down/Up cycle);\n"
     "                  Enter runs the selected command (typed argument tail is kept); typing/\n"
     "                  Backspace/ESC returns to normal editing\n"
     "  Ctrl+O          toggle compact/detailed globally (detailed = full argument JSON + full\n"
     "                  output/diff); press again to switch back\n"
     "  Ctrl+E          focus-view the focused item (or the latest one); Ctrl+E or ESC returns\n"
     "  Tab             with input: complete/cycle slash commands; with empty input: enter focus\n"
     "                  mode on the latest tool item; in focus mode Tab moves older, Shift+Tab newer,\n"
     "                  ESC/Enter exits back to editing\n"
     "  Shift+Tab       cycle the confirmation mode (confirm/auto/yolo) - anytime, matching the\n"
     "                  status line; the only exception is focus mode (there it moves focus newer).\n"
     "                  auto: file edits and safe commands run freely, dangerous commands and\n"
     "                  external tools still ask; yolo: everything runs freely\n"
     "  typing+Enter during streaming  does not interrupt the stream; the line joins the queue shown\n"
     "                  above the input box and is delivered, in order, into the same conversation\n"
     "                  right after the current tool call finishes (the queue title states the\n"
     "                  timing). Esc interrupts and sends immediately; Shift+Left recalls the latest\n"
     "                  message for editing (Enter replaces in place, Esc restores, Del twice\n"
     "                  deletes; the Up key still works as an alias)\n"
     "  /exit           quit (bare exit/quit work too)\n"},
    {"help.config",
     "Configuration priority (high to low, decided per field, not as a whole):\n"
     "  1) LUBANCODE_ dedicated environment variables\n"
     "       LUBANCODE_WIRE          protocol: anthropic / responses / chat_completions\n"
     "       LUBANCODE_BASE_URL      API address\n"
     "       LUBANCODE_API_KEY       auth token\n"
     "       LUBANCODE_MODEL         model name\n"
     "       LUBANCODE_MAX_CONTEXT   history trim threshold (characters, the old hard safety net)\n"
     "       LUBANCODE_THEME         terminal theme, dark / light / plain\n"
     "       LUBANCODE_LANG          UI language, zh-CN / en / a code from languages/; empty = system\n"
     "       LUBANCODE_SYSTEM_PROMPT_FILE  persona file path, same as --system-prompt (CLI wins)\n"
     "       LUBANCODE_CONTEXT_WINDOW      context window tokens, 256k/512k/1m or a plain number\n"
     "       LUBANCODE_COMPACT_MODEL       model used for compaction; empty = session model\n"
     "       LUBANCODE_THINK               reasoning effort, provider-defined; empty = not sent\n"
     "       LUBANCODE_SOUL                soul name, default = SOUL.md, off = disabled, other names\n"
     "                                     = souls/<name>.md\n"
     "  2) config file (first found wins, search order: cwd .lubancode/config.json -> home\n"
     "     .lubancode/config.json -> cwd legacy .lubancode.json -> home legacy .lubancode.json;\n"
     "     legacy locations are migrated automatically). Fields: wire / base_url / api_key / model /\n"
     "     max_context_chars / theme / language / system_prompt_file / context_window / compact_model /\n"
     "     think, all optional. providers is also a whole config-file section: each entry has name / base_url / wire /\n"
     "     key_env / model / context_window; key_env stores an environment variable name, never a key. Project providers\n"
     "     replace the global list. Plus hooks / mcpServers / search sections (config-file only):\n"
     "       \"mcpServers\": {\"name\": {\"command\": \"...\", \"args\": [...], \"env\": {...}}}\n"
     "       after handshake, tools mount as mcp__name__tool; see /mcp for status\n"
     "       \"search\": {\"provider\": \"tavily|brave|serper\", \"api_key\": \"...\"}\n"
     "       required for the web_search tool; web_fetch needs no config and is always available\n"
     "     And an lsp section (config-file only; unset = disabled = no lsp tool):\n"
     "       \"lsp\": {\"cpp\": {\"command\": \"clangd\", \"args\": [...], \"extensions\": [\".cpp\", \".hpp\"],\n"
     "                \"idle_minutes\": 10}}\n"
     "       registers the lsp tool (definition/references/symbols/diagnostics), lazy-started and\n"
     "       auto-stopped when idle; see /lsp for status\n"
     "  3) generic environment variables (backward compatible; they collide with tools like Claude\n"
     "     Code, prefer the LUBANCODE_* tier):\n"
     "       wire=anthropic reads ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN / ANTHROPIC_MODEL\n"
     "       wire=responses reads OPENAI_BASE_URL / OPENAI_API_KEY / OPENAI_MODEL\n"
     "  4) built-in defaults: wire=anthropic, max_context_chars={0}, theme={1}, context_window={2}.\n"
     "     base_url/api_key/model/system_prompt_file/compact_model/think have no built-in defaults -\n"
     "     if nothing is configured, interactive mode runs the setup wizard; one-shot/pipe mode fails\n"
     "     with a readable error listing the three configuration paths. Use --config to inspect.\n"},

    // ---- /help (interactive) ----
    {"slash_help.body",
     "Available commands:\n"
     "  /help           list all commands\n"
     "  /model          fetch the model list and switch by number (default: first)\n"
     "  /model <name>   switch directly to a model name\n"
     "  /model roles    show the normal/cheap/lao task routes and their config sources\n"
     "  /provider       list configured providers; /provider add|switch|edit|remove|set|refresh manages endpoints (refresh updates the catalog)\n"
     "  /config         print the effective configuration (api_key masked) plus the session model\n"
     "  /update         check the latest GitHub Release; installs also sync official skills\n"
     "  /init           create AGENTS.md at the project root and load it now\n"
     "  /language       list available UI languages and switch; /language <code> switches directly\n"
     "  /worktree       create, list, or leave isolated trees; /worktree new [name] | list | exit keep|remove\n"
     "  /clear          clear the conversation history\n"
     "  /context        show context usage; /context 256k|512k|1m changes the window for this session\n"
     "  /compact        compact the history; /compact <note> tells what to keep extra\n"
     "  /think          show reasoning effort; /think <level> sets it (provider-defined; /effort alias)\n"
     "  /skills         list discovered skills (official + home-level + project-level)\n"
     "  /skill          manage skills; run it bare for URL/local install, update, and remove examples\n"
     "  /mcp            list mounted MCP servers and their tools\n"
     "  /lsp            list LSP server status per language\n"
     "  /todos          show the current todo list\n"
     "  /plugins        list mounted plugin tools (DLL + lua) and load warnings\n"
     "  /hooks          hooks ledger: source/command/trust/disabled/last result; trust|untrust|disable|enable <#id>, runs [N]\n"
     "  /tools          list tool states: core / loaded / deferred (tool_search)\n"
     "  /memory         manage project memory; /memory on|off|use|learn|list|remember|forget|rebuild\n"
     "  /sessions       list the 20 most recent session archives here; /sessions all for every dir\n"
     "  /resume         choose and replay a local session; also accepts a number or id; new messages append there\n"
     "  /export [path]  export this session as Markdown (default sessions/<id>.md)\n"
     "  /title [title]  show/set the session title, used by /sessions and /export\n"
     "  /soul           show the current soul; /soul <text> writes SOUL.md and takes effect now;\n"
     "                  /soul clear restores default; an existing soul name still switches it\n"
     "  /prompt         show the persona source and length; /prompt reset restores system_prompt.md\n"
     "  /background     list background command tasks (id/status/PID/command/log); /bg is an alias\n"
     "  /record         record a workflow into a skill; /record start <name> begins, stop drafts,\n"
     "                  run it bare for all subcommands\n"
     "  /peers          list other Lubancode sessions on this machine (name/status/cwd); arrow keys,\n"
     "                  Enter for details\n"
     "  /send           /send <name-or-id> <text>: pass a plain-text note to another session\n"
     "  /peerperm       /peerperm auto|accept|hold|refuse: set how incoming peer messages are received\n"
     "  /exit           quit (bare exit/quit work too)\n"
     "Multi-line input: Shift+Enter inserts a newline (Alt+Enter too, but Windows Terminal may\n"
     "swallow it; Shift+Enter is recommended); Enter sends the whole message; on multi-line input\n"
     "a leading / is treated as text, not a command.\n"
     "Queued messages: typing+Enter during streaming queues the line above the input box and it\n"
     "is delivered into the same turn after the current tool call (the queue title states the\n"
     "timing); Esc interrupts and sends immediately; Shift+Left recalls it for editing (Enter\n"
     "replaces in place, Esc restores, Del twice deletes).\n"
     "Paste content up to 1000 chars stays visible; larger pastes collapse and expand on submit.\n"
     "Candidate menu: with a leading /, Down enters the menu (Down/Up cycle, Enter runs the\n"
     "selection, typed argument tail kept; typing/Backspace/ESC returns to editing); Tab completes.\n"
     "Item view: Ctrl+O toggles compact/detailed globally; Shift+Tab always cycles the confirmation\n"
     "mode (confirm/auto/yolo, shown live in the status line; auto passes file edits and safe\n"
     "commands, asks for dangerous commands and external tools); with empty input Tab enters focus\n"
     "mode on the latest item (Tab older, Shift+Tab newer, ESC/Enter exits); Ctrl+E focus-views the\n"
     "focused item (or the latest), Ctrl+E or ESC returns; these keys are inactive while streaming.\n"},

    // ---- banner / status line / input layer ----
    {"banner.hint", "type a question and press Enter; exit to quit; /help for commands"},
    {"status.mode.confirm", "confirm"},
    {"status.shift_tab_hint", "(shift+tab to cycle)"},
    {"mode.confirm", "confirm"},
    {"ui.menu_more", "  … {0} commands total"},
    {"input.interrupted", "[interrupted]"},
    {"input.queued", "[queued] "},
    {"input.queue_more", "{0} more queued"},
    // ---- 0.28.x queued messages (delivered at tool boundary, Shift+Left to edit) ----
    {"queue.key_hint", "Shift+Left"},
    {"queue.key_hint_fallback", "Shift+Left / Ctrl+Left"},
    {"queue.title.boundary", "Queued: sent after the next tool call · Esc interrupts and sends now · {0} to edit"},
    {"queue.title.end_of_turn", "Queued: sent when this turn ends · {0} to edit"},
    {"queue.title.immediate", "Interrupting and sending now..."},
    {"queue.title.editing", "Editing a queued message · Enter replaces in place · Esc restores · Del twice deletes"},
    {"queue.mark.editing", "[editing] "},
    {"queue.mark.target", "[#{0}] "},
    {"queue.mark.target_gone", "[target finished] "},
    {"queue.mark.failed", "[send failed] "},
    {"queue.commit_conflict", "This message was already delivered or changed; your edit was not saved. The text stays as a new message."},
    {"queue.edit_blocks_panel", "Finish or cancel the queued-message edit (Enter/Esc) before using the panel."},
    {"queue.delete_armed", "Press Del again to delete this queued message (Esc/timeout cancels)"},
    {"queue.disposal_head", "{0} queued message(s) never sent; discarded on exit/clear:"},
    {"queue.disposal_preview", "  first [{0}] {1}"},
    {"queue.archive_head", "{0} queued message(s) not yet sent; saved with the session archive, /resume will bring them back:"},
    {"queue.autosend_returned", "Your queued message was not delivered; returned to queue (no auto-retry): {0}"},
    {"input.pasted_content", "[Pasted Content {0} chars]"},
    {"input.ctrlc_exit", "[exited]"},
    {"stream.hint", "type + Enter to queue next · Esc to interrupt"},
    {"stream.hint.plain", "type + Enter to queue next · Esc to interrupt"},
    {"footer.repaint_unsupported",
     "[This terminal cannot repaint while streaming; no input box during streaming. Typing still queues, Esc still interrupts]"},
    {"spinner.thinking", "Working"},
    {"spinner.stopping", "Stopping..."},
    {"ask_user.other", "Other (type your own answer)"},
    {"ask_user.discuss", "Chat about this"},
    {"ask_user.panel_title", "Your input is needed"},
    {"ask_user.select_prompt", "Choose a number (Esc to cancel): "},
    {"ask_user.multi_prompt", "Choose numbers separated by commas (Esc to cancel): "},
    {"ask_user.menu_hint", "Enter to select · ↑/↓ to navigate · type for Other · Esc to cancel"},
    {"ask_user.menu_multi_hint", "Space to toggle · Enter to submit · ↑/↓ to navigate · Esc to cancel"},
    {"ask_user.menu_select_one", "Select at least one option"},
    {"ask_user.menu_edit_hint", "Type your answer · Backspace deletes · Enter submits · Esc cancels"},
    {"ask_user.custom_prompt", "Enter your answer: "},
    {"ask_user.cancelled", "The user cancelled the question"},
    {"ask_user.declined", "User declined to answer questions"},
    {"ask_user.discuss_prompt", "What would you like to add? "},
    {"ask_user.discuss_empty", "The discussion message cannot be empty."},
    {"ask_user.discussion_recorded", "Added:"},
    {"ask_user.invalid", "Invalid choice; try again."},
    {"ask_user.custom_empty", "The answer cannot be empty."},
    {"ask_user.recorded", "Selected:"},

    // ---- subagent status board (#52, #three: tool calls/tokens/duration) ----
    {"agent_status.state_running", "Running"},
    {"agent_status.state_stopping", "stopping (waiting for current operation)"},
    {"agent_status.state_done", "Done"},
    {"agent_status.state_failed", "Failed"},
    {"agent_status.state_failed_reason", "failed · {0}"},
    {"agent_status.state_stopped_reason", "stopped · {0}"},
    {"agent_status.state_exhausted", "exhausted · {0}/{1} steps"},
    {"agent_status.budget_suffix", " · {0}/{1} steps"},
    {"agent_status.reason_api_error", "API error"},
    {"agent_status.reason_step_limit", "step budget exhausted"},
    {"agent_status.reason_max_context", "context full"},
    {"agent_status.reason_no_final_text", "no final text"},
    {"agent_status.reason_tool_error", "tool error"},
    {"agent_status.reason_user_stop", "stopped by user"},
    {"agent_status.reason_wall_clock", "wall-clock timeout"},
    {"agent_status.reason_protocol_error", "protocol error"},
    {"agent_status.reason_unknown", "unspecified"},
    {"agent_status.summary", "{0} ({1} tool uses · {2} tokens · {3})"},
    {"agent_status.tokens_not_reported", "tokens not reported"},
    // Live activity signal (spec "subagent activity invisible"): the dock/view
    // line swaps this single phrase per stage; counts only, never content.
    {"agent_activity.thinking", "thinking · {0} chars"},
    {"agent_activity.text", "text {0} chars"},
    {"agent_activity.tool", "tool {0} · {1}s"},
    {"agent_activity.waiting", "awaiting first byte · {0}s"},
    {"agent_activity.first_byte", "first byte {0}ms"},
    // Wall-clock fallback (spec item 3): the timeout reason is spelled out and
    // checkpoints are still brought back.
    {"agent_outcome.wall_clock",
     "Subagent wall-clock timeout (>= {0}s, subagent.wall_clock_timeout_secs): forcibly wrapped up; checkpoints gathered "
     "before the timeout follow."},
    {"agent_outcome.wall_clock_force",
     "Subagent hit the wall-clock limit and did not respond to the stop signal within the grace period; the ledger was "
     "forcefully closed (limit {0}s, subagent.wall_clock_timeout_secs)."},
    {"agent_outcome.wall_clock_late",
     "The task thread returned only after the forceful close; the ledger keeps the forced record."},
    // Structured failure page for output budget exhaustion (spec root cause 4):
    // shared by main and subagents, zh/en paired.
    {"agent_outcome.output_budget.head",
     "Output budget exhausted (stop_reason=max_tokens): still no text after {0} automatic continuation(s). Thinking and "
     "tool results received so far are preserved in the session."},
    {"agent_outcome.output_budget.limit", "Output budget this session: {0} tokens (source in /config)"},
    {"agent_outcome.output_budget.limit_unset",
     "Output budget this session: unset — the field was omitted; the wall is the server default"},
    {"agent_outcome.output_budget.continuations", "Automatic continuations used: {0} (limit agent.length_continuations)"},
    {"agent_outcome.output_budget.usage_reported", "usage: reported by the server; see the turn stats"},
    {"agent_outcome.output_budget.usage_not_reported",
     "usage: not reported — the server said length but returned no usage, so token counts are unknown and not treated as "
     "0 (on chat endpoints the usual cause is stream_usage off; /doctor cache usage can probe)"},
    {"agent_outcome.output_budget.escapes",
     "Ways out: 1) type \"continue\" to let it wrap up; 2) raise the output budget (config agent.max_output_tokens); 3) "
     "lower or disable thinking (/think none); 4) split the task or switch to a non-reasoning model"},
    {"error.length_empty_reasoning_bytes", "{0} bytes of thinking received (a checkpoint of the tail is kept; see the "
                                          "session record)"},
    {"agent_status.expand_hint", "(ctrl+o to expand)"},
    {"agent_tool.title_missing",
     "Missing required parameter title: give the task a short semantic title (4-16 CJK chars or 2-6 English words, a "
     "noun phrase, not the first sentence of the prompt), then retry."},
    {"agent_tool.title_bad",
     "title must not contain line breaks or tabs and must fit 40 display columns; pick a short semantic title and "
     "retry (no path lists or full task text)."},
    {"agent_message.queued", "Queued for agent #{0}; delivered at its next safe tool boundary."},
    {"agent_message.finished",
     "Agent #{0} already finished and takes no follow-ups (not rerouted to main, no auto-revive; start a new task "
     "instead)."},
    {"agent_message.not_found",
     "No agent with task id #{0} (cleared or never existed); check the running-agent roster for valid ids."},
    {"agent_message.invalid", "message must be a non-empty string; relay the user's exact words first, deltas only."},
    {"agent_message.task_id_invalid", "task_id must be an integer from the running-agent roster."},
    {"agent_message.unavailable", "No agent runtime available in this session; message not delivered."},
    {"agent_panel.untitled", "Unnamed agent #{0} (legacy task)"},
    {"agent_panel.stream_hint", "↑/↓ select · Enter view · x stop/clear · Esc exits layer by layer"},
    {"agent_panel.source_foreground", "foreground"},
    {"agent_panel.source_background", "background"},
    {"agent_panel.hint", "↑/↓ select · Enter view · x stop/clear · Ctrl+X Ctrl+K stop all agents"},
    {"agent_panel.hint_armed", "Press Ctrl+K again to stop all · Esc/timeout cancels"},
    {"agent_panel.window_note", "{0} agents · {1} hidden above · {2} hidden below"},
    {"agent_panel.stop_all_notice", "Asked {0} running agent(s) to stop"},
    {"agent_panel.stop_notice", "Asked #{0} to stop, wrapping up (stopping...)"},
    {"agent_panel.stop_not_running", "#{0} is no longer running; no stop signal sent"},
    {"agent_panel.pending_note", "{0} queued message(s)"},
    {"agent_panel.detail_gone", "This task has been cleared."},
    {"agent_panel.view_header", "── viewing {0} · {1} · Esc back to main ──"},
    {"agent_panel.back_to_main", "Back to the main session."},
    {"agent_panel.completion_notice", "[background agent finished; results handed back to main]"},
    {"agent_panel.reflow_toast", "Agent {0} finished; result delivered to main (silent while viewing)"},
    {"agent_panel.denial_notice_title", "[background agent permission not pre-approved; request denied]"},
    {"keymap.override_warning", "Keymap override skipped one entry: {0}"},
    {"search.header", "history search"},
    {"search.scope.session", "this session"},
    {"search.scope.project", "this project"},
    {"search.scope.all", "all projects"},
    {"search.key.older", "older"},
    {"search.key.scope", "scope"},
    {"search.key.accept", "recall"},
    {"search.key.accept_submit", "recall & send"},
    {"search.key.cancel", "cancel"},
    {"search.query", "query"},
    {"search.no_match", "no matching prompt"},
    {"cmd.copy.usage", "Usage: /copy (raw Markdown) or /copy plain (plain text)"},
    {"cmd.copy.done", "Copied the last assistant reply ({0} bytes)"},
    {"cmd.copy.no_assistant", "No assistant reply to copy yet."},
    {"cmd.copy.unsupported", "No clipboard channel in this environment: {0}"},
    {"cmd.copy.failed", "Copy failed: {0}"},
    {"slash.desc.copy", "Copy the last completed assistant reply (/copy plain for plain text)"},
    {"stash.stashed", "Draft stashed (press again to restore; keyed to its target and directory)"},
    {"stash.restored", "Stashed draft restored."},
    {"stash.restore_refused", "Target or directory changed since stashing; return there first."},
    {"stash.empty", "Nothing to stash and nothing stashed."},
    {"stash.still_there", "A stashed draft remains (memory only, never written to disk, gone on exit)."},
    {"editor.no_temp", "No temp directory; editor not started."},
    {"editor.write_failed", "Could not write the temp file; editor not started."},
    {"editor.nonzero", "Editor exited with code {0}; original draft untouched."},
    {"editor.file_gone", "Editor left no temp file; original draft untouched."},
    {"editor.bad_utf8", "Editor wrote non-UTF-8 content; original draft untouched."},
    {"editor.done", "Draft read back from {0}."},
    {"mention.header", "@ mention file/directory"},
    {"mention.keys_hint", "↑/↓ select · Enter/Tab insert · Esc dismiss"},
    {"mention.no_match", "no matching file or directory"},
    {"mention.dir_icon", "▸"},
    {"mention.file_icon", "·"},
    {"mention.missing", "Mentioned {0} does not exist; this turn was NOT sent."},
    {"mention.outside_root", "Mentioned {0} escapes the project root; this turn was NOT sent."},
    {"mention.ledger_header", "[User-mentioned files/directories (verified, resolved from project root)]"},
    {"help.scene_header", "Keys for this scene (press ? again to dismiss; fixed keys protect the editor)"},
    {"help.scene_footer", "Rebind with /keymap set <action> <chord>; reset with /keymap reset all."},
    {"help.fixed_suffix", " (fixed)"},
    {"help.unbound_suffix", " (unbound; bind via /keymap set)"},
    {"hint.keys.help", "keys"},
    {"hint.keys.search_history", "history"},
    {"hint.keys.expand", "expand"},
    {"hint.keys.editor", "editor"},
    {"ui.turn_nav", "turn {0}/{1}"},
    {"ui.to_scrollback", "Transcript written to scrollback; use the terminal's own search."},
    {"ui.view_in_editor", "Transcript saved to a temp file for {0} (back to composer on exit)."},
    {"notify.state_busy", "working"},
    {"notify.state_idle", "awaiting input"},
    {"keymap.list_header", "Keymap (scope · chord · action):"},
    {"keymap.usage", "Usage: /keymap list · /keymap set <action> <chord> · /keymap reset <action>|all"},
    {"keymap.unknown_action", "Unknown action {0} (see /keymap for all names)."},
    {"keymap.bad_chord", "Cannot parse chord {0} (e.g. Ctrl+R, Alt+V, ?, Shift+Tab)."},
    {"keymap.bind_failed", "Rebind refused: {0}"},
    {"keymap.bound", "{0} bound to {1}."},
    {"keymap.save_failed", "Keymap not saved: {0}"},
    {"keymap.reset_all", "All bindings reset to defaults."},
    {"keymap.reset_one", "{0} reset to its default."},
    {"keymap.reset_failed", "Cannot reset {0} (fixed key or unknown action)."},
    {"keymap.fixed_suffix", " (fixed)"},
    {"keymap.unbound_suffix", " (unbound)"},
    {"slash.desc.keymap", "Show or rebind keys (/keymap set action chord; saved user-level)"},
    {"slash.desc.workflow", "Reusable workflow graphs: list/show/graph/validate/run/resume/cancel"},
    {"slash.desc.goal", "Durable goal: objective/status/edit/pause/resume/clear (multi-turn until verifiable end)"},
    {"slash.desc.loop", "session-scoped recurring loop: /loop [interval] [prompt] creates, list/pause/resume/stop manage, run dispatches now"},
    {"image.pasted", "Clipboard image ready ({0} KB; path inserted, attaches on send)"},
    {"image.paste_failed", "Paste failed: {0}"},
    {"clipboard.paste_text_failed", "Nothing pasted: {0}"},
    {"transcript.assistant_bg_title", "background reflow · analysis"},
    {"error.step_limit", "Step limit reached (max_steps_per_turn; formerly max_turns); {0} steps used this turn. Raise the limit or set 0 to remove it."},
    {"agent_panel.detail_prompt", "Task: "},
    {"agent_panel.detail_pending_head", "{0} queued intervention message(s) not yet delivered: "},
    {"agent_panel.detail_tools_head", "Tool calls ({0} total): "},
    {"agent_panel.detail_result_head", "Result/output: "},
    {"agent_panel.target_queued", "Queued for agent #{0}; delivered after its current tool call. Esc returns to main."},
    {"agent_panel.target_rejected", "Agent #{0} already finished; message NOT delivered (not rerouted to main). Esc or x to leave/clear."},
    {"agent_panel.main", "main session"},
    {"agent_panel.hint_short", "↑/↓ select · Enter view"},
    {"agent_panel.hint_focused", "Enter view · x stop/clear · Esc back · Ctrl+X Ctrl+K stop all agents"},
    {"agent_panel.hint_focused_short", "Enter view · x stop/clear · Esc back"},
    {"agent_panel.hint_idle_expanded", "Enter view · Esc collapse"},
    {"agent_panel.stream_hint_short", "↑/↓ select · Enter view"},
    {"agent_panel.stream_hint_focused", "Enter view · x stop/clear · Esc exits layer by layer · Ctrl+X Ctrl+K stop all agents"},
    {"agent_panel.stream_hint_focused_short", "Enter view · x stop/clear · Esc exits layer by layer"},
    {"agent_panel.stream_hint_idle_expanded", "Enter view · Esc collapse"},
    {"agent_panel.idle_summary", "{0} more idle agent(s) · Enter to expand"},
    {"agent_panel.event_steering", "Steering"},
    {"agent_panel.event_thinking", "Thinking"},
    {"agent_panel.event_failed", "Task ended"},
    {"agent_panel.events_unavailable",
     "No message ledger for this task (launched by an older build); only the result is available:"},
    {"agent_panel.main_header", "── viewing main · main session ──"},

    // ---- confirm prompt ----
    {"confirm.prompt", "[y] allow once  [a] always allow this tool (this session)  [N] deny: "},
    {"confirm.opt.allow_once", "Allow once"},
    {"confirm.opt.always", "Always allow (this session)"},
    {"confirm.opt.deny", "Deny"},
    {"confirm.menu.hint", "↑/↓ to select · Enter to confirm · Esc to deny"},
    {"confirm.persist.yes", "Yes, save to project settings"},
    {"confirm.persist.no", "No, just this session"},
    {"confirm.persist.menu.hint", "↑/↓ to select · Enter to confirm"},
    {"confirm.detail.path", "    path: {0}"},
    {"confirm.detail.replace_all", "  (replace_all=true)"},
    {"confirm.detail.content", "    content ({0} bytes), first lines:"},
    {"confirm.detail.old", "    - old text:"},
    {"confirm.detail.new", "    + new text:"},
    {"confirm.detail.command", "    command ({0}): {1}"},
    {"confirm.detail.workdir", "    working directory: {0}"},
    {"confirm.detail.background", "    (run in background: returns right after spawn, output goes to a log file)"},
    {"confirm.detail.args", "    args: {0}"},
    {"confirm.detail.omitted", "      ... ({0} lines total, rest omitted)"},

    // ---- wizard ----
    {"wizard.title", "=== lubancode initial setup wizard ==="},
    {"wizard.subtitle", "(base_url / api_key not found; configure once, then the session starts)"},
    {"wizard.lang.title", "界面语言 / Language:"},
    {"wizard.lang.prompt", "选择 / Select [{0}]: "},
    {"wizard.wire.title", "Wire protocol:"},
    {"wizard.wire.opt1", "anthropic (Claude-style)"},
    {"wizard.wire.opt2", "responses (OpenAI-style)"},
    {"wizard.wire.opt3", "chat_completions (OpenAI-compatible)"},
    {"wizard.choose_prompt", "Select [1]: "},
    {"wizard.choose_prompt_n", "Select [{0}]: "},
    {"wizard.choose.hint", "↑/↓ to select · Enter to confirm · Esc to cancel"},
    {"wizard.base_url.title", "base_url (required), e.g.:"},
    {"wizard.base_url.empty", "base_url cannot be empty; try again."},
    {"wizard.api_key.prompt", "api_key (required): "},
    {"wizard.api_key.empty", "api_key cannot be empty; try again."},
    {"wizard.model.hint", "model: press Enter to fetch the list from the API, or type a model name."},
    {"wizard.model.fetch_failed", "Failed to fetch the model list: {0}"},
    {"wizard.model.manual", "Type the model name instead:"},
    {"wizard.model.empty", "model cannot be empty; try again."},
    {"wizard.model.list_empty", "The API returned an empty model list."},
    {"wizard.model.choose", "Select model number [1]: "},
    {"wizard.choice.bad_range", "Invalid number; pick again."},
    {"wizard.choice.not_number", "Not a number; enter one (or press Enter for the default)."},
    {"wizard.summary.title", "Configuration summary:"},
    {"wizard.save_prompt", "Save to {0}? [Y/n]: "},
    {"wizard.saved", "Saved to {0}"},
    {"wizard.save_failed", "Save failed: {0} (this run continues; the config just was not persisted)"},
    {"setup.entry.title", "Get started with lubancode"},
    {"setup.entry.language_progress", "Step 1 / 2"},
    {"setup.entry.language_body", "Choose the interface language. You can change it later with /language."},
    {"setup.entry.method_progress", "Step 2 / 2"},
    {"setup.entry.method_body", "No model provider is configured yet."},
    {"setup.entry.method_hint", "Add one now, or enter the main screen and configure it later."},
    {"setup.entry.add", "Add Provider"},
    {"setup.entry.add_desc", "Choose a service, add credentials, and start using it"},
    {"setup.entry.skip", "Skip for now"},
    {"setup.entry.skip_desc", "Configure later with /provider or /provider add"},
    {"setup.session.hint", "Run /provider add to add one, or /provider to manage existing providers."},
    {"setup.turn.blocked", "No Provider is ready. Run /provider add, or use /provider to switch to an existing one."},
    {"banner.not_connected", "not connected"},

    // ---- /provider add wizard (triggered by a bare /provider add, or /provider
    //      add <name>; wizard reorder adds back navigation and auth tri-state) ----
    {"provider_wizard.title", "Add provider"},
    {"provider_wizard.progress", "step {0}/{1}"},
    {"provider_wizard.footer.first", "Esc exit wizard  Ctrl+C cancel"},
    {"provider_wizard.footer.back", "Esc back  Ctrl+C cancel"},
    {"provider_wizard.current_value", "current: {0}"},
    {"provider_wizard.exit_confirm.body", "This is the first step; going back exits the wizard. Nothing will be written."},
    {"provider_wizard.exit_confirm.prompt", "Exit the wizard? [y/N]: "},
    {"provider_catalog.choose.title", "Choose a model provider (catalog from the LubanCode repository):"},
    {"provider_catalog.choose.custom", "Custom (enter every field)"},
    {"provider_catalog.selected", "Selected {0}; wire {1}, default model {2}."},
    {"provider_catalog.refreshing", "Refreshing the provider catalog..."},
    {"provider_catalog.refresh_failed", "Could not refresh the provider catalog; using the local snapshot: {0}"},
    {"provider_catalog.refresh_current", "The provider catalog is already current."},
    {"provider_catalog.refresh_ok", "Updated the provider catalog to {0}; cache: {1}."},
    {"provider_catalog.warning", "[provider catalog warning] {0}"},
    {"provider_wizard.name.prompt", "name: "},
    {"provider_wizard.name.empty", "name cannot be empty."},
    {"provider_wizard.name.hint", "Letters, digits, underscore, dot, and dash are allowed in the name."},
    {"provider_wizard.name.slug_hint", "How about: {0}"},
    {"provider_wizard.name.prefill_invalid", "The name {0} given on the command line cannot be used: {1}; asking again."},
    {"provider_wizard.wire.hint", "Pick the wire protocol - it decides request paths, the default env var, and the probe URL."},
    {"provider_wizard.wire.opt1", "Anthropic Messages"},
    {"provider_wizard.wire.desc1", "POST {base}/v1/messages · GET {base}/v1/models"},
    {"provider_wizard.wire.opt2", "OpenAI Responses"},
    {"provider_wizard.wire.desc2", "POST {base}/responses · GET {base}/models"},
    {"provider_wizard.wire.opt3", "OpenAI Chat Completions"},
    {"provider_wizard.wire.desc3", "POST {base}/chat/completions · GET {base}/models"},
    {"provider_wizard.base_url.prompt", "base_url: "},
    {"provider_wizard.base_url.empty", "base_url cannot be empty."},
    {"provider_wizard.base_url.bad_scheme", "base_url must start with http:// or https://; try again."},
    {"provider_wizard.base_url.hint",
     "Must include http:// or https://. OpenAI-compatible wires (Responses/Chat Completions) usually also need "
     "/v1, e.g. http://127.0.0.1:8000/v1"},
    {"provider_wizard.base_url.probe", "Will read {0}"},
    {"provider_wizard.base_url.v1_offer", "This service usually also needs /v1."},
    {"provider_wizard.base_url.v1_opt_use", "Use {0}"},
    {"provider_wizard.base_url.v1_opt_keep", "Keep {0}"},
    {"provider_wizard.auth.hint", "Self-hosted services often need no key; for keyed ones pick an env var or paste."},
    {"provider_wizard.auth.opt_none", "No authentication"},
    {"provider_wizard.auth.desc_none", "requests carry no auth headers at all"},
    {"provider_wizard.auth.opt_env", "Read from an environment variable"},
    {"provider_wizard.auth.desc_env", "default {0}"},
    {"provider_wizard.auth.opt_inline", "Paste an API key"},
    {"provider_wizard.auth.desc_inline", "stored in plaintext, displayed masked"},
    {"provider_wizard.auth.env.prompt", "Environment variable name (Enter for the default {0})."},
    {"provider_wizard.auth.env.note_set", "Environment variable {0} is set."},
    {"provider_wizard.auth.env.note_unset", "Environment variable {0} has no value right now - set it before fetching models."},
    {"provider_wizard.auth.env.input", "environment variable name: "},
    {"provider_wizard.auth.inline.hint", "The key is stored in plaintext in api_key and always displayed masked."},
    {"provider_wizard.auth.inline.keep", "A plaintext key is set ({0}); Enter keeps it, typing replaces it."},
    {"provider_wizard.auth.inline.input", "API key: "},
    {"provider_wizard.auth.inline.empty", "The key cannot be empty; go back and pick \"No authentication\" if you have none."},
    {"provider_wizard.auth.summary_env", "environment variable {0}"},
    {"provider_wizard.auth.summary_inline", "inline key {0}"},
    {"provider_wizard.model.prompt", "model: "},
    {"provider_wizard.model.probe", "The model list will be read from {0} [{1}]."},
    {"provider_wizard.model.hint", "Press Enter to fetch the list, or type a model name."},
    {"provider_wizard.model.manual_hint", "Type the model name."},
    {"provider_wizard.model.empty", "model cannot be empty."},
    {"provider_wizard.model.list_empty", "The API returned an empty model list."},
    {"provider_wizard.model.fetch_failed", "Could not read models: {0}"},
    {"provider_wizard.model.fetch_404_hint", "A 404 usually means the URL or the wire protocol is off."},
    {"provider_wizard.model.fetch_401_hint", "401/403 point at the key."},
    {"provider_wizard.model.fetch_network_hint", "Connection failed; check whether the service is running."},
    {"provider_wizard.model.fetch_other_hint", "Check what the server replied."},
    {"provider_wizard.model.err_network", "connection failed"},
    {"provider_wizard.model.err_other", "read failed"},
    {"provider_wizard.model.opt_manual", "Type the model name"},
    {"provider_wizard.model.opt_back_wire", "Go back and check the wire protocol"},
    {"provider_wizard.model.opt_back_url", "Go back and check base_url"},
    {"provider_wizard.model.opt_retry", "Retry"},
    {"provider_wizard.model.opt_add_v1", "Retry with /v1 added (reading {0})"},
    {"provider_wizard.effort.hint",
     "model_reasoning_effort (optional, applied automatically when switching to this provider; candidates match "
     "/think: none/low/medium/high/xhigh/max; leave blank to skip):"},
    {"provider_wizard.effort.prompt", "effort: "},
    {"provider_wizard.effort.unset", "(not set)"},
    {"provider_wizard.extra_body.hint",
     "Extra request parameters (JSON object, leave blank to skip; only for provider fields not modeled by "
     "the catalog, e.g. {\"temperature\":0.2}):"},
    {"provider_wizard.extra_body.prompt", "extra_body: "},
    {"provider_wizard.extra_body.invalid_json", "Not valid JSON: {0}; try again (leave blank to skip)."},
    {"provider_wizard.extra_body.not_object",
     "Must be a JSON object (key-value pairs in braces); try again (leave blank to skip)."},
    {"provider_wizard.extra_body.unset", "(not set)"},
    {"provider_wizard.extra_body.summary", "{0} key(s)"},
    {"provider_wizard.summary.name", "1) name       = {0}"},
    {"provider_wizard.summary.wire", "2) wire       = {0}"},
    {"provider_wizard.summary.base_url", "3) base_url   = {0}"},
    {"provider_wizard.summary.auth", "4) auth       = {0}"},
    {"provider_wizard.summary.model", "5) model      = {0}"},
    {"provider_wizard.summary.effort", "6) effort     = {0}"},
    {"provider_wizard.summary.extra_body", "7) extra_body = {0}"},
    {"provider_wizard.summary.window", "   window     = {0}"},
    {"provider_wizard.confirm.hint", "Enter saves the current settings · 1-7 edits an item · n aborts"},
    {"provider_wizard.confirm.prompt", "Choose (press Enter to save): "},
    {"provider_wizard.confirm.bad_number", "Enter 1-7; press Enter without typing to save, or enter n to abort."},

    // ---- /provider edit wizard (typo unit): same eight panels, prefilled ----
    {"provider_wizard.edit.title", "Edit provider"},
    {"provider_wizard.edit.name_locked", "1) name       = {0} (rename not supported)"},
    {"provider_wizard.edit.no_rename", "Renaming is not supported here; remove and re-add instead."},
    {"provider_wizard.edit.name_prompt", "Press Enter to go back to the summary: "},
    {"provider_wizard.edit.diff.wire", "2) wire       = {0} → {1}"},
    {"provider_wizard.edit.diff.base_url", "3) base_url   = {0} → {1}"},
    {"provider_wizard.edit.diff.auth", "4) auth       = {0} → {1}"},
    {"provider_wizard.edit.diff.model", "5) model      = {0} → {1}"},
    {"provider_wizard.edit.diff.effort", "6) effort     = {0} → {1}"},
    {"provider_wizard.edit.diff.extra_body", "7) extra_body = {0} → {1}"},
    {"provider_wizard.edit.diff_none", "No field changed; Enter writes it back unchanged, n aborts."},
    {"provider_wizard.edit.model.hint", "Type a new model name; Enter keeps the current one (no list fetch in edit)."},
    {"provider_wizard.cancelled", "Cancelled; nothing was written."},

    // ---- slash command descriptions ----
    {"slash.desc.help", "list all commands"},
    {"slash.desc.model",
     "pick a model; /model <name> switches directly; /model cheap <name> sets the background role; /model roles "
     "shows task routing"},
    {"slash.desc.provider",
     "list, add, switch, remove, set, or refresh the provider catalog; /provider add|list|switch|remove|set|refresh"},
    {"slash.desc.config", "print the effective configuration and the session model"},
    {"slash.desc.update", "check the latest GitHub Release; upgrades sync the program and official skills"},
    {"slash.desc.init", "create AGENTS.md at the project root and load it now"},
    {"slash.desc.language", "list available UI languages and switch; /language <code> switches directly"},
    {"slash.desc.image", "attach local images; /image <path> or @path in a message"},
    {"slash.desc.worktree", "create, list, or leave isolated worktrees; /worktree new [name] | list | exit keep|remove"},
    {"slash.desc.clear", "clear the conversation history"},
    {"slash.desc.exit", "quit (bare exit/quit work too)"},
    {"slash.desc.context", "show context usage; /context 256k|512k|1m changes the window temporarily"},
    {"slash.desc.compact", "compact the history; /compact <note> tells what to keep extra"},
    {"slash.desc.think", "show/set reasoning effort; levels are provider-defined (/effort alias)"},
    {"slash.desc.effort", "same as /think (reasoning effort alias)"},
    {"slash.desc.skills", "list discovered skills (official + home-level + project-level)"},
    {"slash.desc.skill", "manage skills (run bare for URL and local-path examples)"},
    {"slash.desc.mcp", "list mounted MCP servers and their tools"},
    {"slash.desc.lsp", "list LSP server status per language"},
    {"slash.desc.todos", "show the current todo list"},
    {"slash.desc.plugins", "list plugins of all three runtimes (native/Lua/process) with load warnings"},
    {"slash.desc.plugin",
     "manage one plugin: inspect / doctor / test / reload / enable|disable"},
    {"slash.desc.tools", "list tool states: core / loaded / deferred (tool_search)"},
    {"slash.desc.memory",
     "manage project memory; /memory on|off|use|learn|review|accept|edit|reject|list|remember|forget|"
     "rebuild|why"},
    {"slash.desc.sessions", "list the 20 most recent session archives here; /sessions all for every dir"},
    {"cmd.sessions.archived_none", "No archived sessions yet (/archive archives; archived ones stay out of the default list)."},
    {"slash.desc.archive", "archive this session (moved into sessions/archive/, then exits; lubancode unarchive <id> to bring it back)"},
    {"slash.desc.delete", "permanently delete this session (asks first; refused while a turn is running or an approval is pending)"},
    {"cmd.sessions.archived_header", "{0} session(s) archived (read-only list; lubancode unarchive <id> to resume):"},
    {"cmd.sessions.archived_hint", "These live under ~/.lubancode/sessions/archive/, bytes untouched."},
    {"slash.desc.resume", "open the session picker: search/filter/sort, then resume (or pass a number/id)"},
    {"slash.desc.export", "export this session as Markdown; /export <path> picks the output file"},
    {"slash.desc.title", "show the session title; /title <title> names this session"},
    {"slash.desc.soul", "show the current soul; /soul <text> writes SOUL.md; /soul clear restores default; a name switches souls"},
    {"slash.desc.prompt", "show the persona source/length; /prompt reset restores system_prompt.md"},
    {"slash.desc.background", "list background command tasks (status/PID/command); from run_command run_in_background"},
    {"slash.desc.record", "record a workflow into a skill; /record start <name> to begin, stop drafts SKILL.md, install after review"},
    {"slash.desc.plan", "read-only research then a reviewable plan; /plan <task> to plan, status, off exits, review reopens"},

    // ---- /plan (Plan mode: read-only research hard gate) ----
    {"plan.entered", "Entered Plan mode (read-only research). Write/unknown external tools are hard-denied; /plan off exits; a review prompt appears once a plan is submitted."},
    {"plan.exited", "Exited Plan mode; no plan approved."},
    {"plan.already_in", "Already in Plan mode."},
    {"plan.not_in", "Not in Plan mode (no need for /plan off)."},
    {"plan.busy", "A turn is running; mode is not switched mid-turn: press Esc first, or queue /plan for the next turn."},
    {"plan.bad_sub", "Unknown subcommand: {0}. Usage: /plan [task] | status | off | review"},
    {"plan.mode_label", "plan"},
    {"plan.review.title", "Plan review ({0} · revision {1} · sha {2})"},
    {"plan.review.hint", "up/down to choose · Enter confirm · Esc closes (stays in Plan; /plan review reopens)"},
    {"plan.review.opt.approve_confirm", "Approve, execute with Confirm (each tool still asks)"},
    {"plan.review.opt.approve_auto", "Approve, execute with Auto (safe commands unattended, risky still ask)"},
    {"plan.review.opt.stay", "Stay in Plan, keep revising"},
    {"plan.review.opt.exit", "Exit Plan, do not execute"},
    {"plan.review.approved", "Plan approved (revision {0}). Switched back to Default; a fresh execution turn follows."},
    {"plan.review.stayed", "Staying in Plan: keep giving the model revision requests."},
    {"plan.review.exited", "Exited Plan mode; plan not executed."},
    {"plan.review.stale", "This revision was superseded (or hash mismatch); answer discarded. Review the latest revision."},
    {"plan.review.no_plan", "No plan to review yet: have the model submit a <proposed_plan> in Plan mode first."},
    {"plan.review.cancelled", "Review prompt closed (still in Plan); /plan review reopens."},
    {"plan.status.in_plan", "Current mode: Plan (read-only research)."},
    {"plan.status.in_default", "Current mode: Default (implementation)."},
    {"plan.status.no_plan", "Latest plan: none."},
    {"plan.status.plan_line", "Latest plan: {0} · revision {1} · state {2}."},
    {"plan.turn.task_prefix", "[Plan mode research request] "},
    {"plan.turn.handoff", "[Approved plan · {0} revision {1}]\n\nImplement the approved plan below. During execution, break it into a todo_write checklist first, then act; run the plan's verification steps as written.\n\n"},
    {"plan.ambiguous", "Multiple <proposed_plan> tags in this turn (nested or two drafts); treated as plain text, no approval prompt. Ask the model to resubmit one complete replacement."},
    {"plan.truncated", "Plan tag not closed (mid-stream or truncated); treated as plain text. Resubmit complete next turn."},
    {"plan.recorded", "Plan received: {0} revision {1} ({2} bytes)."},
    {"plan.resume.approved_pending", "Last session's plan was approved but execution never started: resume it yourself (/plan review to read it, or just dispatch the work)."},
    {"plan.env.bad_mode", "LUBANCODE_COLLABORATION_MODE does not recognize \"{0}\": only plan or default; starting as default."},
    {"plan.settings.bad_mode", "default_collaboration_mode in settings.local.json does not recognize \"{0}\": only plan or default; starting as default."},

    // ---- /record (record & replay) ----
    {"record.usage",
     "Usage:\n"
     "  /record start <name>      start recording (asks goal, variable inputs, done criteria)\n"
     "  /record note <why>        append a user note\n"
     "  /record pause             pause (status line shows REC paused)\n"
     "  /record resume            resume recording\n"
     "  /record stop              stop and draft SKILL.md, preview, optionally install\n"
     "  /record cancel            cancel and delete this recording (installed skills untouched)\n"
     "  /record status            show current recording state\n"
     "  /record list              list recordings (including leftovers from crashes)\n"
     "  /record install <id> [project|home]  install the draft of a recording\n"
     "  /record discard <id>      discard a recording\n"},
    {"record.unavailable", "[record] home directory not found; recording unavailable."},
    {"record.status.idle", "Not recording. Start with /record start <name>."},
    {"record.status.recording", "Recording ({0}): {1}\nRecording dir: {2}"},
    {"record.status.recording_word", "active"},
    {"record.status.paused_word", "paused"},
    {"record.status.paused_marker", "REC paused"},
    {"record.already_active", "[record] already recording: {0}. /record stop or /record cancel first."},
    {"record.not_active", "[record] not recording. Start with /record start <name>."},
    {"record.ask.goal", "What should this workflow produce?"},
    {"record.ask.variables", "Which values change every run? (Enter to skip)"},
    {"record.ask.acceptance", "What tells you it succeeded?"},
    {"record.ask.verification", "Result of the final verification? (Enter to skip)"},
    {"record.started", "[record] started: {0}\nRecording dir: {1}\nStatus line shows REC; /record stop drafts the skill."},
    {"record.start.failed", "[record] failed to start: {0}"},
    {"record.op_failed", "[record] {0}"},
    {"record.note_saved", "[record] note recorded."},
    {"record.paused_msg", "[record] paused; /record resume to continue."},
    {"record.resumed_msg", "[record] resumed."},
    {"record.stop_done", "[record] stopped. Recording: {0} ({1})"},
    {"record.stop.draft_failed", "[record] drafting failed: {0}"},
    {"record.draft.header", "Draft generated ({0} file(s)), full text below: ----"},
    {"record.install.prompt", "Install to which level? [p]roject / [h]ome / anything else skips: "},
    {"record.install.files", "The following will be written under {0}:"},
    {"record.skill_name_placeholder", "skill-name"},
    {"record.install.confirm", "Install? [y/N]: "},
    {"record.install.cancelled", "[record] not installed. The draft stays in the recording; /record install <id> works later."},
    {"record.install.done", "[record] skill {0} installed under {1}; skill list refreshed, usable right away."},
    {"record.install.failed", "[record] install failed: {0}"},
    {"record.install.not_found", "[record] recording {0} not found. See /record list."},
    {"record.install.no_draft", "[record] recording {0} has no draft (half recordings never reach skills; run /record stop first)."},
    {"record.list.header", "Recordings (newest first):"},
    {"record.list.empty", "No recordings yet. Start with /record start <name>."},
    {"record.list.entry", "[{0}] {1}  {2}  {3}  {4}"},
    {"record.list.finished", "stopped"},
    {"record.list.unfinished", "unfinished (crashed or not stopped; cannot install)"},
    {"record.list.has_draft", "has draft"},
    {"record.list.no_draft", "no draft"},
    {"record.discard_done", "[record] discarded {0}."},
    {"slash.desc.peers", "list other Lubancode sessions on this machine (name/status/cwd); arrow keys, Enter for details"},
    {"slash.desc.send", "/send <name-or-id> <text>: pass a short note to another session"},
    {"slash.desc.peerperm", "/peerperm auto|accept|hold|refuse: how incoming peer messages are received"},

    // ---- /update ----
    {"cmd.update.usage", "Usage: /update or /update check"},
    {"cmd.update.checking", "Checking the latest GitHub Release..."},
    {"cmd.update.failed", "Update check failed: {0}"},
    {"cmd.update.current", "No update found. Current: {0}; latest: {1}."},
    {"cmd.update.available", "An update is available. Current: {0}; latest: {1}."},
    {"cmd.update.release", "Release page: {0}"},
    {"cmd.update.install_hint", "Download the new release and run its installer. The program and official skills update together; user skills stay untouched."},

    // ---- 跨会话传话 /peers /send /peerperm ----
    {"cmd.peers.start_failed", "[peers] cross-session messaging failed to start: {0}"},
    {"cmd.peers.off", "Cross-session messaging is not enabled for this session (interactive sessions only)."},
    {"cmd.peers.empty", "No other visible sessions right now."},
    {"cmd.peers.hint", "up/down to select · Enter for details · Esc to dismiss"},
    {"cmd.peers.status.idle", "idle"},
    {"cmd.peers.status.busy", "busy"},
    {"cmd.peers.status.waiting", "waiting"},
    {"cmd.peers.status.closing", "closing"},
    {"cmd.peers.held_notice", "[peers] {0}({1}) sent: {2}"},
    {"cmd.peers.held_prompt", "Hand it to the model? [y/N]: "},
    {"cmd.peers.held_dropped", "Dropped; not handed to the model."},
    {"cmd.peers.incoming_notice", "[message from {0}({1})]"},
    {"cmd.send.usage", "Usage: /send <name-or-id> <text>"},
    {"cmd.send.unknown_target", "No session named {0}; /peers lists the visible ones."},
    {"cmd.send.result", "To {0}({1}): {2}"},
    {"cmd.send.label.delivered", "delivered"},
    {"cmd.send.label.held", "held by the recipient, waiting for its user"},
    {"cmd.send.label.refused", "refused by the recipient"},
    {"cmd.send.label.expired", "not accepted (rate-limited or mailbox full)"},
    {"cmd.send.label.unavailable", "recipient not there (exited or unreachable)"},
    {"cmd.peerperm.usage", "Usage: /peerperm auto|accept|hold|refuse"},
    {"cmd.peerperm.current", "Current incoming-message tier: {0} (auto = decided by modes and cwd distance)"},
    {"cmd.peerperm.set", "Incoming-message tier set to {0}."},

    // ---- /memory ----
    {"cmd.memory.usage",
     "Usage:\n"
     "  /memory                                  show session status\n"
     "  /memory on|off                           toggle project memory (needs global grant)\n"
     "  /memory use on|off                       toggle synchronous retrieval\n"
     "  /memory learn off|review|auto            learning tier (auto needs global grant)\n"
     "  /memory review                           list pending candidates\n"
     "  /memory accept <id>                      accept a candidate into the store\n"
     "  /memory edit <id> title [:: body]        edit a candidate\n"
     "  /memory reject <id> [reason]             reject a candidate (same topic won't return)\n"
     "  /memory list                              list project memories\n"
     "  /memory remember fact|preference|feedback title [:: body]\n"
     "  /memory remember user preference|feedback title [:: body]  (needs global grant)\n"
     "  /memory forget <id>                       archive one memory\n"
     "  /memory rebuild                           rebuild the index in background\n"
     "  /memory stale                             list drifted (fingerprint) and expired memories\n"
     "  /memory verify <id>                       re-verify, reviving the entry under its id\n"
     "  /memory refresh <id>                      re-verify and reset status to active\n"
     "  /memory migrate                           batch-migrate legacy topics to front matter (plan first)\n"
     "  /memory show <id>                         show one topic's front matter and body\n"
     "  /memory open [id]                         edit a topic or the index via $VISUAL/$EDITOR\n"
     "  /memory why [id]                          explain the last recall: hits, misses, blocks\n"},
    {"cmd.memory.unavailable", "[memory] The home directory is unavailable; project memory cannot run."},
    {"cmd.memory.on", "on"},
    {"cmd.memory.off", "off"},
    {"cmd.memory.global", "Global grant: {0}"},
    {"cmd.memory.denied",
     "[memory] Project memory is not enabled in your global config, so session commands cannot turn it "
     "on. Add \"memory\": {\"enabled\": true} to <home>/.lubancode/config.json and restart lubancode."},
    {"cmd.memory.status", "Project memory: {0}; retrieval {1}; writes {2}"},
    {"cmd.memory.learn_status", "Learning tier: {0} (off/review/auto)"},
    {"cmd.memory.candidates", "Pending candidates: {0} (/memory review)"},
    {"cmd.memory.learn_denied", "[memory] {0}"},
    {"cmd.memory.learn_set", "[memory] Learning tier set to {0}."},
    {"cmd.memory.review.empty", "[memory] No pending candidates."},
    {"cmd.memory.review.header", "Pending candidates:"},
    {"cmd.memory.review.hint",
     "Use /memory accept <id> to accept, /memory edit <id> title::body to edit, /memory reject <id> "
     "[reason] to reject."},
    {"cmd.memory.reject.done", "[memory] Candidate rejected; the same topic won't be proposed again."},
    {"cmd.memory.edit.done", "[memory] Candidate updated; still pending review."},
    {"cmd.memory.project", "Project: {0}"},
    {"cmd.memory.directory", "Directory: {0}"},
    {"cmd.memory.counts", "Entries: {0}; pending: {1}"},
    {"cmd.memory.master", "[memory] Project memory is now {0} for this session."},
    {"cmd.memory.toggle", "[memory] The {0} sub-switch is now {1}."},
    {"cmd.memory.retrieval", "retrieval"},
    {"cmd.memory.catalog_warning", "[memory] The catalog is invalid; scanned topic files instead: {0}"},
    {"cmd.memory.empty", "Project memory is empty."},
    {"cmd.memory.queued", "[memory] Queued for background processing: {0}"},
    {"cmd.memory.queue_failed", "[memory] Could not queue the job: {0}"},
    {"cmd.memory.worker_failed", "[memory] Could not start pending background work: {0}"},
    {"cmd.memory.project_failed", "[memory] Could not resolve project identity: {0}"},
    {"cmd.memory.switch_failed", "[memory] Could not switch the memory project: {0}"},
    {"memory.extract.running", "[memory] turn summary ({0})..."},
    {"memory.extract.failed", "[memory] turn summary failed; skipped this turn: {0}"},
    {"memory.extract.done", "[memory] {0} new candidate(s) pending (/memory review); {1} auto-saved."},
    {"cmd.memory.stale.empty", "[memory] No drifted or expired memories."},
    {"cmd.memory.stale.header", "Stale list (fingerprint = files changed, expired = past expires_at):"},
    {"cmd.memory.stale.fingerprint", "related files changed"},
    {"cmd.memory.stale.expired", "expired"},
    {"cmd.memory.stale.hint",
     "Still valid? /memory verify <id> revives it under the same id; expired rules can be renewed or "
     "archived with /memory forget."},
    {"cmd.memory.why.expired", "past expires_at; awaiting renewal or archive"},
    {"cmd.memory.why.scope", "scope does not cover the current working directory"},
    {"cmd.memory.why.none", "[memory] No recall trace yet in this session."},
    {"cmd.memory.why.header", "[memory] Last recall ({0}):"},
    {"cmd.memory.why.origin", "  query origin: {0}"},
    {"cmd.memory.why.skipped_turn", "  synthetic control message; retrieval skipped, no terms."},
    {"cmd.memory.why.terms", "  query terms: {0}"},
    {"cmd.memory.why.hit", "  {0}  score {1} (hard hits {2}, terms {3}) — injected {4} bytes"},
    {"cmd.memory.why.miss", "  {0}  score {1} (hard hits {2}, terms {3}) — not injected: {4}"},
    {"cmd.memory.why.stale", "related files changed; hint only, body withheld"},
    {"cmd.memory.why.duplicate", "duplicate fact/evidence already injected; deduped"},
    {"cmd.memory.why.superseded", "project-layer topic on the same theme was injected; user layer yielded"},
    {"cmd.memory.why.layer_user", " (user layer)"},
    {"cmd.memory.user_layer", "user layer"},
    {"cmd.memory.user_status",
     "User-level memories: {0}; directory: {1} (grant lives in global memory.user_enabled)"},
    {"cmd.memory.why.below_threshold", "score below the minimum threshold"},
    {"cmd.memory.why.budget", "result/byte budget exhausted"},
    {"cmd.memory.why.skipped", "body unavailable"},
    {"cmd.memory.why.total", "  injected {0} entries · {1} bytes"},
    {"cmd.memory.why.missing", "[memory] {0} was not part of the last recall."},
    {"cmd.memory.migrate.none",
     "[memory] No legacy topics to migrate ({0} skipped, {1} warnings)."},
    {"cmd.memory.migrate.plan",
     "[memory] Migration plan: {0} to migrate, {1} skipped, {2} warnings. "
     "Originals are backed up under .state/migration-backup/."},
    {"cmd.memory.migrate.confirm", "Migrate as planned? [y/N]: "},
    {"cmd.memory.migrate.cancelled", "Migration cancelled; legacy topics are untouched."},
    {"cmd.memory.migrate.done", "[memory] Migrated {0} topics to front matter; backup at {1}."},
    {"cmd.memory.show.header", "[memory] {0} (lives under {1}):"},
    {"cmd.memory.open.done", "[memory] Edit accepted; validated and index rebuilt."},

    {"cmd.init.created", "Created {0} and loaded it for this session."},
    {"cmd.init.exists", "Project instructions already exist at {0}; left them untouched and reloaded them."},
    {"cmd.init.failed", "Could not create AGENTS.md: {0} ({1})"},

    // ---- /language ----
    {"cmd.language.list_header", "Available languages (built-in zh-CN/en + <home>/.lubancode/languages/*.json):"},
    {"cmd.language.current_mark", "(current)"},
    {"cmd.language.choose", "Select language number [{0}]: "},
    {"cmd.language.switched", "Language switched to {0} (effective immediately, this session)"},
    {"cmd.language.unknown", "Unknown language {0} (run /language to list options)."},
    {"cmd.language.bad_number", "Invalid number; switch cancelled."},

    // ---- /soul ----
    {"soul.unavailable", "[soul] Could not read {0}; continuing without a soul."},
    {"cmd.soul.no_home", "Could not find the home directory; /soul has nowhere to store its file."},
    {"cmd.soul.available_header", "Available legacy souls (enter a name to switch):"},
    {"cmd.soul.default_item", "  - default (home SOUL.md)"},
    {"cmd.soul.current", "Currently active: {0}"},
    {"cmd.soul.empty_note", "(empty; no effect)"},
    {"cmd.soul.usage", "Usage: /soul shows it; /soul <text> writes SOUL.md; /soul clear restores default; /soul <name> switches souls; /soul off disables; /soul default returns to SOUL.md."},
    {"cmd.soul.off", "Soul disabled for this session; the next request uses the new system prompt."},
    {"cmd.soul.back_default", "Switched back to SOUL.md"},
    {"cmd.soul.switched", "Soul switched to {0}; the next request uses the new system prompt."},
    {"cmd.soul.saved", "Soul saved to SOUL.md; it is active now and after restart."},
    {"cmd.soul.cleared", "SOUL.md restored to its default; soul cleared."},
    {"cmd.soul.write_failed", "Could not write SOUL.md: {0}"},
    {"cmd.soul.default_config_failed", "SOUL.md was written, but the config could not switch back to default: {0}"},
    {"cmd.soul.switch_hint", "Earlier replies may keep the old style for a few turns; /clear removes that history."},
    {"cmd.soul.write_prompt", "Save to config? [y/N]: "},
    // ---- /worktree ----
    {"cmd.worktree.usage", "Usage: /worktree new [name] | list | exit keep|remove"},
    {"cmd.worktree.created", "Created isolated worktree: {0}\nBranch: {1}\nThe session directory has switched."},
    {"cmd.worktree.list_header", "Worktrees in this repository:"},
    {"cmd.worktree.current", "(current)"},
    {"cmd.worktree.detached", "(detached HEAD)"},
    {"cmd.worktree.kept", "Returned to the original directory; kept: {0}"},
    {"cmd.worktree.removed", "Removed worktree and branch: {0}"},
    {"cmd.worktree.dirty", "This worktree has uncommitted changes: {0}"},
    {"cmd.worktree.remove_confirm", "Still force-remove this worktree and branch? [y/N]: "},
    {"cmd.worktree.remove_cancelled", "Not removed; still in this worktree."},
    {"cmd.worktree.not_repo", "This directory is not inside a Git repository; /worktree is unavailable."},
    {"cmd.worktree.invalid_name", "Names allow letters, digits, - and _ only, up to 64 characters."},
    {"cmd.worktree.already_active", "This session is already in a new worktree; run /worktree exit keep|remove first."},
    {"cmd.worktree.no_active", "This session has no /worktree-created tree to leave."},
    {"cmd.worktree.git_failed", "Git failed: {0}"},
    {"cmd.worktree.filesystem_failed", "Filesystem operation failed: {0}"},
    {"cmd.worktree.outside_confirm", "The worktree is outside .lubancode/worktrees: {0}\nYour confirmation (y) is required before entering."},
    {"cmd.worktree.outside_prompt", "Enter this worktree outside the managed yard? (session cwd, write access and project config move with it) [y/N]: "},
    {"cmd.worktree.verify_failed", "Identity check failed; refusing to enter: {0}"},
    {"cmd.worktree.cleaned", "Cleaned up {0} stale subagent worktrees (agent- prefix only; ones with pending work were skipped)."},

    // ---- /config diagnostics ----
    {"config.header", "Effective lubancode configuration:"},
    {"config.not_set", "(not set)"},
    {"config.language.follow_system", "(not set, following system: {0})"},
    {"config.compact_model.unset", "(not set; uses the session model)"},
    {"config.think.unset", "not sent (field absent from request)"},
    {"config.soul.unset", "(not set; uses SOUL.md in the home dir)"},
    {"config.threshold.never", "(never defer)"},
    {"config.steps.unlimited", "(unlimited)"},
    // Output budget (same-level subagents spec, root cause 1): unset is spoken
    // out loud — no fake 0, no hidden magic number.
    {"config.output.unset",
     "unset (field omitted from the request; server/model default applies. anthropic requires it and falls back to "
     "the public default 8192)"},
    {"config.output.tokens", "{0} tokens"},
    {"config.output_source.config", "config agent.max_output_tokens"},
    {"config.output_source.config_subagent", "config subagent.max_output_tokens (explicit override)"},
    {"config.output_source.provider", "provider declaration"},
    {"config.output_source.catalog", "model catalog declaration"},
    {"config.output_source.unset", "unset (no declaration at any of the three levels)"},
    {"cmd.context.output_budget",
     "output budget {0} tokens [{1}] — thinking and text share this budget; it is included in the projected estimate"},
    {"cmd.context.output_budget_unset",
     "output budget unset [no declaration at any of the three levels] — field omitted, server/model default applies; "
     "projected estimate assumes a conservative 8192"},
    // /doctor agents: capability matrix between main and agent types (en pair
    // for the zh entries above; the rest of the doctor section is zh-only as of
    // this writing — new strings come in pairs per house rule).
    {"doctor.agents.header", "Capability matrix: main vs agent types (same level by default; differences come from roles or explicit config)"},
    {"doctor.agents.budget",
     "shared runtime profile: output budget {0} (0 = unset, server default) · steps {1} · length continuations {2}"},
    {"doctor.agents.governance",
     "dispatch governance: concurrency slots ≤ {0} (subagent.max_active) · depth ≤ {1} (subagent.max_depth)"},
    {"doctor.agents.row_main", "main        : {0} tools (incl. agent/todo/ask_user)"},
    {"doctor.agents.row_sub",
     "general-purpose: {0} tools (same capabilities as main; per-task private todo; may dispatch further agents)"},
    {"doctor.agents.row_explore", "Explore     : {0} tools (read-only allowlist, a role restriction — not \"subagent has no permission\")"},
    {"doctor.agents.note",
     "note: output budget/steps/continuations/concurrency/depth are one shared runtime profile for main and subagents; "
     "only Explore narrows tools by role."},
    {"doctor.agents.subagent_debug_log",
     "Subagent stream diagnostics: set LUBANCODE_DEBUG_SUBAGENT=1 and every subagent task logs one line per stream event "
     "to ~/.lubancode/logs/subagent-<id>.log (event types and byte counts only, never content or thinking; a custom "
     "directory can be given instead)."},
    {"config.label.file", "  config file        = {0}"},
    {"config.hooks.none", "(not configured)"},
    {"config.mcp.count", "{0} servers"},
    {"config.search.none", "(not configured; web_search tool not registered)"},
    {"config.label.catalog", "  model catalog      = "},
    {"config.catalog.none", "(not configured; {0} does not exist)"},
    {"config.catalog.entries", "{0} ({1} entries)"},
    {"config.label.catalog_hit", "  model in catalog   = "},
    {"config.catalog.model_unset", "(model not set)"},
    {"config.catalog.hit", "yes ({0})"},
    {"config.catalog.display_name", ", display_name: {0}"},
    {"config.catalog.miss", "no ({0} is not in the catalog; behavior unchanged)"},
    {"config.session_model", "  model in use this session = {0}"},
    {"config.session_model.note", "  (session only; not written to the config file)"},
    {"path.no_home", "<home dir not found>"},

    // ---- config sources / common errors ----
    {"config.source.lubancode_env", "LUBANCODE_ environment variable"},
    {"config.source.config_file", "config file (.lubancode/config.json)"},
    {"config.source.project_config_file", "project config (.lubancode/config.json)"},
    {"config.source.global_config_file", "global config (~/.lubancode/config.json)"},
    {"config.source.generic_env", "generic environment variable (ANTHROPIC_*/OPENAI_*)"},
    {"config.source.default", "built-in default"},
    {"config.source.unknown", "unknown source"},
    {"error.api_key_missing",
     "Missing API key; cannot talk to the model without it. Looked in these places, highest priority\n"
     "first, and found nothing:\n"
     "  1) environment variable LUBANCODE_API_KEY\n"
     "  2) the api_key field of the config file (.lubancode/config.json in cwd or the home dir;\n"
     "     legacy .lubancode.json is accepted and migrated automatically)\n"
     "  3) generic environment variable {0}\n"
     "  4) built-in defaults (api_key has none; you must configure one)\n"
     "Pick one, then run lubancode again. --config shows what each field currently resolves to."},
    {"error.not_configured",
     "Missing configuration: {0}. Cannot talk to the model (lubancode ships no vendor defaults).\n"
     "Pick one of three paths:\n"
     "  1) run lubancode without positional arguments; interactive mode starts the setup wizard\n"
     "  2) put a .lubancode/config.json in your home directory (legacy .lubancode.json is accepted\n"
     "     and migrated) and set {0} in it (all fields optional)\n"
     "  3) set the corresponding environment variables: {1}\n"
     "Afterwards, --config shows what each field resolves to and from which tier."},

    // ---- common errors ----
    {"error.prefix", "[error] "},
    {"error.unexpected", "unexpected exception: {0}"},
    {"error.unknown_command", "Unknown command {0}; try /help"},
    {"error.wizard_incomplete", "Setup wizard not completed; exiting."},
    {"error.system_prompt_arg", "--system-prompt requires a file path"},
    {"image.attached", "[image] attached {0} ({1}x{2})"},
    {"error.image.missing_path", "Image path is empty. Use /image <path>, or @path in a message."},
    {"error.image.not_found", "Image file not found: {0}"},
    {"error.image.not_regular", "This is not a regular file and cannot be attached: {0}"},
    {"error.image.unsupported", "Unsupported image type: {0} (png/jpg/jpeg/gif/webp only)."},
    {"error.image.too_large", "Image is too large: {0} (5MB limit; compress it first)."},
    {"error.image.read_failed", "Could not read image: {0}"},
    {"error.image.invalid", "Image data is invalid; could not read its dimensions: {0}"},
    {"i18n.pack_warning", "[language pack warning] {0}"},

    // ---- M11: network timeout errors (used by the api layer, client.cpp/models.cpp) ----
    {"error.network.connect_timeout",
     "connection timed out: couldn't connect within {0}s; check your network, proxy settings, or base_url"},
    {"error.network.stream_idle_timeout",
     "network read timed out: no new data for {0}s straight; the connection may have dropped, please retry"},
    {"error.network.request_timeout", "request timed out: did not finish within {0}s; check your network and retry"},
    {"error.network.connect_failed", "connection failed: {0}"},
    {"error.network.hard_timeout",
     "request hard timeout: the whole request was force-aborted after {0}s (request_hard_timeout_secs). Usually the "
     "connection was hijacked by a proxy/TUN or the server went silent; check the network before retrying, and raise "
     "this limit for long tasks"},

    // ---- transcript summary words ----
    {"transcript.pending", "Awaiting confirmation"},
    {"transcript.read_lines", "Read {0} lines"},
    {"transcript.checking_hook", "Checking hooks..."},
    {"transcript.hook_blocked", "Blocked by hook (not executed)"},
    {"transcript.exit_code", "exit code {0}"},
    {"transcript.added", "Added {0} lines"},
    {"transcript.added_removed", "Added {0} lines, removed {1} lines"},
    {"transcript.hits", "{0} matches"},
    {"transcript.agent", "Subagent {0} steps · {1} tool calls"},
    {"transcript.error_no_output", "Error: (no output)"},
    {"transcript.error_exit_code", "Error: exit code {0}"},
    {"transcript.error_truncated", "({0} lines total, Ctrl+E for the full output)"},
    {"transcript.params_prefix", "args: "},
    {"transcript.no_full_output", "(no full output)"},
    {"transcript.full_output_header", "── full output ({0} lines) ──"},
    {"transcript.todo_count", "{0} items"},
    {"transcript.thinking_running", "Thinking…"},
    {"transcript.thinking_done", "Thought {0}"},
    {"transcript.thinking_chars", " · {0} chars"},
    {"transcript.thinking_stream_more", "… {0} lines so far — press Ctrl+O again after thinking finishes for the full text"},
    {"transcript.batch_pending", "(queued in this step)"},
    {"transcript.batch_skipped", "not run this step (interrupted)"},
    {"transcript.more_lines", " +{0} lines"},
    {"todo.empty", "No todos."},

    // ---- stats line ----
    {"stats.line", "[tokens] in {0}{1} · out {2} · {3} requests · context {4}%"},
    {"stats.cache", " (cache hit {0}, {1}%)"},

    // ---- pipe mode stable output ----
    {"pipe.tool_start", "[tool] "},
    {"pipe.tool_done", "[tool done] "},
    {"pipe.subtool_start", "  [subagent tool] "},
    {"pipe.todo_updated", "todo list updated ({0} items)"},

    // ---- /skill ----
    {"cmd.skill.usage",
     "Skill management (home-level installs are available immediately):\n"
     "  /skill list\n"
     "      List home/project skills and their local or remote source.\n"
     "  /skill install https://example.com/my-skill.md\n"
     "  /skill install https://github.com/owner/repo\n"
     "      Install from a Markdown URL or GitHub skill repository.\n"
     "  /skill install C:\\path\\to\\my-skill\n"
     "  /skill install C:\\path\\to\\my-skill\\SKILL.md\n"
     "      Install a local directory or SKILL.md; spaces in paths are supported.\n"
     "  /skill update [name]\n"
     "      Update one or all installed skills that have a saved remote source.\n"
     "  /skill remove <name>\n"
     "      Remove a home-level skill.\n"
     "Home path: ~/.lubancode/skills/<name>/SKILL.md\n"
     "Project path (manual): <cwd>/.lubancode/skills/<name>/SKILL.md"},
    {"cmd.resume.usage", "Usage: /resume (full-screen picker) | /resume <number> | /resume <id>"},
    {"cmd.resume.cancelled", "Resume cancelled."},
    {"cmd.resume.queue_restored", "({0} queued message(s) restored too; sent when the turn ends)"},
    {"cmd.resume.worktree_gone", "The worktree this session lived in is gone: {0}. Staying in the current directory; binding to the worktree cleared."},
    {"cmd.resume.worktree_refused", "Refusing to enter {0}: identity check failed ({1}). Staying in the current directory; inspect its .git pointer and retry."},
    {"cmd.resume.worktree_back", "Moved back into the session worktree: {0}"},
    {"cmd.resume.history.header", "Restored history · {0}"},
    {"cmd.resume.history.end", "── End of history; continue below ──"},
    // ---- Archive & permanent delete (session manager, steps 4-5) ----
    {"cmd.session.archive.usage", "Usage: lubancode archive <id|title> · lubancode unarchive <id>"},
    {"cmd.session.delete.usage",
     "Usage: lubancode delete <id|title> [--force]. Permanent, unrecoverable; --force skips the "
     "confirmation (scripts only)."},
    {"cmd.session.ref_not_found", "No session matches {0} (by id or title; try /sessions first)."},
    {"cmd.session.ref_ambiguous", "Reference {0} matches several sessions; name the full id: {1}"},
    {"cmd.session.archive.done", "Archived {0} (moved into sessions/archive/, bytes untouched)."},
    {"cmd.session.archive.failed", "Archiving {0} failed; the file is untouched and still usable."},
    {"cmd.session.unarchive.done", "Unarchived {0} (back in sessions/, resumable)."},
    {"cmd.session.unarchive.failed", "Unarchiving {0} failed; the file is untouched and still usable."},
    {"cmd.session.delete.confirm_header", "Permanent deletion"},
    {"cmd.session.delete.confirm_title", "  Title: {0}"},
    {"cmd.session.delete.confirm_id", "  id: {0}"},
    {"cmd.session.delete.confirm_cwd", "  Cwd: {0}"},
    {"cmd.session.delete.confirm_prompt", "This permanently deletes the session and cannot be undone. Type y to confirm, anything else cancels: "},
    {"cmd.session.delete.cancelled", "Cancelled; nothing was deleted."},
    {"cmd.session.delete.done", "Permanently deleted {0} (this session only; artifact blobs are content-addressed and may be shared)."},
    {"cmd.session.delete.failed", "Deleting {0} failed; the file is untouched."},
    {"cmd.archive.not_active", "This session has never been persisted; there is nothing to archive."},
    {"cmd.delete.not_active", "This session has never been persisted; there is nothing to delete."},
    {"cmd.archive.exiting", "Current session archived; exiting."},
    {"cmd.delete.exiting", "Current session deleted; exiting."},
    {"cmd.archive.usage", "Usage: /archive (no arguments; it archives THIS session; use lubancode archive <id> for another one)"},
    {"cmd.archive.busy", "Background subagents are still running; let them finish (or stop them from the /agents panel) before archiving."},
    {"cmd.delete.usage", "Usage: /delete (no arguments; it deletes THIS session; use lubancode delete <id> for another one)"},
    {"cmd.delete.busy", "Background subagents are still running; let them finish (or stop them from the /agents panel) before deleting."},
    {"cmd.resume.history.user", "You"},
    {"cmd.resume.history.assistant", "Assistant"},
    {"cmd.resume.history.image", "[Image] {0} ({1}x{2})"},
    {"cmd.resume.history.compact", "── Context was compacted here ──"},
    {"cmd.resume.history.tool_missing", "tool result missing during restore"},
    {"cmd.resume.history.tool_error", "tool failed"},
    {"cmd.resume.history.tool_done", "tool completed"},
    {"cmd.resume.history.tool_more", " · {0} more lines"},
    {"cmd.skill.no_home", "Cannot find the home directory; nowhere to store skills."},
    {"cmd.skill.list_empty", "There are no skills here yet. Run /skill install <URL or local path> to add one."},
    {"cmd.skill.list_header", "Local skills:"},
    {"cmd.skill.local", "local"},
    {"cmd.skill.remote", "remote {0}, installed {1}"},
    {"cmd.skill.scope_global", "home-level"},
    {"cmd.skill.scope_project", "project-level"},
    {"cmd.skill.install_done", "Installed: {0}"},
    {"cmd.skill.update_done", "Updated: {0}"},
    {"cmd.skill.update_none", "There are no remote skills with saved sources to update."},
    {"cmd.skill.remove_done", "Removed: {0}"},
    {"cmd.skill.error", "{0} failed: {1}"},
    {"cmd.skill.refreshed", "Skill discovery was refreshed; later turns in this session can use it."},

    // ---- /provider ----
    {"cmd.provider.usage",
     "Usage:\n"
     "  /provider list\n"
     "  /provider refresh                       update common providers from the LubanCode repository\n"
     "  /provider add                          step-by-step wizard (bare)\n"
     "  /provider add <name>                    step-by-step wizard (name given up front, skips the first question)\n"
     "  /provider add <name> <base_url> <anthropic|responses|chat_completions> [--key-env ENV] [--key API_KEY] [--model MODEL] "
     "[--effort LEVEL] [--window SIZE]\n"
     "  /provider switch <name> [model]\n"
     "  /provider remove <name>\n"
     "  /provider set <name> native_web_search on|off   toggle server-side native web search "
     "(also accepts true/false, 1/0)\n"
     "  /provider set <name> extra_body <JSON object>   extra top-level request fields to attach on every "
     "request (shallow merge, overrides built-in fields; pass {} or empty to clear)\n"
     "  /provider set <name> extra_header <Header-Name> <value>   extra HTTP header to attach on every request "
     "(same name overrides a built-in header; empty value deletes it)"},
    {"cmd.provider.empty", "No providers are configured. Use /provider add to add one."},
    {"cmd.provider.header", "Configured providers:"},
    {"cmd.provider.line", "  - {0} [{1}] {2}; model={3}; window={4}; {5}{6}{7}"},
    {"cmd.provider.current", " (current)"},
    {"cmd.provider.model_unset", "(not set)"},
    {"cmd.provider.extra_api_key", "; api_key={0}"},
    {"cmd.provider.extra_effort", "; effort={0}"},
    {"cmd.provider.extra_web_search", "; native_web_search=on"},
    {"cmd.provider.extra_body_hint", "; extra_body={0} key(s)"},
    {"cmd.provider.extra_headers_hint", "; extra_headers={0} entr(y/ies)"},
    {"cmd.provider.added", "Added provider {0} and saved it to global config {1}."},
    {"cmd.provider.add_cancelled", "Cancelled; no provider was added."},
    {"cmd.provider.add_failed", "Could not add provider: {0}"},
    {"cmd.provider.add_kept_connection",
     "Saved {0}, but it has no default model, so nothing switched; this session keeps the current "
     "connection. Set a model, then /provider switch {0}."},
    {"cmd.provider.exists", "Provider already exists: {0}"},
    {"cmd.provider.switched", "Switched to provider {0}; later requests use {1}."},
    {"cmd.provider.remembered", "Remembered provider {0}; it will remain active next time."},
    {"cmd.provider.remember_failed", "Provider switched, but the selection could not be saved: {0}"},
    {"cmd.provider.effort_applied", "Applied reasoning effort {1} from provider {0}."},
    {"cmd.provider.not_found", "Provider not found: {0}"},
    {"cmd.provider.key_missing", "Provider {0} needs environment variable {1}, but it is not set."},
    {"cmd.provider.key_missing_inline",
     "Provider {0} uses an inline key (auth=inline) but api_key is empty. Switch the mode with "
     "/provider set {0} auth env|none, or provide the key."},
    {"cmd.provider.auth_none", "no auth"},
    {"cmd.provider.auth_env_prompt", "Environment variable name (for reading the key): "},
    {"cmd.provider.auth_inline_prompt", "API key (stored in plaintext; displayed masked): "},
    {"cmd.provider.auth_aborted", "Cancelled; nothing was changed."},
    {"cmd.provider.switch.usage_short", "Usage: /provider switch <name> [model]"},

    // ---- /provider subcommand fault tolerance (typo unit) ----
    {"cmd.provider.typo_hint", "Unknown subcommand `{0}` — did you mean `{1}`?"},
    {"cmd.provider.bad_args", "Those arguments don't fit."},
    {"cmd.provider.unknown_sub.tty",
     "Unknown subcommand: {0}. Common ones:\n"
     "  /provider add\n"
     "  /provider switch\n"
     "  /provider list"},
    {"cmd.provider.unknown_sub.pipe",
     "Usage: /provider <subcommand>; run /provider list to see configured providers."},
    {"cmd.provider.usage_short.list", "Usage: /provider list"},
    {"cmd.provider.usage_short.refresh", "Usage: /provider refresh"},
    {"cmd.provider.usage_short.add",
     "Usage: /provider add [name] (wizard), or /provider add <name> <base_url> "
     "<anthropic|responses|chat_completions> [--key-env ENV] [--key API_KEY] [--model MODEL] [--effort LEVEL] "
     "[--window SIZE]"},
    {"cmd.provider.usage_short.remove", "Usage: /provider remove <name>"},
    {"cmd.provider.usage_short.set",
     "Usage: /provider set <name> <field> <value> (fields: auth, native_web_search, extra_body, extra_header)"},
    {"cmd.provider.usage_short.edit", "Usage: /provider edit <name> (bare opens a picker)"},

    // ---- /provider edit (typo unit) ----
    {"cmd.provider.edit.saved", "Saved changes to provider {0}; global config at {1}."},
    {"cmd.provider.edit.save_failed", "Could not save provider changes: {0}"},
    {"cmd.provider.edit.cancelled", "Cancelled; nothing was changed."},

    // ---- /provider switch picker (wizard reorder) ----
    {"provider_switch.title", "Switch provider"},
    {"provider_switch.footer",
     "Up/Down select  Enter switch  Esc cancel  type to filter (press e to edit when filter is empty)"},
    {"provider_switch.edit_title", "Edit which provider?"},
    {"provider_switch.footer_edit", "Up/Down select  Enter edit  Esc cancel  type to filter"},
    {"provider_switch.filter_line", "filter: {0}"},
    {"provider_switch.filter_empty", "(none)"},
    {"provider_switch.empty_hint", "No providers configured yet."},
    {"provider_switch.no_match_hint", "No provider matches the filter."},
    {"provider_switch.opt_add", "Add a provider"},
    {"provider_switch.opt_cancel", "Cancel"},
    {"provider_switch.auth_ready", "ready"},
    {"provider_switch.auth_env_missing", "key missing (needs {0})"},
    {"provider_switch.auth_inline_missing", "inline key missing"},

    // ---- /provider switch missing-key remediation page ----
    {"provider_remedy.title", "Cannot switch to {0} yet"},
    {"provider_remedy.body_env", "Required environment variable: {0}\nThe current process sees no value."},
    {"provider_remedy.body_inline", "{0} uses an inline key (auth=inline) but api_key is empty."},
    {"provider_remedy.footer", "Up/Down select  Enter confirm  Esc back to list  Ctrl+C cancel"},
    {"provider_remedy.opt_input_key", "Enter an API key now"},
    {"provider_remedy.hint_env", "pick where it goes first: this session only, or the user config"},
    {"provider_remedy.hint_inline", "paste a key into api_key"},
    {"provider_remedy.opt_change_env", "Use a different environment variable"},
    {"provider_remedy.opt_no_auth", "Set to no authentication"},
    {"provider_remedy.opt_howto", "Show how to set it"},
    {"provider_remedy.opt_back", "Back to the provider list"},
    {"provider_remedy.key_session", "This session only"},
    {"provider_remedy.key_session_desc", "not written to disk; the old config applies after restart"},
    {"provider_remedy.key_persist", "Write to the user config"},
    {"provider_remedy.key_persist_desc", "stored in plaintext, always displayed masked, at your own risk"},
    {"provider_remedy.key_saved", "Stored the key for {0} ({1}) in {2}."},
    {"provider_remedy.key_session_only", "The key ({0}) applies to this session only; nothing was written."},
    {"provider_remedy.none_saved", "Set {0} to no authentication; saved to {1}."},
    {"provider_remedy.howto_powershell", "PowerShell: $env:{0} = \"your-key\""},
    {"provider_remedy.howto_cmd", "cmd: setx {0} \"your-key\""},
    {"provider_remedy.howto_posix", "POSIX shell: export {0}=your-key"},
    {"provider_remedy.howto_restart", "Restart LubanCode (or reopen the terminal) after setting it."},
    {"cmd.provider.removed", "Removed provider {0}; global config is {1}."},
    {"cmd.provider.remove_active", "Provider {0} is in use. Switch away before removing it."},
    {"cmd.provider.remove_failed", "Could not remove provider: {0}"},
    {"cmd.provider.set_ok", "Set {1} of provider {0} to {2}; saved to global config {3}."},
    {"cmd.provider.set_failed", "Could not set provider field: {0}"},
    {"cmd.provider.set_unknown_field", "Unknown field: {0} (only native_web_search, extra_body, extra_header, "
                                        "auth are supported for now)"},
    {"cmd.provider.set_active_applied",
     "Provider {0} is currently active; the change took effect immediately, no need to /provider switch."},
    {"cmd.provider.extra_body_invalid_json", "extra_body is not valid JSON: {0}"},
    {"cmd.provider.extra_body_not_object", "extra_body must be a JSON object (key-value pairs in braces), not "
                                             "some other type."},
    {"cmd.provider.extra_header_name_missing", "extra_header needs a header name, not just a value."},

    // ---- model routing ----
    {"cmd.model.roles_header",
     "Three model roles (routed by task; sub-agents currently follow the session model; unconfigured roles fall "
     "back to normal):"},
    {"cmd.model.roles_unavailable",
     "The model router is unavailable in this one-shot/test path; /model roles is interactive-only."},
    {"cmd.model.role_switched", "{0} role -> {1} (this session only)"},
    {"cmd.model.role_unknown",
     "Unknown model role: {0} (only normal/cheap/lao are recognized; plan is an alias of lao)."},

    // cmd.context.* 大族仍按下面的 P1 清单回退 zh-CN,这两个口径说明键随
    // "context 状态栏回合内刷新"一起先补上英文,中英成对。
    {"cmd.context.note.semantics",
     "(context = usage of the most recent main-session request; not cumulative spend, sub-agent tokens "
     "not included)"},
    {"cmd.context.note.stale",
     "(the most recent request returned no usage; figures above are from the last measured request. The "
     "status bar shows the same numbers with a ~ prefix)"},

    // ---- compact (0.27.x): new layered-compaction keys, zh+en paired ----
    {"cmd.compact.window_unknown", "(compact model window unknown; no window check was performed this time)"},
    {"cmd.compact.hierarchical", "History exceeded a single compact request: split into {0} episode chunks (map) and merged into the final summary (reduce passes: {1})."},
    {"cmd.compact.manifest", "manifest conservation check passed: {0} constraints / {1} open items"},
    {"cmd.compact.dryrun.header", "/compact --dry-run: calculation only; history and requests untouched."},
    {"cmd.compact.dryrun.reclaim", "Structural compression can reclaim ~{0} bytes ({1} exact duplicates collapsed · {2} superseded file reads · {3} long results offloaded; already applied to every request automatically)"},
    {"cmd.compact.dryrun.pinned", "Pinned, never compressed: recent hot zone ~{0} tokens · {1} active open items (kept verbatim on compaction)"},
    {"compact.midturn_start", "[compact] mid-tool-loop: next request projected to overflow the window; compacting history first..."},
    {"compact.midturn_done", "[compact] mid-turn compaction done; tool loop continues."},
    {"compact.done_stats", "[compact] history ~{0} tokens; manifest kept {1} constraints / {2} open items"},
    {"compact.hard_trim_turns", "[warning] Lossy hard trim: {0} mid-history messages were dropped (character safety net, not semantic compaction). The model can no longer see that text; the full ledger is still in the session file (/export to view, /compact to rebuild the summary)."},
    {"compact.hard_trim_results", "[warning] Lossy hard trim: oversized tool results were truncated (character safety net, not semantic compaction). The model can no longer see the cut text; the full ledger is still in the session file (/export to view)."},

    // ---- Session picker (SessionPicker, the full-screen ledger behind bare /resume) ----
    {"picker.title", "Resume a previous session"},
    {"picker.search.placeholder", "Type to search (title/first line/id/cwd)..."},
    {"picker.filter.label", "Filter"},
    {"picker.filter.cwd", "Cwd"},
    {"picker.filter.all", "All"},
    {"picker.sort.label", "Sort"},
    {"picker.sort.updated", "Updated"},
    {"picker.sort.created", "Created"},
    {"picker.empty.none", "No session archives in this directory yet (Tab to Filter, switch to All)."},
    {"picker.empty.search", "No match: {0}"},
    {"picker.no_text", "(no user text)"},
    {"picker.damaged", "damaged"},
    {"picker.unknown_dir", "(unknown dir)"},
    {"picker.unknown_model", "(unknown model)"},
    {"picker.unknown_time", "(unknown time)"},
    {"picker.expand.title", "Title:"},
    {"picker.expand.cwd", "Cwd:"},
    {"picker.expand.id", "id:"},
    {"picker.expand.model", "Model:"},
    {"picker.expand.messages", "{0} messages"},
    {"picker.expand.created", "Created:"},
    {"picker.expand.updated", "Updated:"},
    {"picker.transcript.title", "Transcript · {0}"},
    {"picker.transcript.empty", "(no transcript to show for this session)"},
    {"picker.transcript.footer",
     "esc/ctrl+t close, back to the list · enter resume · arrows leave the selection alone"},
    {"picker.footer",
     "enter resume · esc exit · tab focus · </> change option · up/down browse · pgup/pgdn page · "
     "home/end jump · ctrl+o comfortable · ctrl+t transcript · ctrl+e expand"},
    {"picker.status", "{0} / {1} · {2}%"},
    {"picker.status.empty", "0 / 0 · 0%"},
    {"picker.ago.now", "just now"},
    {"picker.ago.minutes", "{0}m ago"},
    {"picker.ago.hours", "{0}h ago"},
    {"picker.ago.days", "{0}d ago"},

    // TODO(P1):以下 zh-CN 键暂缺英文翻译,tr 回退 zh-CN——诚实回退,不机翻凑数:
    //   mcp.* / plugin.* / tool_search.* / catalog.* / cmd.tools.* / cmd.plugins.* /
    //   cmd.mcp.* / cmd.lsp.* / cmd.skills.* / cmd.context.* / cmd.compact.* / compact.* /
    //   cmd.think.* / cmd.model.* / cmd.write_config* / cmd.session_only /
    //   cmd.prompt.* / law.* / resetprompt.* / session.* / cmd.sessions.* / cmd.resume.* /
    //   cmd.export.* / cmd.title.* / cmd.clear.* / ui.* / diff.* / settings.local.*
};

std::map<std::string, std::string, std::less<>> BuildMap(const Entry* entries, std::size_t count) {
    std::map<std::string, std::string, std::less<>> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.emplace(entries[i].key, entries[i].value);
    }
    return out;
}

const std::map<std::string, std::string, std::less<>>& BuiltinTable(std::string_view code) {
    static const auto zh = BuildMap(kZhCN, sizeof(kZhCN) / sizeof(kZhCN[0]));
    static const auto en = BuildMap(kEn, sizeof(kEn) / sizeof(kEn[0]));
    static const std::map<std::string, std::string, std::less<>> empty;
    if (code == "zh-CN") {
        return zh;
    }
    if (code == "en") {
        return en;
    }
    return empty;
}

// 全局状态:当前语言 + 用户语言包。锁只护这几样;值的引用在下一次
// LoadLanguagePacksFromDir 之前稳定(SetLanguage 不动表本身)。
std::mutex& StateMutex() {
    static std::mutex m;
    return m;
}
std::string& CurrentLangRef() {
    static std::string lang = "zh-CN";
    return lang;
}
std::map<std::string, std::map<std::string, std::string, std::less<>>>& UserPacks() {
    static std::map<std::string, std::map<std::string, std::string, std::less<>>> packs;
    return packs;
}

// 在"某一种语言"里查:先用户包,后内置。找不到给 nullptr。
const std::string* LookupIn(const std::string& code, std::string_view key) {
    const auto& packs = UserPacks();
    if (const auto pack_it = packs.find(code); pack_it != packs.end()) {
        if (const auto it = pack_it->second.find(key); it != pack_it->second.end()) {
            return &it->second;
        }
    }
    const auto& builtin = BuiltinTable(code);
    if (const auto it = builtin.find(key); it != builtin.end()) {
        return &it->second;
    }
    return nullptr;
}

}  // namespace

const std::string& tr(std::string_view key) {
    std::lock_guard<std::mutex> lock(StateMutex());
    const std::string& current = CurrentLangRef();
    if (const std::string* hit = LookupIn(current, key)) {
        return *hit;
    }
    if (current != "zh-CN") {
        if (const std::string* hit = LookupIn("zh-CN", key)) {
            return *hit;
        }
    }
    // 全都没有:回退 key 本身。缓存一份好返回稳定引用。
    static std::map<std::string, std::string, std::less<>> miss_cache;
    const auto it = miss_cache.find(key);
    if (it != miss_cache.end()) {
        return it->second;
    }
    return miss_cache.emplace(std::string(key), std::string(key)).first->second;
}

std::string TrFormat(std::string_view key, const std::vector<std::string>& args) {
    const std::string& pattern = tr(key);
    std::string out;
    out.reserve(pattern.size() + 16);
    std::size_t pos = 0;
    while (pos < pattern.size()) {
        const char c = pattern[pos];
        // 只认 {digit}(单个数字)这种占位符,别的花括号原样保留——帮助
        // 文本里有 JSON 示例,不能见花括号就动。
        if (c == '{' && pos + 2 < pattern.size() && pattern[pos + 1] >= '0' && pattern[pos + 1] <= '9' &&
            pattern[pos + 2] == '}') {
            const std::size_t idx = static_cast<std::size_t>(pattern[pos + 1] - '0');
            if (idx < args.size()) {
                out += args[idx];
            } else {
                out += pattern.substr(pos, 3);  // 没有对应实参,原样保留好排查
            }
            pos += 3;
            continue;
        }
        out += c;
        ++pos;
    }
    return out;
}

const std::string& CurrentLanguage() {
    std::lock_guard<std::mutex> lock(StateMutex());
    return CurrentLangRef();
}

void SetLanguage(const std::string& code) {
    std::lock_guard<std::mutex> lock(StateMutex());
    CurrentLangRef() = code.empty() ? std::string("zh-CN") : code;
}

std::vector<std::string> AvailableLanguages() {
    std::lock_guard<std::mutex> lock(StateMutex());
    std::vector<std::string> out = {"zh-CN", "en"};
    for (const auto& [code, table] : UserPacks()) {
        if (code != "zh-CN" && code != "en") {
            out.push_back(code);
        }
    }
    return out;
}

bool HasLanguage(const std::string& code) {
    if (code == "zh-CN" || code == "en") {
        return true;
    }
    std::lock_guard<std::mutex> lock(StateMutex());
    return UserPacks().count(code) != 0;
}

std::string LanguageDisplayName(const std::string& code) {
    std::lock_guard<std::mutex> lock(StateMutex());
    if (const std::string* name = LookupIn(code, "language.name")) {
        return *name;
    }
    return code;
}

std::vector<std::string> LoadLanguagePacksFromDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> warnings;
    std::map<std::string, std::map<std::string, std::string, std::less<>>> packs;

    std::error_code ec;
    const fs::path dir_path(std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size()));
    if (fs::exists(dir_path, ec) && fs::is_directory(dir_path, ec)) {
        // 扫描全程走 error_code 重载,一处也不许抛:throwing 版的
        // entry.is_regular_file() 和 range-for 隐含的 operator++ 碰上悬空
        // 符号链接、扫描中途的权限变动会直接抛 filesystem_error,把启动
        // 掀翻——语言包是锦上添花,永远不能阻断启动。构造失败/中途递进
        // 失败记条警告收场;单个脏条目(状态查不动、不是普通文件)悄悄
        // 跳过,跟"目录/非 .json 不理会"同一待遇。
        fs::directory_iterator dir_it(dir_path, ec);
        if (ec) {
            warnings.push_back(dir + ": 语言包目录打不开,整体跳过(" + ec.message() + ")");
        }
        for (; !ec && dir_it != fs::directory_iterator(); dir_it.increment(ec)) {
            const fs::directory_entry& entry = *dir_it;
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) {
                continue;
            }
            const fs::path& path = entry.path();
            if (path.extension() != ".json") {
                continue;
            }
            const std::u8string stem_u8 = path.stem().u8string();
            const std::string code(reinterpret_cast<const char*>(stem_u8.data()), stem_u8.size());
            const std::u8string full_u8 = path.u8string();
            const std::string full(reinterpret_cast<const char*>(full_u8.data()), full_u8.size());

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                warnings.push_back(full + ": 打不开,跳过");
                continue;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();

            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(buffer.str());
            } catch (const nlohmann::json::parse_error& e) {
                warnings.push_back(full + ": 不是合法 JSON,整个文件跳过(" + e.what() + ")");
                continue;
            }
            if (!parsed.is_object()) {
                warnings.push_back(full + ": 顶层必须是 JSON object(平面键值对),整个文件跳过");
                continue;
            }
            std::map<std::string, std::string, std::less<>> table;
            bool bad = false;
            for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                if (!it.value().is_string()) {
                    warnings.push_back(full + ": 键 " + it.key() + " 的值不是字符串,整个文件跳过");
                    bad = true;
                    break;
                }
                table[it.key()] = it.value().get<std::string>();
            }
            if (bad) {
                continue;
            }
            packs[code] = std::move(table);
        }
        if (ec) {
            // dir_it.increment(ec) 失败时迭代器已被置成 end,循环自然收束
            // ——装上的照装,剩下的作罢,记一笔就行。
            warnings.push_back(dir + ": 语言包目录扫描中断,剩余文件跳过(" + ec.message() + ")");
        }
    }

    std::lock_guard<std::mutex> lock(StateMutex());
    UserPacks() = std::move(packs);
    return warnings;
}

std::string MapLocaleToLanguage(std::string_view locale) {
    if (locale.size() >= 2) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[0])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[1])));
        // 前缀后面要么结束、要么跟分隔符/子标签,别把 "english_custom" 之外
        // 的 "eo"(世界语)这类两字码误判——只看前两个字母 + 边界。
        const bool boundary = locale.size() == 2 || !std::isalpha(static_cast<unsigned char>(locale[2]));
        if (boundary) {
            if (a == 'z' && b == 'h') {
                return "zh-CN";
            }
            if (a == 'e' && b == 'n') {
                return "en";
            }
        }
    }
    return "zh-CN";
}

std::string DetectSystemLanguage() {
#ifdef _WIN32
    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
        case LANG_CHINESE:
            return "zh-CN";
        case LANG_ENGLISH:
            return "en";
        default:
            return "zh-CN";
    }
#else
    const char* lang = std::getenv("LANG");
    return MapLocaleToLanguage(lang != nullptr ? lang : "");
#endif
}

}  // namespace lubancode::cli
