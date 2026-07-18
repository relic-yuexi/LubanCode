#include "config/prompt_files.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
// CreateFileW(CREATE_NEW) 排他创建脚手架文件用。max/min 宏与 <algorithm>
// 撞车的老坑,照例 NOMINMAX 关掉(跟 console_input.cpp 一套写法)。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lubancode::config {

namespace {

namespace fs = std::filesystem;

constexpr const char* kLubancodeConfigSkillContent = R"SKILL(---
name: lubancode-config
description: lubancode 自身配置与功能说明(soul/魂/主题/模型/MCP/钩子/技能/语言/快捷键/会话),用户问及或要改 lubancode 自身时先加载本技能
---
<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->

# lubancode 配置手册

用户问起或要改 lubancode 自身(配置、soul、主题、模型、MCP、技能、会话……),先看这份手册。改配置先分清放在哪，再只改眼下要改的字段。`config.json` 顶层须是 JSON object；字段可只写一部分，缺的往下一级找。

## 配置文件与优先级

`config.json` 的查找次序如下：

1. 当前项目 `.lubancode/config.json`
2. 主目录 `~/.lubancode/config.json`
3. 当前项目旧位置 `.lubancode.json`
4. 主目录旧位置 `~/.lubancode.json`

命中旧位置，且对应新位置尚无文件时，lubancode 会尝试迁移过去。

字段按四级逐个决，不是整份配置谁盖谁：

1. `LUBANCODE_*` 专属环境变量。
2. 找到的 `config.json`。
3. 通用环境变量。`wire=anthropic` 读 `ANTHROPIC_BASE_URL`、`ANTHROPIC_AUTH_TOKEN`、`ANTHROPIC_MODEL`；`wire=responses` 读 `OPENAI_BASE_URL`、`OPENAI_API_KEY`、`OPENAI_MODEL`。
4. 内置默认值。

`hooks`、`mcpServers`、`search`、`lsp` 只从配置文件读。`tool_search_threshold` 只从配置文件或内置默认值来。其余字段依上面四级找；没有通用环境变量的字段，直接略过第三级。

## config.json 字段

| 字段 | 类型与取值 | 默认值 | 用处 |
| --- | --- | --- |
| `wire` | 字符串：`anthropic` 或 `responses` | `anthropic` | 选 Anthropic Messages API 或 OpenAI Responses API。 |
| `base_url` | 字符串 | 无 | 模型服务根地址。 |
| `api_key` | 字符串 | 无 | 模型服务认证值。不要把真实值写进仓库。 |
| `model` | 字符串 | 无 | 发请求所用模型名。 |
| `theme` | 字符串：`dark`、`light`、`plain` | `dark` | 终端配色；管道或重定向时会降为 `plain`。 |
| `think` | 字符串：`none`、`low`、`medium`、`high`，也可留空 | 空串 | 推理强度；空串时不向请求带推理参数。 |
| `soul` | 字符串：空串、`default`、`off`，或 `souls/` 下文件名（不带 `.md`） | 空串 | 选风格叠加层。空串和 `default` 读 `SOUL.md`；`off` 不叠加。 |
| `context_window` | 字符串或整数；正数，认 `256k`、`512k`、`1m` 或裸数字 | `256000` | 会话上下文窗口，单位 token。`k=1000`，`m=1000000`。 |
| `compact_model` | 字符串，可留空 | 空串 | `/compact` 专用模型；空串时沿用会话模型。 |
| `max_context_chars` | 正整数 | `600000` | 旧的按字符硬切安全网，和 `context_window` 不是一回事。 |
| `system_prompt_file` | 字符串，UTF-8 文本路径 | 无 | 人格段文件。没配时用内置人格；`--system-prompt` 会压过它。 |
| `tool_search_threshold` | 非负整数 | `20` | 注册工具总数超过此数才启用延迟挂载；`0` 永不延迟。 |
| `hooks` | JSON object | 四类数组都空 | 外部命令钩子，详见下节。 |
| `mcpServers` | JSON object | 空 object | MCP stdio 服务器表，详见下节。 |
| `search` | JSON object | 未配置 | 搜索服务；不写时 `web_search` 工具不注册。 |
| `lsp` | JSON object | 空 object | 语言服务器表；不写时 `lsp` 工具不注册。 |

`base_url`、`api_key`、`model` 没有内置值。交互模式会走初次配置向导；单发和管道模式会报缺项。

### hooks

`hooks` 可有 `pre_tool`、`post_tool`、`session_start`、`session_end` 四个数组。每一项都要有字符串 `command`。命令交给 `cmd.exe` 执行。

`pre_tool` 和 `post_tool` 可再写字符串 `matcher`：精确工具名，或 `"*"` 匹配全部工具；省略或空串也当 `"*"`。`session_start` 与 `session_end` 不看 `matcher`。

```json
{
  "hooks": {
    "pre_tool": [
      { "matcher": "write_file", "command": "echo about to write" }
    ],
    "post_tool": [],
    "session_start": [
      { "command": "echo session started" }
    ],
    "session_end": []
  }
}
```

### mcpServers

`mcpServers` 的键是服务器名，值是服务器设置。`command` 必填，为字符串；`args` 可选，为字符串数组；`env` 可选，为字符串到字符串的 object。

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": { "MCP_MODE": "stdio" }
    }
  }
}
```

### search

`search` 必须同时有 `provider` 与非空字符串 `api_key`。`provider` 只认 `tavily`、`brave`、`serper`。真实密钥只填在本机配置，别进版本库。

```json
{
  "search": {
    "provider": "tavily",
    "api_key": "<在本机填入搜索服务密钥>"
  }
}
```

### lsp

`lsp` 的键是语言名，同时作 LSP 的 `languageId`。每项 `command` 必填且非空；`args` 可选，值为字符串数组；`extensions` 必填，为非空字符串数组；`idle_minutes` 可选，正整数，缺省 `10`。

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".c", ".cc", ".cpp", ".h", ".hpp"],
      "idle_minutes": 10
    }
  }
}
```

