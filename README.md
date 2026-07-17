# lubancode

一个用 C++ 写的 AI 编程 CLI。眼下还是骨架阶段(M0):能编译、能跑、`--version` 有输出。

## 依赖

- CMake ≥ 3.21
- 一个支持 C++23 的编译器(见下方"这台机器上的实际情况")
- [cpr](https://github.com/libcpr/cpr)(HTTP 客户端)、[nlohmann-json](https://github.com/nlohmann/json)——两个都会自动拉取,不用手装

## 构建

本机(Windows,Git Bash)已验证过的构建方式,用 CMakePresets:

```bash
# Debug
cmake --preset debug
cmake --build --preset debug
./build/debug/Debug/lubancode.exe --version

# Release
cmake --preset release
cmake --build --preset release
./build/release/Release/lubancode.exe --version
```

预期输出:

```
lubancode 0.1.0
```

不传参数直接跑,会顺带跑一遍 `cpr`、`nlohmann-json` 的最小示例,证明两个依赖都链接、能用:

```
lubancode 0.1.0
[demo] nlohmann::json -> {"deps":["cpr","nlohmann-json"],"name":"lubancode","version":"0.1.0"}
[demo] cpr 链接成功, CPR_VERSION = 1.11.1
```

## 这台机器上的实际情况

写这份骨架时探测过的工具链,如实记一笔:

- **CMake** 4.2.3,**Ninja** 1.13.2 都有。
- **MSVC**:没装在 PATH 里能直接调用的 `cl.exe`,但 Visual Studio 2022 BuildTools(17.14,VC 工具集 14.44)装在机器上,CMake 用 `Visual Studio 17 2022` 生成器能自动找到、能编译、能跑通 `std::expected`。所以 `CMakePresets.json` 里用的就是这个生成器。
- **g++**(MSYS2 mingw64,15.2.0)也在 PATH 里,同样支持 C++23 与 `std::expected`,留作备选,没有用在默认 preset 里。
- **vcpkg**:这台机器上没装(`where vcpkg` 找不到,`VCPKG_ROOT` 没设,`C:\vcpkg` 不存在)。`vcpkg.json` 照样写好留给 CI/装了 vcpkg 的机器用;`CMakeLists.txt` 里会先探测 `VCPKG_ROOT` / vcpkg 工具链,探测到就走 `find_package`,探测不到就自动回退到 `FetchContent` 直接拉 cpr、nlohmann-json 的源码来构建——本机走的就是这条路。

C++ 标准用的是 **C++23**(MSVC 19.44 与 mingw g++ 15.2 都完整支持 `std::expected`,不用降级到 C++20)。

## 依赖拉取的坑(如实记录)

本机网络环境下,`github.com` 主站(不管是 `git clone` 还是 `github.com/.../releases/download/...` 这类直链)连接经常被重置、连不上;但 `codeload.github.com` 的归档包下载(`https://codeload.github.com/<owner>/<repo>/tar.gz/refs/tags/<tag>`)是通的。所以 `CMakeLists.txt` 里所有 `FetchContent_Declare` 都改成了走 `codeload.github.com` 的 tarball URL,没有用 `GIT_REPOSITORY`。

cpr 默认会自己再拉一份 `curl`(默认地址在 `github.com/curl/curl/releases/download/...`,同样连不上)和 `zlib-ng`(`git clone` 到 `github.com`,也连不上)。这里的处理是:

- `curl` 源码提前用 `codeload.github.com` 下载解压好,通过 `FETCHCONTENT_SOURCE_DIR_CURL` 让 cpr 直接复用,不用它自己再下载一次。
- `zlib-ng` 直接关掉(`CURL_ZLIB OFF`),M0 骨架用不上压缩支持。

另外,这台机器上 MSYS2/mingw64 的 `include` 目录里装着 `nghttp2`、`libidn2`、`libpsl`、`libssh2` 的开发头文件。用 MSVC 编译 curl 时,CMake 的 `find_package` 会把这些"顺手"找出来当系统库用——但那是给 MinGW/GCC 用的头文件,MSVC 编译器读不懂,会直接报语法错误(`C2061`/`C2059` 之类)。处理办法是显式关掉这几个可选特性,并用 `CMAKE_DISABLE_FIND_PACKAGE_<Pkg>` 禁止 `find_package` 再去找它们(见 `CMakeLists.txt` 里的注释)。

依赖默认编译成静态库(`BUILD_SHARED_LIBS OFF`),这样 `lubancode.exe` 自己就能跑,不用把 `cpr.dll`、`libcurl-*.dll` 之类的运行时依赖到处放、还得操心 PATH。

## 配置

lubancode 要跟大模型对话,得知道 wire(协议)、base_url、api_key、model 这几件事。配置分四级,优先级从高到低,按**字段**逐个决——不是整套配置一刀切:

1. **LUBANCODE_ 专属环境变量**:`LUBANCODE_WIRE`、`LUBANCODE_BASE_URL`、`LUBANCODE_API_KEY`、`LUBANCODE_MODEL`、`LUBANCODE_MAX_CONTEXT`、`LUBANCODE_THEME`、`LUBANCODE_SYSTEM_PROMPT_FILE`
2. **配置文件**:`.lubancode/` 是 lubancode 的"家",往后 `plugins/`、`skills/` 也会住这儿,配置只是先搬进去的第一样东西。查找顺序四级:当前目录的 `.lubancode/config.json` → 用户主目录(Windows 是 `%USERPROFILE%`)下的 `.lubancode/config.json` → 当前目录的旧位置 `.lubancode.json` → 用户主目录的旧位置 `.lubancode.json`。命中旧位置且新位置还没有文件时,会自动把文件搬过去(建好 `.lubancode/` 目录,原地挪一份,搬不动就复制再删旧的),屏幕上打一行"配置已迁移到 ..."通知;搬家失败也不会中断程序,照旧用回那份旧文件,只是提示一句。字段除了 `wire`/`base_url`/`api_key`/`model`/`max_context_chars`,还可以写 `theme`、`system_prompt_file`
3. **通用环境变量**(向后兼容):`wire=anthropic` 时读 `ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_MODEL`;`wire=responses` 时读 `OPENAI_BASE_URL`/`OPENAI_API_KEY`/`OPENAI_MODEL`
4. **内置默认值**:`wire=anthropic`、`max_context_chars=600000`、`theme=dark`。`base_url`/`api_key`/`model`/`system_prompt_file` **没有内置默认值**——lubancode 是通用工具,不绑死哪一家模型服务,`base_url`/`api_key`/`model` 这三项四级都没配到,交互模式会自动走一遍初次配置向导,单发模式/管道模式会直接报错并提示三条配置途径;`system_prompt_file` 没配到就用内置的默认人格。

### 为什么要有专属环境变量

不少人机器上已经装了 Claude Code、Codex 之类的工具,全局环境变量里早就设好了 `ANTHROPIC_BASE_URL`、`ANTHROPIC_AUTH_TOKEN`——这些变量是给那些工具专用的中转服务配的,lubancode 要是也去读它们,轻则连错服务,重则被中转拒之门外(比如报 `this group only allows Claude Code clients`)。所以 lubancode 才自立门户,推荐直接用 `LUBANCODE_*` 专属变量,或者干脆放一份配置文件,不跟别的工具打架。

### 最快上手:交互模式的初次配置向导

第一次用、还没配过 `base_url`/`api_key`/`model` 的话,什么都不用提前准备,直接不带参数运行 `lubancode`,会自动进初次配置向导:

```
$ lubancode
=== lubancode 初次配置向导 ===
(base_url / api_key 没读到,先配一遍,配完直接进入会话)

接口格式:
  1) anthropic (Claude 系)
  2) responses (OpenAI 系)
选择 [1]: 1

base_url(必填),例如:
  https://api.minimaxi.com/anthropic
base_url: https://api.minimaxi.com/anthropic

api_key(必填): sk-...

model:回车自动从接口获取列表,或者直接输入模型名。
model: 
  1) MiniMax-M3
  2) MiniMax-M2.7
  ...
选择模型编号 [1]: 1

配置汇总:
  wire     = anthropic
  base_url = https://api.minimaxi.com/anthropic
  api_key  = sk-xxxxxx...
  model    = MiniMax-M3

保存到 C:/Users/你的用户名/.lubancode/config.json? [Y/n]: 
已保存到 C:/Users/你的用户名/.lubancode/config.json
lubancode 0.5.0  [anthropic] MiniMax-M3
cwd: D:/your/project  ·  输入问题回车发送,exit 退出,/help 看命令
> 
```

model 那一步回车不填,会真的去接口拉一份模型列表(`GET {base_url}/v1/models` 或 `{base_url}/models`,看 wire),编号选;拉取失败(网络问题、404 之类)会如实报错,然后回落到手输模型名,不会卡住。选 `Y`(默认)会把这份配置存进主目录的 `.lubancode/config.json`,下次直接进会话,不用再配一遍;选 `n` 只在这一次会话生效。

如果机器上还留着老版本的旧配置(`~/.lubancode.json`),不用手动搬——启动时读到旧文件、新位置又还没有,会自动挪过去,打一行"配置已迁移到 C:/Users/你的用户名/.lubancode/config.json",接着照常进会话,内容一字不改,旧文件搬完就没了。

### 手动配置:主目录放一份 .lubancode/config.json

也可以不走向导,自己在用户主目录(Windows 是 `%USERPROFILE%`,即 `C:\Users\你的用户名\`)下建一个 `.lubancode` 目录,里面放一个 `config.json`,下面拿 MiniMax 举例(换成你自己在用的模型服务地址、密钥、模型名即可,lubancode 不绑定任何一家):

```json
{
  "wire": "anthropic",
  "base_url": "https://api.minimaxi.com/anthropic",
  "api_key": "sk-替换成你自己的密钥",
  "model": "MiniMax-M3",
  "max_context_chars": 600000
}
```

字段全部可选,缺的自动往下一级找(通用环境变量,再往下是内置默认值——但 `base_url`/`api_key`/`model` 这三项没有内置默认值)。这份文件带着密钥,别提交进任何仓库——`.gitignore` 里已经排除了整个 `.lubancode/` 目录(连带旧位置的 `.lubancode.json` 一并排除,免得漏网)。

也可以只在某个项目目录下放一份 `.lubancode/config.json`,cwd 那份优先级比主目录那份高,适合某个项目要连别的模型服务的场景。

### 交互模式里的命令

交互模式下,输入以 `/` 开头的一行走命令,不发给模型:

| 命令 | 作用 |
| --- | --- |
| `/help` | 列出所有命令 |
| `/model` | 拉取模型列表,编号选择切换(不带参数) |
| `/model 名字` | 直接切到指定模型名,不用拉列表 |
| `/config` | 打印当前生效配置(`api_key` 打码)和本会话实际在用的 model |
| `/clear` | 清空对话历史 |
| `/exit` | 退出(裸词 `exit`/`quit` 也认) |

`/model` 切换只影响当前会话;如果当前有生效的配置文件,切完会问一句要不要顺手写进去,不写就只是这一次会话用新模型,下次启动还是原来配的那个。

### 排查配置问题

`lubancode --config` 能打印出当前实际生效的配置,以及每个字段是从哪一级来的(`api_key` 会打码,只显示前 8 位):

```
lubancode 最终生效的配置:

  wire               = anthropic  [配置文件(.lubancode/config.json)]
  base_url           = https://api.minimaxi.com/anthropic  [配置文件(.lubancode/config.json)]
  api_key            = sk-xxxxxx...  [配置文件(.lubancode/config.json)]
  model              = MiniMax-M3  [配置文件(.lubancode/config.json)]
  max_context_chars  = 600000  [内置默认值]
```

配置文件坏了(不是合法 JSON)、`api_key` 四级都没配到,都会报错,错误信息里带着文件路径或者提示去哪几个地方配。

## 终端体验

### 配色主题

交互模式默认按 `dark` 主题给启动横幅、`> ` 提示符、`[工具]` 行、确认提示、`[错误]`、token 统计行上色,模型回复正文本身**不**上色(保持原样,不干扰阅读)。可选三套:

- `dark`(默认):适合深色背景终端
- `light`:适合浅色背景终端
- `plain`:完全不上色,等同于纯文本

切换方式跟别的字段一样,按四级优先级:`LUBANCODE_THEME` 环境变量,或者配置文件里的 `"theme"` 字段(`"dark"`/`"light"`/`"plain"`)。

程序启动时会自动探测 stdout 是不是一个真终端:管道/重定向到文件时,不管配的哪个主题,一律自动降级成 `plain`,不会往文件里混进 ANSI 转义序列;Windows 下还会尝试给控制台开 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`,开不开得成也会影响是否真正上色。想在管道模式下也强制看到颜色(比如拿 `grep` 校验 ANSI 序列),设 `LUBANCODE_FORCE_COLOR=1` 可以绕开这个自动降级。

### 思考中转轮 & token 统计

交互模式下,发出请求到模型开始吐第一个字之间,会在原地转一个 ASCII 字符(`-\|/`,不用 Unicode 盲文块,避免某些控制台字体下显示成方块),旁边跟着"思考中"字样;模型一旦开始出字或者在跑工具,转轮就停了,不会跟输出混在一起。管道/非真终端模式下转轮完全不开,不会往输出里混入任何转轮字符。

每次问答(哪怕中间因为工具调用来回了好几轮)结束后,会打一行暗色的 token 用量统计,累计这一次问答里所有请求的输入/输出 token 数和请求次数,例如:

```
[tokens] 输入 1024 · 输出 256 · 请求 3 次
```

命中了 prompt cache 的话(Anthropic 后端看 `cache_read_input_tokens`,Responses 后端看 `usage.input_tokens_details.cached_tokens`),输入这一项后面会带一个括号,注明缓存命中了多少 token;没命中就照旧不带括号,不多打空括号:

```
[tokens] 输入 1578(缓存命中 128) · 输出 83 · 请求 2 次
```

### 逐键编辑、历史、Tab 补全、确认模式

真实控制台下(不是管道/重定向进来的输入),`> ` 提示符这一行是逐键编辑的,不是敲完整行回车才生效:

- 方向键左右移动光标,Home/End 跳到行首/行尾,Backspace 删字符;中日韩文字按显示宽度算,光标定位不会因为宽字符错位。
- 上下方向键翻历史(本次会话里敲过的每一行,回车非空才记一条);翻到一半又开始敲字符,就地当成编辑这条历史,历史索引落回"底部"——再按一次 Up 是重新从最新一条开始翻,不接着刚才那条继续走。
- 一行以 `/` 开头时,下面会实时跟一行淡色提示,列出当下匹配的命令名和一句话说明;按 Tab 补全公共前缀,唯一匹配直接补全命令全名(带一个尾随空格);还有多个匹配,再按一次 Tab 依次轮着候选走。
- Shift+Tab 循环切换会话级"确认模式",切一次打一行提示,提示符前缀跟着变:
  - `> `(默认,`确认`档):`write_file`/`edit_file`/`run_command` 等需要确认的工具照旧逐条问
  - `[auto] > `(`auto` 档):`write_file`/`edit_file` 自动放行,`run_command` 仍然要问
  - `[yolo] > `(`yolo` 档):全自动,什么都不问——跟命令行传 `--yes` 效果一样,`--yes` 直接从 `yolo` 档起手
- Ctrl+C:行里有内容就清空、留在同一次输入里接着编辑;行是空的按 Ctrl+C 才当退出处理。Ctrl+D 一律当 EOF。

输入是从管道/文件重定向进来的(比如 `printf "...\n" | lubancode`),不会走这条逐键编辑的路,原样按行读取,行为跟没有这套编辑器之前一样。

### `--system-prompt`:自定义人格

lubancode 的系统提示词拆成两段:**人格段**(定义"模型是谁、该用什么语气说话")和**环境段**(工作目录、"该用工具时就用工具"这条硬规矩)。`--system-prompt` 换的只是人格段,环境段不管怎么换人格都会原样追加,所以自定义人格哪怕定得很出格(比如"只用文言文回答"),工具照样能正常调用。

```bash
lubancode --system-prompt ./persona.md "帮我看看这个项目的结构"
```

- 文件要求 UTF-8 编码的 `.md` 或 `.txt`,内容整篇原样当人格段用
- 文件不存在、打不开、或者内容是空的,启动时就会报可读的错误,不会打到一半才发现
- 也可以写进配置文件的 `"system_prompt_file"` 字段(或者 `LUBANCODE_SYSTEM_PROMPT_FILE` 环境变量),`--system-prompt` 命令行参数会压过配置文件里的这个字段

## 目录结构

```
src/
├── main.cpp   # 入口:解析 --version / --help
├── cli/       # 命令行交互层
├── agent/     # agent 核心循环
├── api/       # 与大模型对话的通路(Anthropic / Responses 两个后端)
├── tools/     # 模型可调用的工具(read_file、run_command……)
└── config/    # 配置
docs/
└── architecture.md   # 架构说明,详见此文档
```

架构细节(分层依赖、api 双后端设计、工具层、错误处理、里程碑规划)见 [`docs/architecture.md`](docs/architecture.md)。
