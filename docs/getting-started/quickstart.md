# 三分钟跑完第一场任务

[文档首页](../README.md) · [为什么是 LubanCode](why-lubancode.md) · [配置手册](../reference/configuration.md) · [排错手册](troubleshooting.md)

这页只办一件事：把 LubanCode 跑起来，接上一只模型，让它在仓库里做完一场可验的活。

## 1. 安装

Windows PowerShell：

```powershell
irm https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/scripts/install.ps1 | iex
lubancode --version
```

Linux 与 macOS 从 [GitHub Releases](https://github.com/relic-yuexi/LubanCode/releases) 取对应压缩包，解开后运行：

```bash
./install.sh
lubancode --version
```

Release 是原生程序，不要求 Node.js 或 Python。安装包还会放下随包 `rg`、官方 Skills、文档与许可证。

## 2. 接模型

进一座仓库，直接启动：

```bash
cd your-project
lubancode
```

头一回若没连接，开场页会叫你选 Provider。选厂家，填 Key，挑模型。LubanCode 把地址、协议和默认参数带上；配妥便进会话，不用重启。

目录没有你要的端点，就选“自定义”。也可先跳过，进主界面后再敲：

```text
/provider add
```

密钥最好放环境变量，不要写进仓库。若连接不对，先看最终配置从哪儿来：

```powershell
lubancode --config
```

它会给密钥打码，并逐字段列出来源。Provider 字段、环境变量与覆盖次序见[配置手册](../reference/configuration.md)。

## 3. 让它先认规矩

在交互界面敲：

```text
/init
```

这会在 Git 根生成 `AGENTS.md`。把构建命令、测试入口、目录边界写进去。已有项目指令时，LubanCode 不覆盖，只重载。

想看这场究竟吃了哪几份指令，再敲：

```text
/instructions
```

## 4. 交代一件能验的活

头一场别只问“看看项目”。给它目标、边界与验收：

```text
先读 AGENTS.md 和构建配置。找出用户登录失败时错误码丢失的根因，
只改认证模块，补回归测试，跑相关测试。最后列出改动、证据和没验证的地方。
```

模型要读文件、搜索或抓网页，LubanCode 直接记下。要改文件、跑命令，便按当前审批档过门。`Shift+Tab` 可切档；不熟时留在默认档（`default`），逐项看清再放。

模型工作时仍能打下一句话。按回车，消息排进队列；当前工具收尾后再送进去。`Ctrl+O` 展开工具参数与全文，`Ctrl+E` 单看当前条目，`Esc` 打断本轮。

## 5. 收工前看三样

```text
/todos
/usage
/export
```

- `/todos` 看任务有没有漏尾巴。
- `/usage` 看本场 token、cache、模型与用途分账；服务端没回的数据会明标未知。
- `/export` 把会话导成 Markdown。

退出后要接着干：

```bash
lubancode --continue
```

只想跑一枪，不进交互界面：

```bash
lubancode "读完项目后，列出三处最该修的错误处理，并给出源码位置"
```

拿现成 diff 喂进去也成：

```bash
git diff --cached | lubancode "审这份改动，只报会挡发布的问题"
```

要把单发运行的 Harness 轨迹另存为 JSONL：

```bash
lubancode --yes --output run.jsonl "修复这个问题，跑测试并报告证据"
```

`--yes` 会放行全部确认项。脚本和隔离环境可用；日常仓库别顺手带上。

## 下一步

| 你要做什么 | 往哪儿走 |
| --- | --- |
| 换 Provider、自建 vLLM、查推理档 | [配置手册](../reference/configuration.md) · [vLLM 兼容](../features/providers/vllm.md) |
| 开子代理、Skills、MCP、LSP、插件 | [扩展指南](../features/extensions/README.md) |
| 做可重复编排 | [Workflow 指南](../features/workflows/README.md) |
| 接 GUI 或远程客户端 | [app-server](../features/app-server/README.md) |
| 查命令与按键 | [命令参考](../reference/commands.md) |
| 启动失败、乱码、连接不通 | [排错手册](troubleshooting.md) |