## LUBANCODE_* 环境变量

| 环境变量 | 对应字段 | 值 |
| --- | --- | --- |
| `LUBANCODE_WIRE` | `wire` | `anthropic` 或 `responses`。 |
| `LUBANCODE_BASE_URL` | `base_url` | 模型服务根地址。 |
| `LUBANCODE_API_KEY` | `api_key` | 模型服务认证值。 |
| `LUBANCODE_MODEL` | `model` | 模型名。 |
| `LUBANCODE_MAX_CONTEXT` | `max_context_chars` | 正整数；无效或不大于零时当作没设。 |
| `LUBANCODE_THEME` | `theme` | `dark`、`light` 或 `plain`。 |
| `LUBANCODE_SYSTEM_PROMPT_FILE` | `system_prompt_file` | UTF-8 人格文件路径。 |
| `LUBANCODE_CONTEXT_WINDOW` | `context_window` | `256k`、`512k`、`1m` 或正整数。 |
| `LUBANCODE_COMPACT_MODEL` | `compact_model` | 压缩模型名；空值视作没设。 |
| `LUBANCODE_THINK` | `think` | `none`、`low`、`medium` 或 `high`。 |
| `LUBANCODE_SOUL` | `soul` | `default`、`off` 或 `souls/` 下魂名。 |
| `LUBANCODE_FORCE_COLOR` | 终端颜色开关 | 设为 `1` 时，管道或重定向也强制尝试输出颜色；它不写入 `config.json`。 |

环境变量设为空串，按没设处理。`hooks`、`mcpServers`、`search`、`lsp`、`tool_search_threshold` 没有对应的 `LUBANCODE_*` 变量。

## models.json 模型目录

