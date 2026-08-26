# 开发指南

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [测试指南](testing.md) · [文档规范](documentation.md)

这页写给要编译、调试或改 LubanCode 的开发者。用户配置与命令查对应参考页；这里管工具链、目录、目标和一趟可靠的本地改动流程。

## 1. 工具链

最低要求：

- CMake 3.21 以上。
- 支持 C++23 的编译器。
- Git。
- 能访问依赖归档，或已配 vcpkg manifest 工具链。

仓库在 MSVC、GCC、Clang 三路 CI 编译。Windows 预设用 Visual Studio 2022 x64；Linux/macOS 示例用 Ninja。CMake 先认已启用的 vcpkg toolchain，否则用 FetchContent 拉 `cpr`、`nlohmann/json`、curl 与 doctest。

## 2. 第一次构建

### Windows

```powershell
cmake --preset release
cmake --build --preset release
.\build\release\Release\lubancode.exe --version
```

默认目标会编主程序，并把官方 `skills/` 与 `docs/` 镜到 exe 旁。只点名
`--target lubancode` 虽能得到程序，却不会刷新这两棵资源树。

要带测试：

```powershell
cmake --build --preset release --target lubancode_tests
ctest --test-dir build\release -C Release --output-on-failure
```

调试构建把 `release` 换成 `debug`。产物落在 `build\debug\Debug\`。

### Linux / macOS

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target lubancode lubancode_tests -j 4
ctest --test-dir build --output-on-failure
./build/lubancode --version
```

CI 会显式传 `-DLUBANCODE_USE_CODELOAD=OFF` 走 GitHub 官方地址。本机默认可走 codeload 归档；这只是依赖下载路线，不改程序行为。

## 3. 构建目标

| 目标 | 用处 | 默认构建 |
| --- | --- | --- |
| `lubancode` | 主程序 | 是 |
| `lubancode_official_skills` | 把 `skills/` 镜到程序旁 | 是 |
| `lubancode_official_docs` | 把 `docs/` 镜到程序旁 | 是 |
| `lubancode_tests` | doctest 测试总目标 | `BUILD_TESTING=ON` 时 |
| `deepseek_e2e` | 真服务 opt-in 探针 | 否 |
| `latex_box_experiment` | 公式视觉小样 | 否 |
| `*_driver` | Windows 真控制台刮屏驱动 | 否，仅 Windows |

`EXCLUDE_FROM_ALL` 目标须点名构建。不要以“常规 build 没报错”冒充它们已经编过。

## 4. 源码地图

| 目录 | 现职 | 常见改动 |
| --- | --- | --- |
| `src/app/` | 应用编排、交互会话、slash handler、运行时接线 | 新命令、会话流程、跨模块状态 |
| `src/agent/` | 有状态 Agent、AgentLoop、上下文、compact、prompt 拼装 | 工具循环、历史视图、压缩 |
| `src/api/` | 中立消息与四种 wire | 请求 JSON、SSE 事件、usage |
| `src/runtime/` | 中立回合事件、调度器、goal/loop 服务 | 跨宿主回合语义、排程、重试 |
| `src/sessions/` | 会话存档、目录与生命周期 | JSONL 写入、恢复、归档 |
| `src/workflow/` | workflow 定义、编译、执行与 journal | 图校验、节点调度、恢复 |
| `src/cli/` | 输入、footer、转录、Markdown、diff、i18n | 键位、布局、渲染 |
| `src/config/` | 配置、Provider/模型目录、项目指令 | 字段、默认值、合并 |
| `src/tools/` | 内置工具、子代理、插件桥 | schema、确认、执行 |
| `src/hooks/` | Hook 事件、分派与归并 | 生命周期、决策协议 |
| `src/memory/` | 项目记忆、召回、候选与 worker | 索引、写入、审阅 |
| `src/ptc/` | PTC stub、协议、runner、画像与基准 | 程序化工具调用 |
| `src/mcp/`、`src/lsp/` | 外部进程协议客户端 | framing、生命周期 |
| `src/platform/` | 终端、进程、路径、编码、动态库 | 跨平台语义 |
| `src/prompts/` | 内置提示模块 | 工具方针与协议提示 |

依赖方向与一轮请求见[架构说明](../architecture/README.md)。不确定改哪层时，先问一句：这段逻辑在没有终端、没有某家协议、没有某件工具时还成立吗？答案能帮你把它落回合适边界。

## 5. 一趟本地改动

### 起手

```powershell
git status --short
git log -5 --oneline
```

先认出用户已有改动。不要清理、覆盖或顺手格式化无关文件。

### 找入口

```powershell
rg -n "目标命令|字段名|工具名" src tests docs
```

先读实现，再读相邻测试。大型模块常在头文件注释里写状态账与线程约束，别只扫函数名。

