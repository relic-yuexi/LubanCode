# Node 外部客户端(应用 Worker 接入参考实现)

外部应用常驻、一项独立任务起一只 LubanCode Worker 的参考客户端。这是
**验收协议与部署形态的工具,不是产品**——与 [web-console](../web-console/)
同一定位:全程只走 app-server 协议,不调任何内部 C++ 接口,不读私有文件
猜结果。

| 文件 | 内容 |
| --- | --- |
| [lubancode_worker_client.js](lubancode_worker_client.js) | 客户端库:专属 env 构造(§13.1 铁律)、stdio JSON-RPC、协议 1.3 幂等受理闭合、收口四步 |
| [run_e2e.js](run_e2e.js) | 端到端验收跑法(五幕,真 exe + 假模型,ctest 名 `e2e.app_server.node_client`) |
| [deployment.example.json](deployment.example.json) | 无真实密钥的托管部署样例(零工具默认面) |

## 形态

一只 Worker = 一个子进程 = 一套独立材料:

```text
LUBANCODE_HOME      参数根            config.json、deployment.json、agents/、skills/…
                                       (启动会查漏补缺播种发行提示脚手架,只建新不覆盖)
LUBANCODE_DATA_HOME 数据根(可写)    workspaces/(会话账)、logs/、缓存…
LUBANCODE_MANAGED=1 托管档           个人材料层整层不读,cwd 项目级整层不读
```

状态只落数据根;参数根收材料(含发行脚手架的播种副本),个人家目录零读写。

客户端给**每个 child 构造专属 env**——从宿主 env 快照出发,摘掉应用根
变量残留,再按本 Worker 的根写入;不修改宿主自己的全局环境,也不重定义
`HOME`/`USERPROFILE` 冒充应用参数根(`docs/reference/capability-contract.md`
§13.1)。模型凭据经参数根的 `config.json` 交给 Worker,**不进 argv、不进
宿主环境**。

空值 env 的跨进程传递走 envblock 一条真路:Node `spawn` 的 env 表里空串
原样落环境块,Worker 的启动门把"设了但为空"识别为配置错误并拒启(退出码
1 + stderr 人话)。不要用 `spawn` 之外的路径(如先 `process.env.X=''` 再
继承)传空值——Windows CRT 面上"空=未设"。

## 最小闭环

```js
const { LubancodeWorkerClient } = require('./lubancode_worker_client.js');
const worker = await LubancodeWorkerClient.spawn({
  binary, configRoot, dataRoot, managed: true,
  deploymentProfile: configRoot + '/deployment.json',
  model: { baseUrl, model, apiKey, wire: 'anthropic' },
});
const init = await worker.initialize();   // 钉协议版本(不符即抛),能力表在 init.capabilities
const thread = await worker.startThread({ clientOperationId: 'CREATE-1', cwd });
const accepted = await worker.startTurn({ threadId: thread.threadId, text, clientOperationId: 'OP-1' });
await worker.waitTurnCompleted(thread.threadId, accepted.turnId);
const facts = await worker.readOperation({ threadId: thread.threadId, clientOperationId: 'OP-1' });
```

回执丢失的重试策略:**同键同载荷原样重发**——thread/start 回原场身份
(`duplicate`、`active`),turn/start 回原受理(原 `operationId`/`turnId`),
不建第二场、不重跑模型;同键异载荷会吃 `operation_conflict`(-32602 +
`error.data.code`),那是键用错了,不是重试。

## 收口四步(§十)

1. **停受理** `beginClose()`——客户端侧不再发 thread/start、turn/start;
2. **断调度**(由 1 保证)——没有新受理就没有新派发;
3. **排空** `waitIdle()`——等在飞回合都到 `turn/completed`;
4. **回收** `shutdown()` 请求 → 进程自退(退出码 0);超时不退才
   `terminateHard()` 硬杀(POSIX SIGKILL / Windows TerminateProcess)。

宿主自己消亡的孤儿形态:stdio app-server 读到 stdin EOF 自行收线(退出码
0)。硬杀/断电后的执行状态经重启 + `operation/read` 核对:终态行在才是
`final`,否则 `unknown` + `gaps` 交代缺口——**待核对,不是失败也不是成功**。

## 端到端验收

```bash
node examples/node-client/run_e2e.js --binary <lubancode 可执行文件>
```

五幕:主链路(托管档+幂等+凭据+状态落点+收口)、双 Worker 并行隔离、
飞行中硬杀与重启找回、EOF 孤儿收口与记录可查、空值 env 的 envblock 传递。
已验范围、平台与未验边界见
[固定版本兼容记录](../../docs/reference/app-server-external-client.md)。