模型目录放在主目录 `~/.lubancode/models.json`。文件顶层写一个 `models` 数组。每项 `slug` 必填，正是发给 API 的模型名；其余字段都可省。目录缺失、整份 JSON 损坏或某条坏掉，只告警并跳过，不拦启动。

```json
{
  "models": [
    {
      "slug": "example-model",
      "display_name": "Example Model",
      "description": "模型说明",
      "default_think": "high",
      "supported_think_levels": [
        { "effort": "none", "description": "直接回答" },
        { "effort": "high", "description": "较深推理" }
      ],
      "base_instructions": "工具调用要果断。",
      "context_window": "1m",
      "supports_parallel_tool_calls": true,
      "input_modalities": ["text"],
      "truncation_policy": "auto"
    }
  ]
}
```

`display_name` 给 `/model` 列表展示；`description` 给人看；`default_think` 是模型默认推理档；`supported_think_levels` 每项写 `effort` 与 `description`；`base_instructions` 单独注入系统提示；`context_window` 认 `k`、`m` 后缀或裸数字；后三项会解析存储，眼下不启用。用户在环境变量或 `config.json` 明写的 `think`、`context_window` 压过目录默认。

## ~/.lubancode/ 目录

```text
~/.lubancode/
  config.json                         主配置
  models.json                         模型目录，可选
  system_prompt.md                    人格段(法)，首启脚手架生成
  SOUL.md                             默认风格叠加层(魂)，首启脚手架生成
  souls/                              备选魂；首启附 wenyan.md
  prompts/                            提示词运行时模块，首启播种自内置版
    core/                             身份/干活方式/答话风格(默认人格)
    features/                         各工具方针段
    platforms/                        协议平台段
  sessions/                           会话存档
  plugins/                            DLL 与 Lua 插件
  skills/lubancode-config/SKILL.md    本配置手册，随版本自动更新
  languages/                          预留
```

`sessions/`、`plugins/`、`skills/`、`languages/` 都可按需出现。项目级的 `.lubancode/` 也能放 `config.json` 与 `skills/`；同名技能时项目级压过主目录级。

## 魂(soul):风格叠加层与 souls/ 目录

魂只影响答话的语气风格，注入在系统提示最后；工具照常调用，内容照常准确。

- 默认魂是 `~/.lubancode/SOUL.md`。它默认只有一行注释、内容空白（空白 = 无效果）；写上风格指令就生效。
- 添加一个新魂：在 `~/.lubancode/souls/` 下建 `<名字>.md`，正文写风格指令。例如加"猫娘"魂就写 `souls/catgirl.md`，正文写清语气规则（顶部可用 `<!-- 注释 -->` 写给人看的说明，注入前会剥掉）。
- 切换：交互里 `/soul <名字>`（即时生效，下一轮请求换新系统提示）；`/soul off` 本会话关魂；`/soul default` 回 SOUL.md；`/soul` 裸敲列全部可用魂。
- 持久化：配置 `soul` 字段或 `LUBANCODE_SOUL` 环境变量填魂名（不带 `.md`）。
- 切魂后历史里的旧风格回答可能带偏几轮，`/clear` 立净。

## 提示词运行时模块(prompts/)

系统提示按模块拼装。`~/.lubancode/prompts/{core,features,platforms}/*.md` 首启播种自内置版；逐模块看：用户文件存在且非空就用文件，否则回退内置。

- 改 `prompts/core/*.md` 可定制默认人格（身份/干活方式/答话风格）；改 `prompts/features/*.md` 可定制各工具方针段。
- 改完开新会话即生效（`/clear` 或重启），不用重编不用重启进程。
- 想回退内置：删掉对应文件或清空内容即可。
- `system_prompt.md`（法）被用户改过且非空时整段替换 core 模块；`/prompt` 裸敲能看法的来源与各模块来源统计，`/prompt reset` 还原法文件。

## 常用会话命令与快捷键

