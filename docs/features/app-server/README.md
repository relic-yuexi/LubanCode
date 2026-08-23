# app-server:无界面后台协议

[文档首页](../../README.md) · [命令参考](../../reference/commands.md) · [工具参考](../../reference/tools.md) · [测试手册](../../development/testing.md) · [架构说明](../../architecture/README.md)

`lubancode app-server` 是无界面后台入口:前端从 stdin 发 JSON-RPC 请求,后台在项目机上读文件、改代码、跑命令,再从 stdout 把正文、工具进度、审批、diff 与结果一笔笔传回来。SSH 只管架通道,活计全落在远端。

```text
本机 LubanCode 前端
    │ 消息 / 审批 / 打断 / 进度
    └──── SSH ────► 远端 lubancode app-server
                         ├─ 会话与模型请求
                         ├─ 项目文件和工具
                         └─ 日志、diff、usage 与终态事件
```

不把监听端口裸露到公网。首版只走 SSH 承载的 stdio;WebSocket 是另一张单的事。

## 传输与进程纪律

- stdin 逐行收 UTF-8 JSON,stdout 逐行吐 UTF-8 JSON;一行一条完整消息。
- stdout 是协议专线。日志、诊断、崩溃前说明一概走 stderr;工具子进程输出装进事件字段,绝不漏到 stdout 搅坏分帧。
- **jsonrpc 字段已冻结**:出站不带 `jsonrpc:"2.0"`,入站不校验(带不带都认)。翻这个开关等于协议破坏,须 bump 协议版本。
- 先 `initialize`(回能力表)再 `initialized`(通知),然后业务放行;重复 initialize 报 `-32600`,握手前调业务报 `-32002`。
- EOF、断管、`shutdown`、`exit` 都收口:打断在跑回合、刷完能刷的终态、退出,不留孤儿进程。
- 入站单行上限 1MB,超限回 parse error 后退线。
- 事件队列有界(默认 4096)。撞满先合并可合并的 delta(同条目的增量并成一条,计 `coalesced`),再丢可丢事件并补一条 `queue/overflow` 通报(计 `dropped`);终态与审批类事件绝不丢。

## 协议版本

`1.0`。任何报文形状变更必须 bump,前端拿 `initialize` 结果里的 `protocolVersion` 对表。

## 方法面

### 握手与退场

| 方法 | 方向 | 说明 |
| --- | --- | --- |
| `initialize` | 请求 | 回 `protocolVersion`/`lubancodeVersion`/`platform`/`capabilities`。能力表如实分 `methods`(已接线)、`pending`(名字认识没接线)、`serverRequests`(服务端反向请求)、`itemTypes`、`turnStatuses`。 |
| `initialized` | 通知 | 握手收尾,业务放行。 |
| `shutdown` | 请求 | 回空 result 后收线。 |
| `exit` | 通知 | 立即收线,不回话。 |

### thread(会话)

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `thread/start` | `cwd?` | `{threadId, cwd}`;会话档真落盘(`~/.lubancode/sessions/`),meta 写真值(wire/model 来自配置四级合并)。 |
| `thread/list` | `scope?/state?/sort?/search?/cwd?/cursor?/limit?` | `{threads:[...], total}`;走 `runtime::SessionCommandService`,与终端 `/sessions` 同一碗饭。缺省全量 + active + updated。`startedAt` 续给(`createdAt` 同源),老前端不断。 |
| `thread/stop` | `threadId` | 停场;在跑回合按打断收口。 |
| `thread/archive` | `threadId` | 搬进 `archive/`;成功发 `thread/updated`(state=archived)。开着的 thread 拒 `active_thread`。 |
| `thread/unarchive` | `threadId` | 搬回根;成功发 `thread/updated`(state=active)。 |
| `thread/delete` | `threadId, confirm` | 没带 `confirm` 拒 `confirmation_required`;带了真删,发 `thread/deleted`。 |

