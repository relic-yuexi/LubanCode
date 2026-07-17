# 提示词模块(编译期嵌入)

这个目录是系统提示词的**源码**。没有外部拼装脚本,没有生成产物落在这儿——
构建时 `cmake/embed_prompts.cmake` 把各 .md 读进
`<build>/generated/embedded_prompts.hpp`(每个模块一个 `constexpr` 字符串常量),
运行时由 `src/agent/prompt_assembler.cpp` 的 `AssembleSystemPrompt()` 按条件拼装。
改任何一个 .md,重新构建即生效,不用重新跑 cmake configure。

## 目录与拼装规则

| 目录 | 嵌入常量 | 注入条件 |
| --- | --- | --- |
| `core/` | `kCore_*`(按文件名排序) | 恒在,合起来就是内置默认人格(法);`~/.lubancode/system_prompt.md` 或 `--system-prompt` 非空时整段让位 |
| `features/files.md` `shell.md` `delegation.md` `todo.md` | `kFeature_*` | 恒在 |
| `features/skills.md` | `kFeature_skills` | 扫到技能(skills 段非空)才注,后面紧跟技能清单 |
| `features/web.md` | `kFeature_web` | 配置了 `search` 段(web_search 注册了)才注 |
| `features/mcp.md` | `kFeature_mcp` | 配置了 `mcpServers` 才注 |
| `features/lsp.md` | `kFeature_lsp` | 配置了 `lsp` 段才注 |
| `platforms/` | `kPlatform_*` | 按 wire 注一个:`anthropic.md` 或 `responses.md` |
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
