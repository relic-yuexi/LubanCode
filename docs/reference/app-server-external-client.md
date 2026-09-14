# app-server 外部客户端固定版本兼容记录

[参考手册](README.md) · [app-server 协议](../features/app-server/README.md) · [能力裁剪合同](capability-contract.md) §13

外部应用接入 LubanCode Worker 的兼容基线与验收口径。出处
`todos/应用Worker接入补齐_Agent与Skills装配_独立参数目录及恢复隔离.todo`
§十一(报告交接与能力声明)、P4。首发参考客户端:
[examples/node-client](../../examples/node-client/)。

## 一、钉什么(应用侧的 runtime-lock 面)

外部应用接入前锁四样,任一变动先跑兼容探针再升级:

| 锁项 | 取法 | 说明 |
| --- | --- | --- |
| 二进制 | 文件 SHA-256 + 构建提交 | `initialize` 回的 `lubancodeVersion` 是人读版号,**不做兼容凭据**;凭据是哈希与提交。 |
| 协议版本 | `initialize` 结果的 `protocolVersion` | 本记录基线:**`1.3`**。参考客户端 `expectedProtocolVersion` 钉死,不符即断,不带病上岗。 |
| 方法面 | `initialize` 结果的 `capabilities.methods` | 关键方法(下表)须逐枚在场;`pending` 里的名字按"认识未接线"处理,不调用。 |
| 部署档 | `deployment.json` 的 `schemaVersion`(当前 `1`) | schema 对未知字段拒绝,升级须对 `tests/unit/capability` 冻结侧校验。 |

## 二、关键方法清单(1.3 基线)

| 方法 | 形态 | 兼容要点 |
| --- | --- | --- |
| `initialize` / `initialized` | 请求 + 通知 | 回 `protocolVersion`/`lubancodeVersion`/`platform`/`capabilities`。业务方法前必须握手,否则 `-32002`。 |
| `thread/start` | 请求 | 可选 `clientOperationId`(1.3):同键同 cwd 回原身份 `duplicate:true` + `active`;异载荷 `operation_conflict`。生产入口的回执另带可选 `connection`(尾款 additive):启动冻结的连接快照(wire/model/provider/脱敏端点 `endpoint`/密钥引用 `secretRef`/配置版本 `configVersion`/逐字段 `sources`/角色路由 `roles`)——零密钥,凭据只以引用形态出现;同一 Worker 进程逐场同一份,凭它核对应用配置真生效。 |
| `turn/start` | 请求 | 立即回 `{threadId, turnId, operationId, inputId}`;可选 `clientOperationId`(1.3)受理幂等;终态走 `turn/completed` 事件。 |
| `operation/read` | 请求(只读) | 1.3 新增。`threadId` + `clientOperationId`/`operationId` 至少一枚;回 `status` 五态与 final 事实;零副作用。 |
| `thread/read` / `trace/query` | 请求(只读) | 历史与事件账查询,分页走 `lastSeq` 游标。 |
| `shutdown` | 请求 | 回空 result 后进程自退(退出码 0);EOF(stdin 断)同效。 |

关键事件:`thread/started`、`turn/completed`(唯一终态,带 `executionStatus`)、
`turn/usage`。错误码:标准段 `-32601/-32602/-32603`,服务器段
`-32002/-32004`;幂等面稳定串走 `error.data.code`(`operation_conflict`/
`session_create_unknown`/`operation.append_failed`/`operation_id_mismatch`),
装配拒走 `component_unavailable`(P2)。

## 三、部署隔离基线(§十/§13.1)

一只 Worker 一套独立材料:`LUBANCODE_HOME`(参数根)、
`LUBANCODE_DATA_HOME`(数据根,可写)、`LUBANCODE_MANAGED=1`(托管档)。
宿主给**每个 child 构造专属 env**,不改自己的全局环境;模型凭据经参数根
`config.json` 交付,不进 argv。空值 env 的跨进程传递只有 envblock 一条真路
(Node `spawn` 的 env 表空串原样落块),Worker 启动门对"设了但为空"明拒
(退出码 1 + stderr 人话)。