搬删的错误码走 `error.data.reason`:SessionCommandService 的稳定串(`not_found`/`ambiguous`/`confirmation_required`/`path_outside_root`/`target_exists`/`io_error`)加协议侧的 `active_thread`。

### turn(回合)

| 方法 | 参数 | 说明 |
| --- | --- | --- |
| `turn/start` | `threadId, text, images?` | 立即回 `{threadId, turnId}`,整回合在工作线程跑。`images` 是数组,元素 `{mediaType, data, filename?, width?, height?}`(`data` 是不带 data URL 前缀的 base64)——字段名与 `api::ImageBlock` 对齐,图片原样入会话历史。同一 thread 同拍两轮拒 `-32004`。 |
| `turn/interrupt` | `threadId, turnId?` | 置打断旗;审批悬停立即醒;终态 `interrupted`。回合不在跑或点名别的回合报 `-32005`(迟到不追)。 |
| `turn/steer` | 留位 | 未接线,`initialize` 能力表里在 `pending`。 |

### workflow(run 账查询)

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `workflow/query` | `runId, lastSeq?` | `{runId, workflowId, state, lastSeq, nodes, ...}` 快照 + `lastSeq+1` 起的增量事件(`workflow/event`)。`lastSeq` 缺省 0 = 全量。读 `~/.lubancode/workflow-runs/<run-id>/`。 |

## 事件账

统一三层:会话装回合,回合装条目。每条事件的 `params` 带 `seq`——进程内单调序号(连接层统一盖),前端凭它排序、查漏。

| 事件 | 说明 |
| --- | --- |
| `thread/started` | `params`: `threadId, cwd`。 |
| `thread/updated` | 搬删后:`threadId, state`。 |
| `thread/deleted` | 删除后:`threadId`。 |
| `thread/stopped` | 停场后:`threadId`。 |
| `turn/started` | `threadId, turnId`。 |
| `item/started` | `params.item`: `{id, type, ...}`。工具条目带 `tool`/`toolUseId`/`input`;`write_file`/`edit_file` 额外带 `diff`(见下);`run_command` 的 type 是 `command`。 |
| `item/delta` | `itemId, delta`(正文/思考/输出的增量)。 |
| `item/completed` | `params.item`: 终态字段(`result`/`isError`;打断收口的带 `cancelled`)。 |
| `turn/usage` | 每次到模型的请求收尾:`usage`(五项 camelCase,与 `turn/completed` 的 usage 同形)、`model`。 |
| `turn/context` | 上下文压力:`context.phase`(pre_request/after_hard_trim)、`projectedTokens`、`windowTokens` 等。前端画水位条吃这个。 |
| `turn/completed` | 唯一终态:`status`(success/error/cancelled/interrupted)、`usage`、`stepsUsed`;error 时带 `error` 文案。 |
| `queue/overflow` | 丢事件后的通报:`dropped`、`coalesced`。 |
| `workflow/event` | wf run 的 journal 事件:`runId`、`eventSeq`(run 内单调号,与快照的 `lastSeq` 对账)、`type`、`workflowId`、`nodeId?`、`data`。 |

条目类型(`item.type`):`text`、`thinking`、`tool`、`command`、`file_change`(留位)、`question`(留位)、`agent`(留位)、`error`。

### diff 行表

`write_file`/`edit_file` 的 `item/started` 带 `diff` 字段——`runtime::DiffTable` 中立行表直转,零翻译:

```json
{
  "path": "src/a.cpp",
  "located": true,
  "replacedCount": 1,
  "oldExists": true,
  "addedLines": 2,
  "removedLines": 1,
  "rows": [
    {"kind": "context", "text": "int main() {", "oldNo": 1, "newNo": 1},
    {"kind": "del", "text": "  return 0;", "oldNo": 2, "newNo": 0},
    {"kind": "add", "text": "  return 1;", "oldNo": 0, "newNo": 2}
  ]
}
```