### 小步验证

1. 改纯函数，先跑对应 doctest filter。
2. 改公共库，编 `lubancode_tests`。
3. 改终端状态机，再跑相关 driver。
4. 改协议或真服务兼容，先过离线 fixture，再决定是否跑 opt-in e2e。
5. 改用户可见行为，按[文档规范](documentation.md)同步页面。

只改文档，也要跑目录与断链检查：

```powershell
bash scripts/check_docs.sh
```

### 收口

```powershell
git diff --check
git diff --stat
git status --short
```

提交前看完整 diff。生成文件、测试日志、临时 home、API 响应与截图原件不该误进仓库。

## 6. 新功能怎样落位

一项跨层功能通常按这条顺序长：

1. 纯数据类型与语义函数。
2. 单元测试钉输入、输出与失败态。
3. 接到运行时或 UI。
4. 加配置解析、默认值与诊断。
5. 加集成/真终端测试。
6. 补用户入口、专题页与 CHANGELOG。

不要先在 `interactive_session.cpp` 塞一大段临时逻辑，再回头找抽象。也不要为只用一处的简单分支起一座框架。边界以可测试、可替换、少重复为准。

## 7. 配置改动

新增字段至少要对齐：

- 文件层 optional：分清没写与显式值。
- 有效配置默认值。
- 全局/项目/环境变量的合并规矩。
- 序列化与迁移；旧字段何时还认。
- `--config` 或相应诊断输出。
- 中英文 i18n 文案。
- 配置解析、覆盖、坏值与写回测试。
- [配置手册](../reference/configuration.md)。

密钥与鉴权要分状态，不以空串猜“无需鉴权”。Provider 当前用 `none / env / inline` 三态，详见配置手册。

## 8. 协议改动

四种 wire 各自编码请求、解析事件，Agent 层只认中立类型。改一家的私有字段时：

- 先问能否由 `extra_body` / `extra_headers` 解决。
- 通用能力才进中立层。
- usage 字段须归一，但原始含义不能偷换。
- 工具 use/result 配对要跨断包、取消和错误收口。
- 离线 JSON fixture 先行，真网络只作补证。

详见 [Query 数据流](../architecture/query-data-flow.md)。

## 9. 终端改动

终端不是一串 `cout`。它有 transcript、活动块、footer、composer、面板、焦点与可视区几本账。改动时：

- 状态与绘制分开，先写纯状态机。
- 真正读键盘的路径只有一处持读锁。
- 所有插打都要让旧锚点失效。
- Unicode 按显示列算，不按字节算。
- Windows 真控制台、POSIX TTY、plain 管道三路分清。
- 截图好看不算验收；须断言行序、焦点、残影与 resize。

详见 [终端交互](../features/terminal/README.md) 与 [测试指南](testing.md)。

## 10. 发行

版本号只写在 `src/app/version.hpp`；CMake 配置时从它读取 `PROJECT_VERSION`。用户变化写 `CHANGELOG.md`。发版前先跑：

```powershell
bash scripts/check_release.sh
bash scripts/check_docs.sh
```

前一页核对源码版本与 CHANGELOG；release workflow 还会把 tag 一并送进去，三者有一处不合便停。

推送 `v*` 标签后，release workflow 先查文档目录、链接与 Skill 路由，再在 Windows、Linux、macOS 重编。包里收程序、双语 README、`docs/`、`skills/` 与安装脚本，最后创建 GitHub Release。不要拿本机 `build/` 产物手工塞进发行包。

`docs/` 与 `skills/` 是一对同版本资源。Windows 与便携安装把它们摆在程序旁；POSIX 前缀安装把它们摆进 `<prefix>/share/lubancode/`。`lubancode-config` 由 `../../docs` 找到同包文档。两边须一同复制，一同升级。

## 11. 常见构建故障

| 现象 | 先查 |
| --- | --- |
| configure 卡在下载 | 网络、`LUBANCODE_USE_CODELOAD`、vcpkg toolchain |
| MSVC runtime 不匹配 | 是否绕过顶层 CMake 的静态 runtime 设置 |
| 测试找不到 fixture | 是否经 `tests/CMakeLists.txt` 构建，别手拼测试可执行文件 |
| 只改 Skill 或 docs 却没同步 | 构建 `lubancode_official_skills`、`lubancode_official_docs`，或主默认目标 |
| 文档检查报孤儿页 | 把页面收进 `docs/catalog.txt`，或移到 `interview/` 等合适目录 |
| Debug/Release 路径找错 | 多配置生成器在配置名子目录下放 exe |
| CI 与本机下载路线不同 | CI 关了 codeload；行为应同，依赖来源不同 |

更长的症状树见[排错手册](../getting-started/troubleshooting.md)。