参数根不是严格零写入:Worker 启动时对材料根**查漏补缺地播种发行提示脚手架**
(`system_prompt.md`/`SOUL.md`/`souls/`/`prompts/`,只建新绝不覆盖,写失败
静默跳过——挂只读根时播种自然跳过,运行期有内置回退)。承重边界是**状态
零进参数根**:workspaces/logs/cache/workflow-runs 等一切状态只落数据根
(e2e 断言钉死);个人家目录零读写。

## 四、已验范围(ctest `e2e.app_server.node_client`,真 exe + 假模型)

探针即 [examples/node-client/run_e2e.js](../../examples/node-client/run_e2e.js),
五幕全部真 `lubancode app-server` 子进程 + 本地回环假 anthropic 后端,
零真实密钥:

| 幕 | 验的是什么 | 对账验收单 |
| --- | --- | --- |
| 主链路(托管档) | 握手钉 1.3 + 能力表 → 幂等受理闭环(thread/turn 双键)→ `operation/read` final+稳定正文 → 同键重发零重跑/异载荷冲突 → 凭据只走参数根(argv/stdout/stderr/结果全无密钥)→ 状态只落数据根、状态零进参数根、个人家目录零读写 → 收口四步退出码 0 | AW-02/11/12/15/18/20 |
| 双 Worker 并行 | 两套根并跑互不串材料,模型流量与终态正文各归各家 | AW-04(进程级) |
| 飞行中硬杀 | 假后端扣住应答,Worker 被 SIGKILL/TerminateProcess → 同根重启 `operation/read` 回 `unknown`+`gaps` 不冒充,创建去重回原场 `active=false`,模型零重跑 | AW-14/19(进程级硬杀) |
| EOF 孤儿收口 | 宿主消亡(毁 stdin)→ Worker 自退退出码 0 → 重启后记录仍能查询 | AW-20 尾 |
| 空值 env | envblock 跨进程传空值 → 启动门明拒(`LUBANCODE_DATA_HOME=''`/`LUBANCODE_HOME=''` 两案) | §13.1 Windows 空值语义 |

平台:GitHub Actions runner 实测——macos-clang 腿(PR 腿)必跑;
windows-msvc、linux-manylinux 腿按 CI 设计合入 main 后跑。**linux-manylinux
容器无 node,e2e 不注册,Linux 平台此面未验**;每批 CI 结论以 PR/run 链接
为准(见 todos 的 P4 记账)。

## 五、未验边界(如实,不冒充)

| 边界 | 状态 | 归属 |
| --- | --- | --- |
| 真拔电 / OS 崩溃 | **未验**(进程级硬杀已由幕 3 钉;账态注入模拟见 `tests/unit/app_server/test_app_server_operation_idempotency.cpp`) | 真机批次 |
| 协作取消 → 超时 → 宿主按部署规则终止进程树 | **未接**——产品面没有"部署级 kill 超时"配置;`turn/interrupt` 的协作取消在跑动回合上的收口未入本 e2e | 后续小单(§十"取消先走 runtime 协作取消,超时后宿主按部署规则终止进程树") |
| MCP 子进程随 Worker 强杀收口 | **未做真机断言**。源码层:Windows 走 `ChildProcess` 的 Job Object `KILL_ON_JOB_CLOSE`(内核保证,worker 死则 MCP 子进程死);POSIX 走进程组 + 管道 EOF 协作退出(不响应 EOF 的子进程会残留) | 真机批次(§十"Windows验证MCP/子进程随Worker收口") |
| OS 级隔离(独立账号/受控工具名单之外的进程权限) | **未验**——本记录只覆盖"显式零工具默认档"的部署形态,不冒充强隔离(§十"声明需强隔离而当前平台无法落实时拒启") | 多租户隔离 owner 单 |
| artifact 越权/过期/损坏(AW-21)、structured output | 未入本 e2e | 后续切片 |
| 恢复执行(续跑) | 1.x 协议面没有恢复方法,`active:false` 如实交代 | SessionV3 owner 单 2.0 面 |

## 六、兼容探针的跑法

```bash
node examples/node-client/run_e2e.js --binary <lubancode 可执行文件>
```

ctest 里即 `e2e.app_server.node_client`(无 node 的环境不注册、如实缺证据)。
升级 Worker 前后的对外行为对账以这五幕为准;报文形状变更必须 bump
`protocolVersion`(协议合同),本记录随之更新。
