# 提示词模块(编译期嵌入 + 运行时用户覆盖)

这个目录是系统提示词的**源码**。没有外部拼装脚本,没有生成产物落在这儿——
构建时 `cmake/embed_prompts.cmake` 把各 .md 读进
`<build>/generated/embedded_prompts.hpp`(每个模块一个 `constexpr` 字符串常量,
外加 `kAllModules` 的 `{相对路径, 正文}` 总表),
运行时由 `src/agent/prompt_assembler.cpp` 的 `AssembleSystemPrompt()` 按条件拼装。
改任何一个 .md,重新构建即生效,不用重新跑 cmake configure。

## 运行时用户覆盖(0.21.x)

嵌入版的角色是**默认值、播种源、回退源**。每次启动,`EnsurePromptScaffold`
把全部模块播种到 `~/.lubancode/prompts/{core,features,platforms}/*.md`
(缺哪补哪,已存在的绝不覆盖);拼装时(启动构建 AgentLoop、每次 /clear
重建、子代理每次启动)逐模块现读现拼:用户文件存在且剥空白后非空就用文件,
否则回退嵌入常量。用户改了模块,开新会话即生效,不用重编不用重启;删掉
文件或清空内容即回退内置。`/prompt` 裸敲能看各模块来源(用户文件/内置)
统计。注意:法(`~/.lubancode/system_prompt.md`)被用户改出内容差异时仍
整段替换 core 模块;播种原样未改的法文件视同"内置默认",core 走运行时模块。

## Prompt Profile(阶段 2)

自定义 Agent 可在 YAML 里 `prompt.profile: <名>` 点名一组稀疏覆盖,详见
`profiles/README.md` 与契约 `docs/reference/agents.md` §6。同一模块按五层找,
越往后权越大:内置 default → 用户全局 default 覆盖(`~/.lubancode/prompts/`)
→ 内置选中 Profile(`src/prompts/profiles/<名>/`)→ 用户选中 Profile
(`~/.lubancode/prompts/profiles/<名>/`)→ 项目选中 Profile
(`<项目根>/.lubancode/prompts/profiles/<名>/`)。没点名的模块原样走默认;
删掉覆盖文件即稳退下一层;`/agent inspect <名字>` 逐段列来源账
(PromptSourceLedger)。default 上下文(主 Agent)不走 Profile 层,黄金输出
一字不变。

## 目录与拼装规则

| 目录 | 嵌入常量 | 注入条件 |
| --- | --- | --- |
| `core/` | `kCore_*`(按文件名排序) | 恒在,合起来就是内置默认人格(法);`~/.lubancode/system_prompt.md` 或 `--system-prompt` 非空时整段让位 |
| `features/files.md` `shell.md` `delegation.md` `todo.md` | `kFeature_*` | 恒在 |
| `features/skills.md` | `kFeature_skills` | 扫到技能(skills 段非空)才注,后面紧跟技能清单 |
| `features/web.md` | `kFeature_web` | 配置了 `search` 段(web_search 注册了)才注 |
| `features/mcp.md` | `kFeature_mcp` | 配置了 `mcpServers` 才注 |
| `features/lsp.md` | `kFeature_lsp` | 配置了 `lsp` 段才注 |
| `platforms/` | `kPlatform_*` | 按 wire 注一个:`anthropic.md`、`responses.md` 或 `chat_completions.md` |
| `models/` | 不嵌入 | 模型专属指令走 `models.json` 的 `base_instructions`,见 `models/README.md` |

拼装顺序:人格(core 或法)→ 运行环境段(工作目录、当天日期、操作系统,运行时现填)
→ features → platform。模型专属指令、魂(SOUL.md)、延迟工具索引仍由
`prompts.hpp` 的 `With*` 系列在包装层往后叠,顺序不变。

## 加模块 / 改模块

- 改内容:直接改 .md,重新构建(`cmake --build`)即可,自定义命令按文件时间戳触发重生成。
- 加文件:core/、features/、platforms/ 下新建 .md 会被自动嵌入(README.md 除外;
  依赖清单靠 `file(GLOB ... CONFIGURE_DEPENDS)`,新文件会触发自动重新 configure)。
  但 features/platforms 的新常量要有人用——在 `prompt_assembler.cpp` 里接上注入条件才会进提示词。
- 常量名由路径算:`core/10-identity.md` → `kCore_10_identity`,
  `features/files.md` → `kFeature_files`,`platforms/anthropic.md` → `kPlatform_anthropic`。
- 模块正文别出现 `)LUBAN_MD"` 这串字符(raw string 定界符),嵌入脚本查到会直接报错。

## 边界

- 运行时数据(日期、目录、技能清单、模型指令)不写进模块,拼装时现填。
- 工具的参数说明走 API 的工具 schema,不抄进提示词。
- 平台模块只写协议层面的、验证过的差异;一个模型才有的怪癖走 `models.json`。