- `/context` 看上下文占用；`/context 512k` 临时改窗口（只本会话）。
- `/compact [重点]` 手动压缩历史成存档；占用超 80% 会自动压缩。
- `/clear` 清空历史开新会话；`/resume`、`/sessions` 续聊与列存档；`/export` 导出 Markdown。
- `/model`、`/think`、`/language`、`/soul`、`/prompt`、`/skills`、`/tools`、`/mcp`、`/lsp`、`/todos`、`/config` 各管一摊，`/help` 有全表。
- 快捷键：Ctrl+O 切换紧凑/详细；Tab/Shift+Tab 在工具条目间移焦点；Ctrl+E 聚焦查看全文;ESC 打断当前轮。

## 常见任务

### 换模型

编辑 `~/.lubancode/config.json`；只改顶层 `model`。例如：

```json
{
  "model": "example-model"
}
```

只想这一次生效，设 `LUBANCODE_MODEL`，或在交互里用 `/model`。别为了换模型把整份配置重写掉。

### 改主题

编辑同一份 `config.json`，把顶层 `theme` 改成下列一值：

```json
{
  "theme": "light"
}
```

另可设 `LUBANCODE_THEME=dark`、`light` 或 `plain`。管道输出仍会自动降为纯文本，除非另设 `LUBANCODE_FORCE_COLOR=1`。

### 挂 MCP 服务器

编辑 `~/.lubancode/config.json`，在顶层添或合并 `mcpServers`。服务器名自定，改 `command`、`args`、`env`：

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": { "MCP_MODE": "stdio" }
    }
  }
}
```

`command` 必填；不需参数或环境变量，就删去 `args` 或 `env`。

### 加 pre_tool 钩子

编辑 `~/.lubancode/config.json`，在 `hooks.pre_tool` 加一项。`matcher` 填工具精确名，或填 `"*"`：

```json
{
  "hooks": {
    "pre_tool": [
      { "matcher": "write_file", "command": "echo about to write" }
    ]
  }
}
```

`command` 是交给 `cmd.exe` 的整条命令。它会在工具之前执行，先审命令内容，再落配置。

### 配搜索服务

编辑 `~/.lubancode/config.json`，写顶层 `search.provider` 与 `search.api_key`：

```json
{
  "search": {
    "provider": "brave",
    "api_key": "<只在本机填入搜索服务密钥>"
  }
}
```

把占位文字换成你本机保存的值。provider 只能用 `tavily`、`brave`、`serper`。别把真实值贴进源码、技能、测试、截图或 Git 提交。

### 开 LSP

编辑 `~/.lubancode/config.json`，在顶层 `lsp` 里按语言名加服务器。C++ 可这样写：

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".cpp", ".hpp"],
      "idle_minutes": 10
    }
  }
}
```

改 `command` 为本机语言服务器命令，`extensions` 填它该接手的扩展名。`extensions` 不能空；`idle_minutes` 必须大于零。
)SKILL";

// UTF-8 字符串 -> fs::path,不走系统 ANSI 代码页那条错路(跟 main.cpp 的
// 插件目录、tools 层各处同一套写法)。
fs::path Utf8Path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// fs::path -> UTF-8 字符串,同上,反方向。
std::string PathToUtf8(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 整篇写入(binary + trunc)。成功 true。覆盖语义,给 /prompt reset 这类
// "明确要重写"的场景用;脚手架"不存在才建"走下面的排他创建,别混。
bool WriteWholeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

// 排他创建 + 整篇写入:检查与创建并成一步,文件已存在——包括并发进程
// 刚抢先建出来的——直接失败返回 false,绝不 trunc 覆盖。"先 exists 探
// 一眼再开写"的老路数中间有窗口期,会把并发刚落地的用户文件盖掉,脚手架
// 一律走这条。Windows 用 CreateFileW 的 CREATE_NEW(宽字符路径,不踩系统
// ANSI 代码页,跟 Utf8Path 一套约定);其余平台用 C11 fopen 的 "x" 旗
// (O_CREAT|O_EXCL 语义)。写不全/收不了尾也算失败,调用方只在 true 时
// 记账。
bool CreateNewFileWithContent(const fs::path& path, const std::string& content) {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;  // 已存在(ERROR_FILE_EXISTS)/建不了,一律当"已存在"跳过
    }
    bool ok = true;
    std::size_t offset = 0;
    while (ok && offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(content.size() - offset, static_cast<std::size_t>(1) << 20));
        DWORD written = 0;
        ok = ::WriteFile(handle, content.data() + offset, chunk, &written, nullptr) != 0 && written == chunk;
        offset += written;
    }
    if (::CloseHandle(handle) == 0) {
        ok = false;
    }
    return ok;