`kind` 三值:`context`/`del`/`add`;`oldNo`/`newNo` 1 起,没有的是 0。行表在工具执行前算(预览语义,与终端确认块同款);edit_file 定位失败(`located: false`)也照样给段内行表,前端不须特判。终端按 `kind` 添色,GUI 按 `kind` 上 DOM class——两家渲染,一份真值。

### 命令输出

`run_command` 的条目类型是 `command`;输出整段落 `item/completed` 的 `result`(工具层没有流式回调,增量留待工具层出流式钩子后再接)。

## 服务端反向请求(审批与提问)

需确认工具停住发 `permission/request`;`ask_user` 工具发 `user/ask`。两者都是服务端发给前端的双向请求,信封 `id` 为 0,配对靠 `params.requestId`(`req-<n>`)。

`permission/request` 的 params:`threadId`、`turnId`、`requestId`、`tool`、`input`(结构化 JSON)、`toolUseId?`、`reason?`。

前端响应:`{"id":0, "result":{"requestId":"...", "decision":"..."}}`,decision 四态:

| 决定 | 行为 |
| --- | --- |
| `accept` | 本次放行。 |
| `acceptForSession` | 放行并记入本 thread 会话级账,同工具后续免问(只写内存,不落盘)。 |
| `decline` | 拒绝;原因随 tool_result 送回模型。 |
| `cancel` | 视作用户撤回,按拒绝收口。 |

悬停期间事件泵照常活:前端可随时 `turn/interrupt`(悬停立即按取消收口,文案写"被 turn/interrupt 打断",不冒充用户拒绝)。悬停超时按"没人可答"悬空收口,同样不冒充。迟到/失效的答复报 `-32005`。

`user/ask` 的 params:`threadId`、`turnId`、`requestId`、`header`、`question`、`options:[{label, description}]`、`multiSelect`。响应 result 里 `requestId` + `answers`(字符串数组)。

## 错误码

标准段:`-32700` parse error、`-32600` invalid request(含重复 initialize)、`-32601` method not found、`-32602` invalid params、`-32603` internal error。

服务器段:`-32000` busy、`-32002` 未握手、`-32003` 已 shutdown、`-32004` 同 thread 同拍两轮、`-32005` 迟到/失效的反向请求答复。

错误响应的 `error.data` 可带稳定 `reason` 串(会话搬删等),前端凭它分支。

## SSH 承载

远端只须备好 SSH、LubanCode CLI 和既有 provider 凭据。前端 SSH 登录后从远端用户的 login shell 拉起:

```bash
ssh <host> lubancode app-server
```

验证口径:

- 单测 `tests/integration/app_server/test_app_server_smoke.cpp`:真进程 + stdio 管道替身(SSH 通道的进程形状),验 stdout 逐行可解析、握手一条线、坏 JSON 不炸、EOF 自退。
- 手测脚本 `scripts/tests/app_server_ssh_smoke.sh`:真 SSH 通道(`ssh localhost` 起手;无 sshd 的机器 `--local` 走本机管道)。验 login shell 拉起、stderr 隔离、断线后远端不留孤儿。

```bash
# 本机管道(CI/开发机)
bash scripts/tests/app_server_ssh_smoke.sh --local
# 真 SSH(要求 localhost sshd 与免密)
bash scripts/tests/app_server_ssh_smoke.sh localhost
```

SSH 断开后 app-server 读到 stdin EOF 自退,远端不留后台子进程;再连后 `thread/list` 照样能列出既有会话档。

## 不做

- 不口头宣称兼容 Codex app-server。兼容要有 schema 版本、黄金报文与跨版本合同测试作证。
- 不内置 SSH 客户端、不代管密钥;SSH 发现、登录和隧道归调用方。
- 不把本机文件、凭据或工具混进远端会话。哪台机器跑 app-server,哪台机器出环境。
