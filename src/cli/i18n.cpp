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
     "  lubancode                  不带参数则进入交互循环;首次运行缺配置会先走一遍初次配置\n"
     "                              向导,配完直接进入会话,不用重启。exit/quit 或 EOF(Ctrl+Z /\n"
     "                              管道读尽)退出;空行只是重新给提示符,不退出\n"},
    {"help.options",
     "选项:\n"
     "  --version              打印版本号\n"
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
     "  /provider       列已配服务端;/provider add|switch|remove 管多端模型\n"
     "  /config         打印当前生效配置(复用 --config 的逻辑),外加本会话实际在用的 model\n"
     "  /language       列可选界面语言并切换(内置 zh-CN/en,languages/*.json 可扩展)\n"
     "  /worktree       新建/列出/退出隔离工作树;/worktree new [名字] | list | exit keep|remove\n"
     "  /clear          清空对话历史\n"
     "  /context        看当前上下文占用分析(系统提示/工具定义/对话历史分类明细 + 条形图)\n"
     "  /context 512k   临时改窗口大小(256k/512k/1m/裸数字都认),只本会话生效\n"
     "  /compact [重点说明]  手动触发一次历史压缩,可选指定这次额外保留什么\n"
     "  /think          看当前推理强度(/effort 同义)\n"
     "  /think 档位     切推理强度,档位以服务商为准(anthropic 内置 none/low/medium/high/xhigh/max\n"
     "                  映射,responses 原样递给 API)\n"
     "  /skills         列出扫描到的技能(主目录级 + 项目级)\n"
     "  /skill list     列本机技能,标本地/远端来源;/skill install <url> 安装,/skill update [名字] 更新,\n"
     "                  /skill remove <名字> 删除主目录级远端技能\n"
     "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
     "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
     "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
     "  /plugins        列出挂载的插件工具(主目录 .lubancode/plugins 下的 *.dll 和 *.lua)\n"
     "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(工具总数超过配置文件\n"
     "                  tool_search_threshold(默认 20,0=永不延迟)时,MCP/插件等外挂工具\n"
     "                  延迟挂载,模型用 tool_search 检索后方可调用)\n"
     "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
     "  /resume 编号或id  载入该场存档历史续聊(编号按本目录列表数)\n"
     "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
     "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
     "  /soul           看当前魂;/soul 内容 写进 SOUL.md 并即时生效,/soul clear 清空还原默认\n"
     "                  /soul 名字 仍可切换已有备选魂,/soul off 关,/soul default 回 SOUL.md\n"
     "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
     "  /image 路径     附本地图片(也可在消息里写 @路径；支持 png/jpg/jpeg/gif/webp，每张不超过 5MB)\n"
     "  Shift+Enter     输入框里插一个换行,写多行消息(Alt+Enter 同义;注意 Windows Terminal\n"
     "                  默认把 Alt+Enter 绑成全屏切换、会吞掉这个键,用 Shift+Enter 最稳);\n"
     "                  Enter 把整段(多行拼换行)一次发出,空白内容按 Enter 原地不动\n"
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
     "  流式期间打字回车  不会打断当前流,而是排进队列,本轮结束后按顺序自动发出\n"
     "  /exit           退出(裸词 exit/quit 也认)\n"},
    {"help.config",
     "配置优先级(从高到低,按字段逐个决,不是整套配置一刀切):\n"
     "  1) LUBANCODE_ 专属环境变量\n"
     "       LUBANCODE_WIRE          协议选择,anthropic 或 responses\n"
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
     "  /config         打印当前生效配置(api_key 打码),外加本会话实际在用的 model\n"
     "  /language       列可选界面语言并切换;/language 语言码 直接切(会话级,可写回配置)\n"
     "  /worktree       新建/列出/退出隔离工作树;/worktree new [名字] | list | exit keep|remove\n"
     "  /clear          清空对话历史\n"
     "  /context        看当前上下文占用;/context 256k|512k|1m 临时改窗口大小\n"
     "  /compact        手动压缩历史;/compact 重点说明 可指定这次额外保留什么\n"
     "  /think          看当前推理强度;/think 档位 切档位,档位以服务商为准(/effort 同义)\n"
     "  /skills         列出扫描到的技能(主目录级 + 项目级)\n"
     "  /skill list     列本机技能,标本地/远端来源;/skill install <url> 安装,/skill update [名字] 更新,\n"
     "                  /skill remove <名字> 删除主目录级远端技能\n"
     "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
     "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
     "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
     "  /plugins        列出挂载的插件工具(DLL + lua)和加载警告\n"
     "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(tool_search 延迟挂载)\n"
     "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
     "  /resume 编号或id  载入该场存档历史续聊(编号按本目录列表数),后续消息追加写回同一文件\n"
     "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
     "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
     "  /soul           看当前魂;/soul 内容 写进 SOUL.md 并即时生效,/soul clear 清空还原默认\n"
     "                  /soul 名字 仍可切换已有备选魂,/soul off 关,/soul default 回 SOUL.md\n"
     "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
     "  /image 路径     附本地图片(也可在消息里写 @路径；支持 png/jpg/jpeg/gif/webp，每张不超过 5MB)\n"
     "  /exit           退出(裸词 exit/quit 也认)\n"
     "多行输入:Shift+Enter 插换行(Alt+Enter 同义,但 Windows Terminal 默认把它绑成全屏\n"
     "切换、会吞掉,推荐 Shift+Enter);Enter 发送整段;多行时首行的 / 是正文,不当命令。\n"
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
    {"stream.hint", "\xe2\x8e\x8b 打断  ·  键入并回车 排队下一条"},
    {"stream.hint.plain", "ESC 打断 · 键入排队"},
    {"stream.queueing", "\xe2\x8e\x8b 打断  ·  排队中: "},
    {"stream.queueing.plain", "ESC 打断 · 排队中: "},
    {"spinner.thinking", "思考中"},

    // ---- 子代理状态条(#52) ----
    {"agent_status.running", "{0} · 运行中 · {1} · 已调用 {2} 次工具"},
    {"agent_status.done_ok", "{0} · 完成 · {1} · {2} 次工具"},
    {"agent_status.done_error", "{0} · 失败 · {1} · {2} 次工具"},

    // ---- 确认提示 ----
    {"confirm.prompt", "[y] 本次允许  [a] 本会话总是允许(该工具)  [N] 拒绝: "},
    {"confirm.detail.path", "    路径: {0}"},
    {"confirm.detail.replace_all", "  (replace_all=true,全部替换)"},
    {"confirm.detail.content", "    内容({0} 字节),前几行:"},
    {"confirm.detail.old", "    - 旧文本:"},
    {"confirm.detail.new", "    + 新文本:"},
    {"confirm.detail.command", "    命令({0}): {1}"},
    {"confirm.detail.args", "    参数: {0}"},
    {"confirm.detail.omitted", "      ...(共 {0} 行,已省略其余)"},

    // ---- 向导 ----
    {"wizard.title", "=== lubancode 初次配置向导 ==="},
    {"wizard.subtitle", "(base_url / api_key 没读到,先配一遍,配完直接进入会话)"},
    {"wizard.lang.title", "界面语言 / Language:"},
    {"wizard.lang.prompt", "选择 / Select [{0}]: "},
    {"wizard.wire.title", "接口格式:"},
    {"wizard.wire.opt1", "  1) anthropic (Claude 系)"},
    {"wizard.wire.opt2", "  2) responses (OpenAI 系)"},
    {"wizard.choose_prompt", "选择 [1]: "},
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

    // ---- slash 命令描述表 ----
    {"slash.desc.help", "列出所有命令"},
    {"slash.desc.model", "拉模型列表选,或 /model 名字 直接切"},
    {"slash.desc.provider", "列、添、切、删模型服务端;/provider add|list|switch|remove"},
    {"slash.desc.config", "打印当前生效配置和本会话在用的 model"},
    {"slash.desc.language", "列可选界面语言并切换;/language 语言码 直接切"},
    {"slash.desc.image", "附本地图片;/image 路径 或在消息里写 @路径"},
    {"slash.desc.worktree", "新建、列出或退出隔离工作树;/worktree new [名字] | list | exit keep|remove"},
    {"slash.desc.clear", "清空对话历史"},
    {"slash.desc.exit", "退出(裸词 exit/quit 也认)"},
    {"slash.desc.context", "看当前上下文占用;/context 256k|512k|1m 临时改窗口大小"},
    {"slash.desc.compact", "手动压缩历史;/compact 重点说明 可指定这次额外保留什么"},
    {"slash.desc.think", "看当前推理强度;/think 档位 切档位,档位以服务商为准(/effort 同义)"},
    {"slash.desc.effort", "同 /think(推理强度别名)"},
    {"slash.desc.skills", "列出扫描到的技能(主目录级 + 项目级)"},
    {"slash.desc.skill", "安装、查看、更新或删除主目录里的远端技能"},
    {"slash.desc.mcp", "列出挂载的 MCP 服务器状态和工具清单"},
    {"slash.desc.lsp", "列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)"},
    {"slash.desc.todos", "查看当前待办清单"},
    {"slash.desc.plugins", "列出挂载的插件工具(DLL + lua)和加载警告"},
    {"slash.desc.tools", "列工具三态:核心(恒在)/已加载/延迟未加载(tool_search 延迟挂载)"},
    {"slash.desc.sessions", "列本目录最近 20 场会话存档,倒序编号;/sessions all 列全部目录"},
    {"slash.desc.resume", "/resume 编号或id 载入该场存档历史续聊"},
    {"slash.desc.export", "当前会话导出 Markdown;/export 路径 可指定输出文件"},
    {"slash.desc.title", "看当前会话标题;/title 标题 给本场起名,/sessions 列表和导出都用它"},
    {"slash.desc.soul", "看当前魂;/soul 内容 写进 SOUL.md,/soul clear 还原默认；名字仍可切换备选魂"},
    {"slash.desc.prompt", "看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md"},

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

    // ---- /config 诊断 ----
    {"config.header", "lubancode 最终生效的配置:"},
    {"config.not_set", "(未设置)"},
    {"config.language.follow_system", "(未设置,跟系统: {0})"},
    {"config.compact_model.unset", "(未设置,跟会话模型一致)"},
    {"config.think.unset", "(未设置,不发这个参数)"},
    {"config.soul.unset", "(未设置,用主目录 SOUL.md)"},
    {"config.threshold.never", "(永不延迟)"},
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

    // ---- transcript 摘要词(彩色主题;plain 的 [RUNNING] 等不进表) ----
    {"transcript.pending", "待确认"},
    {"transcript.read_lines", "读取 {0} 行"},
    {"transcript.exit_code", "退出码 {0}"},
    {"transcript.added", "新增 {0} 行"},
    {"transcript.added_removed", "新增 {0} 行,删除 {1} 行"},
    {"transcript.hits", "命中 {0} 处"},
    {"transcript.agent", "子代理 {0} 轮 · {1} 次工具"},
    {"transcript.error_no_output", "Error: (无输出)"},
    {"transcript.error_exit_code", "Error: 退出码 {0}"},
    {"transcript.error_truncated", "(共 {0} 行,Ctrl+E 查看完整)"},
    {"transcript.params_prefix", "参数: "},
    {"transcript.no_full_output", "(无完整输出)"},
    {"transcript.full_output_header", "── 完整输出({0} 行)──"},
    {"transcript.todo_count", "{0} 项"},
    {"todo.empty", "没有待办。"},

    // ---- 统计行 ----
    {"stats.line", "[tokens] 输入 {0}{1} · 输出 {2} · 请求 {3} 次 · context {4}%"},
    {"stats.cache", "(缓存命中 {0})"},

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
    {"tool_search.enabled", "[tool_search] 工具超过阈值 {0},MCP/插件等外挂工具改为延迟挂载(/tools 看三态)"},
    {"catalog.warning", "[models.json 警告] {0}"},
    {"settings.local.warning", "[settings.local.json 警告] {0}"},
    {"settings.local.persist_prompt", "也永久写进项目 settings.local.json?[y/N] "},
    {"settings.local.persisted", "已永久允许 {0}(项目级)"},
    {"settings.local.persist_failed", "写 settings.local.json 失败:{0}"},

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
     "  C ABI DLL: {0}/*.dll\n"
     "      导出 luban_plugin_entry(见仓库 include/luban_plugin.h),示例在\n"
     "      examples/plugins/hello_plugin/。注意:DLL 跟宿主同进程,插件里崩了\n"
     "      整个程序一起完蛋,装谁的插件风险自担。\n"
     "  Lua:       {0}/*.lua\n"
     "      每个文件 return { name=..., description=..., input_schema=...,\n"
     "      execute=function(input) ... end } 一张表,示例在 examples/plugins/word_count.lua。"},
    {"cmd.plugins.mounted", "已挂载 {0} 个插件工具:"},
    {"cmd.plugins.warnings", "加载警告(这些没挂上):"},

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
    {"cmd.skill.usage", "用法:/skill list | install <url> | update [名字] | remove <名字>"},
    {"cmd.skill.no_home", "找不到用户主目录，远端技能没处安放。"},
    {"cmd.skill.list_empty", "这里还没有技能。用 /skill install <url> 装一份。"},
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
    // /context 裸敲的分类占用分析(拼装规则见 FormatContextBreakdown)。
    {"cmd.context.bd.header", "上下文占用分析(窗口 {0})"},
    {"cmd.context.bd.system", "系统提示"},
    {"cmd.context.bd.tools", "工具定义"},
    {"cmd.context.bd.history", "对话历史"},
    {"cmd.context.bd.used", "已用"},
    {"cmd.context.bd.threshold", "自动压缩线"},
    {"cmd.context.bd.remaining", "剩余"},
    {"cmd.context.bd.cache", "(缓存命中 {0})"},
    {"cmd.context.bd.measured", "(实测)"},
    {"cmd.context.bd.history_derived", "(=实测总量−系统−工具)"},
    {"cmd.context.bd.note.measured", "(总量为上一轮实测 token;系统提示/工具为字符估,历史为实测总量反推)"},
    {"cmd.context.bd.note.est", "(尚无实测,启动估算:token 为字符数/3 粗估,实际以模型返回为准)"},
    {"cmd.compact.empty", "当前没有对话历史,不用压缩。"},
    {"cmd.compact.failed", "压缩失败: {0}"},
    {"cmd.compact.result", "压缩前 ~{0} tokens → 压缩后 ~{1} tokens"},
    {"compact.auto_start", "[compact] 上下文接近上限,自动压缩中..."},
    {"compact.auto_done", "[compact] 自动压缩完成。"},
    {"compact.auto_failed", "[compact] 自动压缩失败: {0}"},
    {"compact.auto_failed_tail", "(继续按原历史发送,字符数安全网仍会兜底)"},
    {"cmd.think.current", "当前推理强度: {0}"},
    {"cmd.think.catalog_header", "模型目录声明的档位({0}):"},
    {"cmd.think.provider", "支持哪些档位以服务商为准。"},
    {"cmd.think.switched", "推理强度已切到 {0}(本会话生效)。"},
    {"cmd.think.undeclared", "提示: 模型目录未声明该档,仍会发送。"},

    // ---- 模型目录应用 / /model ----
    {"catalog.apply_think", "think→{0}(目录默认)"},
    {"catalog.apply_window", "上下文窗口→{0} tokens(目录声明)"},
    {"catalog.apply_instructions", "base_instructions 已注入系统提示(目录条目 {0},下一轮请求生效)"},
    {"cmd.model.fetch_failed", "拉取模型列表失败: {0}"},
    {"cmd.model.list_empty", "接口返回的模型列表是空的。"},
    {"cmd.model.choose", "选择模型编号 [1]: "},
    {"cmd.model.bad_number", "编号不对,取消切换。"},
    {"cmd.model.not_number", "没听懂,取消切换。"},
    {"cmd.model.switched", "已切换到模型: {0}(本会话生效)"},
    {"cmd.write_config_prompt", "写进配置文件 {0}? [y/N]: "},
    {"cmd.write_config.updated", "已更新 {0}"},
    {"cmd.write_config.failed", "更新失败: {0}"},
    {"cmd.session_only", "当前没有生效的配置文件,只在本会话生效。"},

    // ---- /provider ----
    {"cmd.provider.usage",
     "用法:\n"
     "  /provider list\n"
     "  /provider add <名字> <base_url> <anthropic|responses> [--key-env 环境变量名] [--model 默认模型] [--window 大小]\n"
     "  /provider switch <名字> [模型]\n"
     "  /provider remove <名字>"},
    {"cmd.provider.empty", "还没有配 provider。用 /provider add 添一个。"},
    {"cmd.provider.header", "已配 provider:"},
    {"cmd.provider.line", "  - {0} [{1}] {2}; model={3}; window={4}; key_env={5}{6}"},
    {"cmd.provider.current", " (当前)"},
    {"cmd.provider.model_unset", "(未设置)"},
    {"cmd.provider.added", "已添 provider {0},写进全局配置 {1}。"},
    {"cmd.provider.add_failed", "添 provider 失败: {0}"},
    {"cmd.provider.exists", "provider 已存在: {0}"},
    {"cmd.provider.switched", "已切到 provider {0},后续请求走 {1}。"},
    {"cmd.provider.not_found", "找不着 provider: {0}"},
    {"cmd.provider.key_missing", "provider {0} 要环境变量 {1},眼下没取到值。"},
    {"cmd.provider.removed", "已删 provider {0},全局配置在 {1}。"},
    {"cmd.provider.remove_active", "provider {0} 正在用，先切到别处再删。"},
    {"cmd.provider.remove_failed", "删 provider 失败: {0}"},

    // ---- /soul、/prompt ----
    {"soul.unavailable", "[soul] 无法读取 {0},已按无魂运行。"},
    {"cmd.soul.no_home", "找不到用户主目录,魂文件没处安身,/soul 用不了。"},
    {"cmd.soul.available_header", "可选旧魂(输入名字切换):"},
    {"cmd.soul.default_item", "  - default(主目录 SOUL.md)"},
    {"cmd.soul.current", "当前生效: {0}"},
    {"cmd.soul.empty_note", "(内容空白,无效果)"},
    {"cmd.soul.usage", "用法:/soul 看当前;/soul 内容 写进 SOUL.md;/soul clear 还原默认。"},
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
    {"cmd.resume.usage", "用法:/resume 编号或id(/sessions 看列表)"},
    {"cmd.resume.none", "本目录还没有会话存档,没什么可恢复(/sessions all 看全部目录)。"},
    {"cmd.resume.out_of_range", "编号 {0} 超出范围(本目录现有 {1} 场,/sessions 看列表)。"},
    {"cmd.resume.read_failed", "读不到存档 {0}。"},
    {"cmd.resume.bad_meta", "存档 {0} 首行不是合法 meta,认不得这个格式。"},
    {"cmd.resume.takeover_failed", "[会话存档] 接管 {0} 失败,恢复的历史只在内存里,本场不再落盘。"},
    {"cmd.resume.restored_compact", "已恢复 {0},有效 {1} 条(全量 {2} 条,经 {3} 次压缩)"},
    {"cmd.resume.restored", "已恢复 {0},{1} 条消息"},
    {"cmd.resume.repaired", "(补了 {0} 条缺失的工具结果)"},
    {"cmd.resume.skipped", "(跳过 {0} 行解析不动的存档)"},
    {"cmd.resume.estimate", "上下文占用(按字符粗估): ~{0} tokens,首轮请求后以真实用量为准。"},
    {"cmd.resume.model_mismatch", "[提醒] 存档时用的 model 是 {0},当前是 {1},继续聊没问题,风格可能有差。"},
    {"cmd.resume.wire_mismatch", "[提醒] 存档时用的 wire 是 {0},当前是 {1}。"},
    {"cmd.export.empty", "当前会话还没有内容,没什么可导出。"},
    {"cmd.export.need_path", "找不到用户主目录,请显式给个路径:/export 路径"},
    {"cmd.export.write_failed", "写不进 {0}。"},
    {"cmd.export.done", "已导出 Markdown: {0}"},
    {"cmd.title.none", "本场还没设标题(/title 标题 起一个)。"},
    {"cmd.title.current", "当前标题: {0}"},
    {"cmd.title.set", "标题已设为: {0}"},
    {"cmd.title.set_pending", "标题已设为: {0}(首条消息落盘后写入存档)"},
    {"cmd.title.write_failed", "[会话存档] 标题写入失败,只在本次会话内存里生效。"},
    {"cmd.clear.done", "已清空对话历史。"},
    {"session.create_failed", "[会话存档] 在 {0} 建档失败,本场对话不落盘(不影响继续聊)。"},
    {"session.append_failed", "[会话存档] 追加写入失败,后续不再落盘(不影响继续聊)。"},
    {"session.compact_event_failed", "[会话存档] 存档事件写盘失败,/resume 将回放到压缩前状态。"},

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
     "                              an empty line just re-prompts\n"},
    {"help.options",
     "Options:\n"
     "  --version              print the version\n"
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
     "  /provider       list configured providers; /provider add|switch|remove manages endpoints\n"
     "  /config         print the effective configuration plus the model in use this session\n"
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
     "  /skills         list discovered skills (home-level + project-level)\n"
     "  /skill list     list local skills and their source; /skill install <url> installs, /skill update [name]\n"
     "                  updates, and /skill remove <name> removes a home-level remote skill\n"
     "  /mcp            list mounted MCP servers and their tools\n"
     "  /lsp            list LSP server status per language\n"
     "  /todos          show the current todo list (maintained by the todo_write tool)\n"
     "  /plugins        list mounted plugin tools (*.dll and *.lua under .lubancode/plugins)\n"
     "  /tools          list tool states: core / loaded / deferred (tool_search deferral kicks in\n"
     "                  when the tool count exceeds tool_search_threshold, default 20, 0 = never)\n"
     "  /sessions       list the 20 most recent session archives of this directory; /sessions all\n"
     "                  lists every directory\n"
     "  /resume <n|id>  load a session archive and continue chatting\n"
     "  /export [path]  export the current session as Markdown (default sessions/<id>.md)\n"
     "  /title [title]  show/set the session title, used by /sessions and /export\n"
     "  /soul           show the current soul; /soul <text> writes SOUL.md and takes effect now;\n"
     "                  /soul clear restores its default; an existing soul name still switches it\n"
     "  /prompt         show the source and length of the current system prompt persona;\n"
     "                  /prompt reset restores system_prompt.md\n"
     "  /image <path>   attach a local image (or use @path in a message; png/jpg/jpeg/gif/webp, 5MB each)\n"
     "  Shift+Enter     insert a newline in the input box (Alt+Enter works too, but Windows Terminal\n"
     "                  binds it to fullscreen by default; Shift+Enter is safest); Enter sends the\n"
     "                  whole message; Enter on blank input does nothing\n"
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
     "  typing+Enter during streaming  does not interrupt the stream; the line is queued and sent\n"
     "                  in order after this turn ends\n"
     "  /exit           quit (bare exit/quit work too)\n"},
    {"help.config",
     "Configuration priority (high to low, decided per field, not as a whole):\n"
     "  1) LUBANCODE_ dedicated environment variables\n"
     "       LUBANCODE_WIRE          protocol, anthropic or responses\n"
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
     "  /config         print the effective configuration (api_key masked) plus the session model\n"
     "  /language       list available UI languages and switch; /language <code> switches directly\n"
     "  /worktree       create, list, or leave isolated trees; /worktree new [name] | list | exit keep|remove\n"
     "  /clear          clear the conversation history\n"
     "  /context        show context usage; /context 256k|512k|1m changes the window for this session\n"
     "  /compact        compact the history; /compact <note> tells what to keep extra\n"
     "  /think          show reasoning effort; /think <level> sets it (provider-defined; /effort alias)\n"
     "  /skills         list discovered skills (home-level + project-level)\n"
     "  /skill list     list local skills and their source; /skill install <url> installs, /skill update [name]\n"
     "                  updates, and /skill remove <name> removes a home-level remote skill\n"
     "  /mcp            list mounted MCP servers and their tools\n"
     "  /lsp            list LSP server status per language\n"
     "  /todos          show the current todo list\n"
     "  /plugins        list mounted plugin tools (DLL + lua) and load warnings\n"
     "  /tools          list tool states: core / loaded / deferred (tool_search)\n"
     "  /sessions       list the 20 most recent session archives here; /sessions all for every dir\n"
     "  /resume <n|id>  load a session archive and continue; new messages append to the same file\n"
     "  /export [path]  export this session as Markdown (default sessions/<id>.md)\n"
     "  /title [title]  show/set the session title, used by /sessions and /export\n"
     "  /soul           show the current soul; /soul <text> writes SOUL.md and takes effect now;\n"
     "                  /soul clear restores default; an existing soul name still switches it\n"
     "  /prompt         show the persona source and length; /prompt reset restores system_prompt.md\n"
     "  /exit           quit (bare exit/quit work too)\n"
     "Multi-line input: Shift+Enter inserts a newline (Alt+Enter too, but Windows Terminal may\n"
     "swallow it; Shift+Enter is recommended); Enter sends the whole message; on multi-line input\n"
     "a leading / is treated as text, not a command.\n"
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
    {"stream.hint", "\xe2\x8e\x8b interrupt  \xc2\xb7  type + Enter to queue next"},
    {"stream.hint.plain", "ESC interrupt \xc2\xb7 type to queue next"},
    {"stream.queueing", "\xe2\x8e\x8b interrupt  \xc2\xb7  queueing: "},
    {"stream.queueing.plain", "ESC interrupt \xc2\xb7 queueing: "},
    {"spinner.thinking", "thinking"},

    // ---- subagent status board (#52) ----
    {"agent_status.running", "{0} · running · {1} · {2} tool calls"},
    {"agent_status.done_ok", "{0} · done · {1} · {2} tool calls"},
    {"agent_status.done_error", "{0} · failed · {1} · {2} tool calls"},

    // ---- confirm prompt ----
    {"confirm.prompt", "[y] allow once  [a] always allow this tool (this session)  [N] deny: "},
    {"confirm.detail.path", "    path: {0}"},
    {"confirm.detail.replace_all", "  (replace_all=true)"},
    {"confirm.detail.content", "    content ({0} bytes), first lines:"},
    {"confirm.detail.old", "    - old text:"},
    {"confirm.detail.new", "    + new text:"},
    {"confirm.detail.command", "    command ({0}): {1}"},
    {"confirm.detail.args", "    args: {0}"},
    {"confirm.detail.omitted", "      ... ({0} lines total, rest omitted)"},

    // ---- wizard ----
    {"wizard.title", "=== lubancode initial setup wizard ==="},
    {"wizard.subtitle", "(base_url / api_key not found; configure once, then the session starts)"},
    {"wizard.lang.title", "界面语言 / Language:"},
    {"wizard.lang.prompt", "选择 / Select [{0}]: "},
    {"wizard.wire.title", "Wire protocol:"},
    {"wizard.wire.opt1", "  1) anthropic (Claude-style)"},
    {"wizard.wire.opt2", "  2) responses (OpenAI-style)"},
    {"wizard.choose_prompt", "Select [1]: "},
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

    // ---- slash command descriptions ----
    {"slash.desc.help", "list all commands"},
    {"slash.desc.model", "pick from the model list, or /model <name> to switch directly"},
    {"slash.desc.provider", "list, add, switch, or remove model providers; /provider add|list|switch|remove"},
    {"slash.desc.config", "print the effective configuration and the session model"},
    {"slash.desc.language", "list available UI languages and switch; /language <code> switches directly"},
    {"slash.desc.image", "attach local images; /image <path> or @path in a message"},
    {"slash.desc.worktree", "create, list, or leave isolated worktrees; /worktree new [name] | list | exit keep|remove"},
    {"slash.desc.clear", "clear the conversation history"},
    {"slash.desc.exit", "quit (bare exit/quit work too)"},
    {"slash.desc.context", "show context usage; /context 256k|512k|1m changes the window temporarily"},
    {"slash.desc.compact", "compact the history; /compact <note> tells what to keep extra"},
    {"slash.desc.think", "show/set reasoning effort; levels are provider-defined (/effort alias)"},
    {"slash.desc.effort", "same as /think (reasoning effort alias)"},
    {"slash.desc.skills", "list discovered skills (home-level + project-level)"},
    {"slash.desc.skill", "install, list, update, or remove remote skills in the home directory"},
    {"slash.desc.mcp", "list mounted MCP servers and their tools"},
    {"slash.desc.lsp", "list LSP server status per language"},
    {"slash.desc.todos", "show the current todo list"},
    {"slash.desc.plugins", "list mounted plugin tools (DLL + lua) and load warnings"},
    {"slash.desc.tools", "list tool states: core / loaded / deferred (tool_search)"},
    {"slash.desc.sessions", "list the 20 most recent session archives here; /sessions all for every dir"},
    {"slash.desc.resume", "/resume <n|id> loads a session archive and continues"},
    {"slash.desc.export", "export this session as Markdown; /export <path> picks the output file"},
    {"slash.desc.title", "show the session title; /title <title> names this session"},
    {"slash.desc.soul", "show the current soul; /soul <text> writes SOUL.md; /soul clear restores default"},
    {"slash.desc.prompt", "show the persona source/length; /prompt reset restores system_prompt.md"},

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
    {"cmd.soul.usage", "Usage: /soul shows it; /soul <text> writes SOUL.md; /soul clear restores default."},
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

    // ---- /config diagnostics ----
    {"config.header", "Effective lubancode configuration:"},
    {"config.not_set", "(not set)"},
    {"config.language.follow_system", "(not set, following system: {0})"},
    {"config.compact_model.unset", "(not set; uses the session model)"},
    {"config.think.unset", "(not set; parameter not sent)"},
    {"config.soul.unset", "(not set; uses SOUL.md in the home dir)"},
    {"config.threshold.never", "(never defer)"},
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

    // ---- transcript summary words ----
    {"transcript.pending", "Awaiting confirmation"},
    {"transcript.read_lines", "Read {0} lines"},
    {"transcript.exit_code", "exit code {0}"},
    {"transcript.added", "Added {0} lines"},
    {"transcript.added_removed", "Added {0} lines, removed {1} lines"},
    {"transcript.hits", "{0} matches"},
    {"transcript.agent", "Subagent {0} rounds · {1} tool calls"},
    {"transcript.error_no_output", "Error: (no output)"},
    {"transcript.error_exit_code", "Error: exit code {0}"},
    {"transcript.error_truncated", "({0} lines total, Ctrl+E for the full output)"},
    {"transcript.params_prefix", "args: "},
    {"transcript.no_full_output", "(no full output)"},
    {"transcript.full_output_header", "── full output ({0} lines) ──"},
    {"transcript.todo_count", "{0} items"},
    {"todo.empty", "No todos."},

    // ---- stats line ----
    {"stats.line", "[tokens] in {0}{1} · out {2} · {3} requests · context {4}%"},
    {"stats.cache", " (cache hit {0})"},

    // ---- pipe mode stable output ----
    {"pipe.tool_start", "[tool] "},
    {"pipe.tool_done", "[tool done] "},
    {"pipe.subtool_start", "  [subagent tool] "},
    {"pipe.todo_updated", "todo list updated ({0} items)"},

    // ---- /skill ----
    {"cmd.skill.usage", "Usage: /skill list | install <url> | update [name] | remove <name>"},
    {"cmd.skill.no_home", "Cannot find the home directory; nowhere to store remote skills."},
    {"cmd.skill.list_empty", "There are no skills here yet. Run /skill install <url> to add one."},
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
     "  /provider add <name> <base_url> <anthropic|responses> [--key-env ENV] [--model MODEL] [--window SIZE]\n"
     "  /provider switch <name> [model]\n"
     "  /provider remove <name>"},
    {"cmd.provider.empty", "No providers are configured. Use /provider add to add one."},
    {"cmd.provider.header", "Configured providers:"},
    {"cmd.provider.line", "  - {0} [{1}] {2}; model={3}; window={4}; key_env={5}{6}"},
    {"cmd.provider.current", " (current)"},
    {"cmd.provider.model_unset", "(not set)"},
    {"cmd.provider.added", "Added provider {0} and saved it to global config {1}."},
    {"cmd.provider.add_failed", "Could not add provider: {0}"},
    {"cmd.provider.exists", "Provider already exists: {0}"},
    {"cmd.provider.switched", "Switched to provider {0}; later requests use {1}."},
    {"cmd.provider.not_found", "Provider not found: {0}"},
    {"cmd.provider.key_missing", "Provider {0} needs environment variable {1}, but it is not set."},
    {"cmd.provider.removed", "Removed provider {0}; global config is {1}."},
    {"cmd.provider.remove_active", "Provider {0} is in use. Switch away before removing it."},
    {"cmd.provider.remove_failed", "Could not remove provider: {0}"},

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