#else
    std::FILE* file = std::fopen(path.c_str(), "wbx");
    if (file == nullptr) {
        return false;  // 已存在/建不了,一律当"已存在"跳过
    }
    const std::size_t written = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), file);
    const bool closed = std::fclose(file) == 0;
    return written == content.size() && closed;
#endif
}

}  // namespace

std::string SystemPromptFilePath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "system_prompt.md");
}

std::string SoulFilePath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "SOUL.md");
}

std::string SoulsDirPath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "souls");
}

std::string SoulPathByName(const std::string& lubancode_dir, const std::string& name) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "souls" / Utf8Path(name + ".md"));
}

std::string DefaultSystemPromptFileContent(const std::string& default_persona) {
    return "<!-- 这是 lubancode 的系统提示词\"法\"——改它定制 lubancode 的行为;/prompt reset 一键还原为内置默认。 -->\n" +
           default_persona + "\n";
}

std::string DefaultSoulFileContent() {
    return "<!-- 这是\"魂\"——风格叠加层:在这里写风格指令(比如\"只用文言文答话\"),会注入在系统提示最后;"
           "留空 = 无效果;/soul 可切换 souls/ 目录里的其他魂。 -->\n";
}

std::string DefaultWenyanSoulFileContent() {
    return "<!-- 文言文风格示例:/soul wenyan 启用。 -->\n"
           "答话一律用文言,行文简古,如明清笔记之体。称用户为\"君\"。\n"
           "技术名词、代码、命令、文件路径皆存原文,不译。\n"
           "工具照常调用,义理为先,辞章为后。\n";
}

std::string DefaultLubancodeConfigSkillContent() {
    return kLubancodeConfigSkillContent;
}

std::vector<std::string> EnsurePromptScaffold(
    const std::string& lubancode_dir, const std::string& default_persona,
    const std::vector<std::pair<std::string, std::string>>& prompt_modules) {
    std::vector<std::string> created;
    if (lubancode_dir.empty()) {
        return created;
    }

    const fs::path base = Utf8Path(lubancode_dir);
    const fs::path souls_dir = base / "souls";
    const fs::path skills_dir = base / "skills";
    std::error_code ec;
    fs::create_directories(souls_dir, ec);  // 顺手把 base 也建了;失败就整个放弃(下面反正写不进)
    if (ec) {
        return created;
    }
    fs::create_directories(skills_dir / "lubancode-config", ec);
    if (ec) {
        return created;
    }

    // 一个文件一条:排他创建(CREATE_NEW 语义),建成且写全了才记账;已
    // 存在(含并发抢先落地的)、建失败、写失败都不吭声(运行期有回退)。
    const auto ensure_file = [&created](const fs::path& path, const std::string& content) {
        if (CreateNewFileWithContent(path, content)) {
            created.push_back(PathToUtf8(path));
        }
    };

    ensure_file(base / "system_prompt.md", DefaultSystemPromptFileContent(default_persona));
    ensure_file(base / "SOUL.md", DefaultSoulFileContent());
    ensure_file(souls_dir / "wenyan.md", DefaultWenyanSoulFileContent());

    // lubancode-config/SKILL.md:系统维护件。缺了排他新建;已存在时,带
    // 管理标记(kManagedSkillMarker)且内容不等于当前内置版 → 覆盖刷新
    // (随版本自动更新,不算新建不记账);无标记(用户自建、或改过删了
    // 标记)→ 一个字不动。
    const fs::path skill_path = skills_dir / "lubancode-config" / "SKILL.md";
    if (CreateNewFileWithContent(skill_path, kLubancodeConfigSkillContent)) {
        created.push_back(PathToUtf8(skill_path));
    } else if (const auto existing = ReadTextFileIfExists(PathToUtf8(skill_path));
               existing.has_value() && existing->find(kManagedSkillMarker) != std::string::npos &&
               *existing != kLubancodeConfigSkillContent) {
        WriteWholeFile(skill_path, kLubancodeConfigSkillContent);  // 写失败不吭声,下次启动再试
    }

    // 提示词运行时模块(0.21.x):~/.lubancode/prompts/<相对路径> 缺哪补哪,
    // 内容 = 嵌入版(调用方递来的清单);已存在的绝不覆盖——用户改过的
    // 模块就是要压过内置版,这正是运行时化的意义。
    if (!prompt_modules.empty()) {
        const fs::path prompts_dir = base / "prompts";
        for (const auto& [rel_path, content] : prompt_modules) {
            const fs::path module_path = prompts_dir / Utf8Path(rel_path);
            std::error_code dir_ec;
            fs::create_directories(module_path.parent_path(), dir_ec);
            if (dir_ec) {
                continue;
            }
            ensure_file(module_path, content + "\n");
        }
    }
    return created;
}

std::optional<std::string> ReadTextFileIfExists(const std::string& path) {
    const fs::path fs_path = Utf8Path(path);
    std::error_code ec;
    if (!fs::exists(fs_path, ec) || ec || fs::is_directory(fs_path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::expected<std::string, std::string> ResetSystemPromptFile(const std::string& lubancode_dir,
                                                                const std::string& default_persona) {
    if (lubancode_dir.empty()) {
        return std::unexpected(std::string("找不到 .lubancode 目录,没法还原 system_prompt.md"));
    }
    const fs::path base = Utf8Path(lubancode_dir);
    const fs::path prompt_path = base / "system_prompt.md";
    const fs::path bak_path = base / "system_prompt.md.bak";

    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        return std::unexpected("建目录 " + PathToUtf8(base) + " 失败: " + ec.message());
    }

    std::string bak_result;
    ec.clear();
    if (fs::exists(prompt_path, ec) && !ec) {
        // 旧文件挪成 .bak,已有 .bak 就覆盖(先删再挪;rename 跨不过去时退化
        // 成 copy + remove,跟配置迁移一个路数)。
        std::error_code remove_ec;
        fs::remove(bak_path, remove_ec);  // 不存在也无妨
        ec.clear();
        fs::rename(prompt_path, bak_path, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(prompt_path, bak_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return std::unexpected("备份旧文件到 " + PathToUtf8(bak_path) + " 失败: " + ec.message());
            }
            std::error_code cleanup_ec;
            fs::remove(prompt_path, cleanup_ec);  // 尽力删,删不掉下面 trunc 重写也能盖住
        }
        bak_result = PathToUtf8(bak_path);
    }

    if (!WriteWholeFile(prompt_path, DefaultSystemPromptFileContent(default_persona))) {
        return std::unexpected("重写 " + PathToUtf8(prompt_path) + " 失败(检查一下权限)");
    }
    return bak_result;
}

std::vector<std::string> ListSouls(const std::string& lubancode_dir) {
    std::vector<std::string> names;
    if (lubancode_dir.empty()) {
        return names;
    }
    const fs::path souls_dir = Utf8Path(lubancode_dir) / "souls";
    std::error_code ec;
    if (!fs::exists(souls_dir, ec) || ec || !fs::is_directory(souls_dir, ec)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(souls_dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const fs::path& path = entry.path();
        if (path.extension() != ".md") {
            continue;
        }
        names.push_back(PathToUtf8(path.stem()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace lubancode::config
