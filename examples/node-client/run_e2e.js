#!/usr/bin/env node
// 应用Worker接入单 P4:Node 外部客户端端到端验收(AW-02/04/11/12/15/18/20
// 与 §十收口的进程级证据)。
//
// 五幕,全部真 `lubancode app-server` 子进程 + 本地回环假 anthropic 后端,
// 零真实密钥、零真实网络:
//   1. 主链路(托管档):握手钉协议 1.3 与能力表 → thread/start(键)→
//      turn/start(键)→ 收终态 → operation/read 复查(final+稳定正文)→
//      同键同载荷重发回原受理(模型零重跑)/同键异载荷 operation_conflict →
//      thread/start 同键回原场 → 凭据只经参数根 config.json(argv/stdout/
//      stderr/协议结果全无密钥,假后端确实收到钥匙头)→ 状态只落数据根、
//      参数根零写入、个人家目录零读写 → 收口四步(停受理→排空→shutdown→
//      退出码 0)。
//   2. 双 Worker 并行:两套参数根/数据根/假后端并跑,生效快照与结果互不
//      串材料(AW-04 的进程级版)。
//   3. 强杀找回:假后端扣住应答,Worker 在回合飞行中被硬杀(POSIX SIGKILL
//      /Windows TerminateProcess)→ 同根重启 Worker → operation/read 回
//      unknown+gaps 不冒充,thread/start 同键回原场(active=false),模型
//      零重跑(§十"强杀后的执行标为待核对";真拔电不在此列,归真机批次)。
//   4. EOF 孤儿收口:宿主消亡(直接关 stdin)→ Worker 读 EOF 自退(退出码
//      0)→ 同根重启后记录仍能查询(operation/read final + 创建去重)。
//   5. 空值 env 跨进程:Node spawn 的 env 表把空串原样落 envblock——
//      LUBANCODE_DATA_HOME='' / LUBANCODE_HOME='' 均被启动门明拒(退出码
//      1 + stderr 人话)。这是 capability-contract.md §13.1"宿主传空值唯一
//      真路=envblock"的端到端证据(#70 合同留的验收位)。
//
// 用法:node examples/node-client/run_e2e.js [--binary <lubancode>]
// 找不到可执行文件打印 SKIP 退 0,不冒充通过(与 web console 冒烟同口径)。
'use strict';

const fs = require('fs');
const http = require('http');
const path = require('path');

const { LubancodeWorkerClient, makeTempRoot } =
  require(path.resolve(__dirname, 'lubancode_worker_client.js'));

let passed = 0;
let failed = 0;
const failures = [];
const liveWorkers = []; // 诊断底:跑挂时吐各 Worker 的 stderr/命令行

function ok(name, condition, detail) {
  if (condition) {
    ++passed;
    console.log('  PASS ' + name);
  } else {
    ++failed;
    failures.push(name + (detail ? ': ' + detail : ''));
    console.log('  FAIL ' + name + (detail ? ' -- ' + detail : ''));
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function findBinary(explicit) {
  if (explicit) {
    return fs.existsSync(explicit) ? explicit : null;
  }
  const repo = path.resolve(__dirname, '..', '..');
  const candidates = [
    path.join(repo, 'build', 'lubancode'),
    path.join(repo, 'build', 'lubancode.exe'),
    path.join(repo, 'build', 'Debug', 'lubancode.exe'),
    path.join(repo, 'build', 'Release', 'lubancode.exe'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

// ---------------------------------------------------------------------------
// 本地回环假 anthropic 后端:reply 模式按脚本回一幕 SSE;hold 模式收下请求
// 什么都不回(强杀场景的"回合在飞行中"闸)。
// ---------------------------------------------------------------------------

class FakeBackend {
  constructor(options) {
    this.options = options || {};
    this.requests = [];
    this.server = http.createServer((req, res) => {
      const chunks = [];
      req.on('data', (chunk) => chunks.push(chunk));
      req.on('aborted', () => { /* 强杀后断管,忽略 */ });
      res.on('error', () => { /* 同上 */ });
      req.on('end', () => {
        this.requests.push({
          method: req.method,
          url: req.url,
          headers: req.headers,
          body: Buffer.concat(chunks).toString('utf8'),
        });
        if (this.options.mode === 'hold') {
          res.setHeader('Content-Type', 'text/event-stream');
          res.writeHead(200);
          return; // 扣住:不写正文不收尾
        }
        res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
        const sse = (name, object) => res.write('event: ' + name + '\ndata: ' + JSON.stringify(object) + '\n\n');
        sse('message_start', { type: 'message_start', message: { id: 'msg_node_e2e', model: 'fake-model' } });
        sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
        sse('content_block_delta', {
          type: 'content_block_delta', index: 0,
          delta: { type: 'text_delta', text: this.options.replyText || '假后端的回话' },
        });
        sse('content_block_stop', { type: 'content_block_stop', index: 0 });
        sse('message_delta', {
          type: 'message_delta', delta: { stop_reason: 'end_turn' },
          usage: { input_tokens: 17, output_tokens: 7 },
        });
        sse('message_stop', { type: 'message_stop' });
        res.end();
      });
    });
  }

  async listen() {
    await new Promise((resolve) => this.server.listen(0, '127.0.0.1', resolve));
    this.port = this.server.address().port;
    return this;
  }

  baseUrl() {
    return 'http://127.0.0.1:' + this.port;
  }

  // 钥匙头是否到达(anthropic 面 x-api-key 或 Authorization Bearer 任一)。
  receivedCredential(apiKey) {
    if (this.requests.length === 0) {
      return false;
    }
    for (const request of this.requests) {
      const header = request.headers['x-api-key'] || request.headers['authorization'] || '';
      if (String(header).indexOf(apiKey) !== -1) {
        return true;
      }
    }
    return false;
  }

  async waitForRequests(count, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 30000);
    while (this.requests.length < count && Date.now() < deadline) {
      await sleep(50);
    }
    return this.requests.length >= count;
  }

  close() {
    this.server.close();
  }
}

// ---------------------------------------------------------------------------
// 场子搭法:一枚场景根 = config/data/home/ws 四份临时材料。
// ---------------------------------------------------------------------------

function makeField(tag) {
  const root = makeTempRoot(tag);
  const field = {
    root,
    configRoot: path.join(root, 'config'),
    dataRoot: path.join(root, 'state'),
    neutralHome: path.join(root, 'home'),
    workspace: path.join(root, 'ws'),
  };
  fs.mkdirSync(field.workspace, { recursive: true });
  fs.mkdirSync(field.neutralHome, { recursive: true });
  field.cleanup = () => {
    try {
      fs.rmSync(root, { recursive: true, force: true });
    } catch (_) { /* 尽力 */ }
  };
  return field;
}

// 托管部署档(零工具默认面 + 内置 general-purpose 档案):与
// deployment.example.json 同构,e2e 自己落一份进参数根。
const ZERO_TOOL_DEPLOYMENT = {
  schemaVersion: 1,
  service: { mode: 'managed', listeners: { stdio: { enabled: true } }, defaultProfile: 'app' },
  harnessProfiles: {
    app: {
      agentRef: 'general-purpose',
      features: { default: 'disabled', enabled: [], disabled: [] },
      tools: { mode: 'none', deny: [] },
      exposure: { default: 'direct' },
    },
  },
};

function writeDeployment(field) {
  const deploymentPath = path.join(field.configRoot, 'deployment.json');
  fs.mkdirSync(field.configRoot, { recursive: true });
  fs.writeFileSync(deploymentPath, JSON.stringify(ZERO_TOOL_DEPLOYMENT, null, 2));
  return deploymentPath;
}

async function spawnWorker(binary, field, backend, extraOptions) {
  const worker = await LubancodeWorkerClient.spawn(Object.assign({
    binary,
    configRoot: field.configRoot,
    dataRoot: field.dataRoot,
    managed: true,
    deploymentProfile: writeDeployment(field),
    cwd: field.workspace,
    neutralHome: field.neutralHome,
    v3Sessions: true,
    model: { baseUrl: backend.baseUrl(), model: 'fake-model', apiKey: 'sk-node-e2e', wire: 'anthropic' },
  }, extraOptions || {}));
  liveWorkers.push(worker); // 诊断底:跑挂时吐各 Worker 的 stderr/命令行
  return worker;
}

// 跑挂时的现场快照:每只 Worker 的命令行与 stderr 尾巴。
function dumpLiveWorkers() {
  for (const worker of liveWorkers) {
    console.log('---- worker 诊断 ----');
    console.log('argv: ' + worker.argv.join(' '));
    const stderrTail = (worker.stderrText || '').split('\n').slice(-12).join('\n');
    if (stderrTail.trim()) {
      console.log('stderr 尾巴:\n' + stderrTail);
    }
  }
}

// 递归收相对路径清单(参数根零写入断言用)。
function walkRelative(root) {
  const names = [];
  const visit = (dir, prefix) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const rel = prefix ? prefix + '/' + entry.name : entry.name;
      names.push(rel);
      if (entry.isDirectory()) {
        visit(path.join(dir, entry.name), rel);
      }
    }
  };
  visit(root, '');
  return names.sort();
}

// ---------------------------------------------------------------------------
// 幕 1:主链路(托管档)+ 幂等 + 凭据 + 状态落点 + 收口
// ---------------------------------------------------------------------------

async function sceneMainLoop(binary) {
  console.log('\n---- 幕 1:主链路(托管档)----');
  const field = makeField('main');
  const backend = await new FakeBackend({ mode: 'reply', replyText: '主链路假后端的最终答复' }).listen();
  let worker = null;
  try {
    worker = await spawnWorker(binary, field, backend);

    // 握手:协议版本与能力表(报告交接面)。
    const init = await worker.initialize('node-e2e-main');
    ok('握手:protocolVersion 1.3', init.protocolVersion === '1.3', JSON.stringify(init.protocolVersion));
    ok('握手:lubancodeVersion/platform 在场', Boolean(init.lubancodeVersion) && Boolean(init.platform));
    const methods = (init.capabilities && init.capabilities.methods) || [];
    for (const required of ['initialize', 'shutdown', 'thread/start', 'turn/start', 'operation/read']) {
      ok('能力表:methods 含 ' + required, methods.includes(required));
    }

    // 会话创建(幂等键):AW-11 的首棒。
    const CREATE_KEY = 'CREATE-MAIN-1';
    const thread = await worker.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    const threadId = thread.threadId;
    ok('thread/start:回执带 threadId', Boolean(threadId), JSON.stringify(thread));
    ok('thread/start:首发非 duplicate', thread.duplicate !== true);
    ok('thread/started 事件恰一次', worker.countEvents('thread/started', (p) => p.threadId === threadId) === 1);

    // 回合受理(幂等键)+ 终态 + operation/read 复查。
    const TURN_KEY = 'OP-MAIN-1';
    const accepted = await worker.startTurn({ threadId, text: '问一句', clientOperationId: TURN_KEY });
    ok('turn/start 回执:turnId/operationId/inputId 齐',
      Boolean(accepted.turnId) && Boolean(accepted.operationId) && Boolean(accepted.inputId),
      JSON.stringify(accepted));
    const completed = await worker.waitTurnCompleted(threadId, accepted.turnId, 60000);
    ok('turn/completed:executionStatus=success', Boolean(completed) && completed.executionStatus === 'success',
      JSON.stringify(completed));

    const facts = await worker.readOperation({ threadId, clientOperationId: TURN_KEY });
    ok('operation/read:status=final', facts.status === 'final', JSON.stringify(facts.status));
    ok('operation/read:operationId/turnId 与受理回执对账',
      facts.operationId === accepted.operationId && facts.turnId === accepted.turnId);
    ok('operation/read:usageReported/resultEnvelopePersisted 如实',
      facts.usageReported === true && facts.resultEnvelopePersisted === true);
    ok('operation/read:v3 稳定正文与假后端脚本一致',
      facts.sourceFormat === 'v3' && Array.isArray(facts.finalMessages) &&
        facts.finalMessages.length === 1 && facts.finalMessages[0].text === '主链路假后端的最终答复',
      JSON.stringify(facts.finalMessages && facts.finalMessages[0]));

    // 同键同载荷重发:回原受理,模型零重跑,终态不重投(AW-12/15)。
    const retry = await worker.startTurn({ threadId, text: '问一句', clientOperationId: TURN_KEY });
    ok('同键同载荷重发:duplicate=true 回原受理',
      retry.duplicate === true && retry.operationId === accepted.operationId && retry.turnId === accepted.turnId,
      JSON.stringify(retry));
    ok('重发后模型请求数仍为 1', backend.requests.length === 1, String(backend.requests.length));
    ok('重发不重投 turn/completed',
      worker.countEvents('turn/completed', (p) => p.threadId === threadId) === 1);

    // 同键异载荷:operation_conflict,不另起回合。
    let conflict = null;
    try {
      await worker.startTurn({ threadId, text: '换一句', clientOperationId: TURN_KEY });
    } catch (error) {
      conflict = error;
    }
    ok('同键异载荷:operation_conflict(-32602 + data.code)',
      Boolean(conflict) && conflict.code === -32602 && conflict.data && conflict.data.code === 'operation_conflict',
      conflict && (conflict.code + ' ' + JSON.stringify(conflict.data)));
    ok('冲突后模型请求数仍为 1', backend.requests.length === 1, String(backend.requests.length));

    // 会话创建同键重发:回原场身份,不建第二场、不重放 thread/started。
    const createRetry = await worker.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    ok('thread/start 同键重发:duplicate=true 回原场',
      createRetry.duplicate === true && createRetry.threadId === threadId && createRetry.active === true,
      JSON.stringify(createRetry));
    ok('thread/started 仍只一次',
      worker.countEvents('thread/started', (p) => p.threadId === threadId) === 1);

    // 凭据边界(AW-18 抽查):钥匙只经参数根 config.json 走。
    ok('钥匙到达假后端(请求头)', backend.receivedCredential('sk-node-e2e'));
    ok('钥匙不进 argv', worker.argv.join(' ').indexOf('sk-node-e2e') === -1, worker.argv.join(' '));
    ok('钥匙不进协议 stdout', worker.stdoutText.indexOf('sk-node-e2e') === -1);
    ok('钥匙不进 stderr', worker.stderrText.indexOf('sk-node-e2e') === -1);
    ok('钥匙不进 operation/read 结果', JSON.stringify(facts).indexOf('sk-node-e2e') === -1);

    // 状态落点:数据根有账、状态零进参数根、个人家目录零读写(AW-02)。
    // 参数根是材料根,发行提示脚手架会查漏补缺地播种(system_prompt.md/
    // SOUL.md/souls/prompts/,只建新不覆盖)——那是材料不是状态,P1 既定
    // 行为,如实认账;承重断言是"状态目录一个不进参数根"。
    ok('数据根下 workspaces 在场', fs.existsSync(path.join(field.dataRoot, 'workspaces')));
    const configEntries = walkRelative(field.configRoot);
    ok('参数根部署材料在场(config.json/deployment.json)',
      configEntries.includes('config.json') && configEntries.includes('deployment.json'),
      JSON.stringify(configEntries));
    const stateEntries = configEntries.filter((name) =>
      name === 'workspaces' || name.startsWith('workspaces/') ||
      name === 'logs' || name.startsWith('logs/') ||
      name === 'cache' || name.startsWith('cache/') ||
      name === 'data' || name.startsWith('data/') ||
      name === 'workflow-runs' || name.startsWith('workflow-runs/') ||
      name === 'browser-artifacts' || name.startsWith('browser-artifacts/'));
    ok('状态零进参数根(workspaces/logs/cache/data 等一个不在)', stateEntries.length === 0,
      JSON.stringify(stateEntries));
    ok('发行提示脚手架播种在参数根(材料,查漏补缺)',
      configEntries.includes('system_prompt.md') && configEntries.includes('SOUL.md') &&
        configEntries.includes('prompts/core/10-identity.md'),
      JSON.stringify(configEntries.slice(0, 6)));
    ok('个人家目录零读写(无 .lubancode/.agents)',
      !fs.existsSync(path.join(field.neutralHome, '.lubancode')) &&
        !fs.existsSync(path.join(field.neutralHome, '.agents')));

    // 收口四步:停受理 → 排空 → shutdown → 退出码 0。
    worker.beginClose();
    let rejected = null;
    try {
      await worker.startTurn({ threadId, text: '收口后不许再发', clientOperationId: 'OP-LATE' });
    } catch (error) {
      rejected = error;
    }
    ok('停受理:收口后 turn/start 客户端侧拒发', Boolean(rejected) && /收口/.test(rejected.message));
    const idle = await worker.waitIdle();
    ok('排空:在飞回合清零', idle);
    const exit = await worker.shutdown();
    ok('shutdown:进程自退,退出码 0', Boolean(exit) && exit.code === 0,
      JSON.stringify(exit));
    ok('收口后模型请求数仍为 1(全程恰好一轮)', backend.requests.length === 1, String(backend.requests.length));
    worker = null; // 已退,finally 不再收
  } finally {
    if (worker && (!worker.exitInfo)) {
      try { await worker.terminateHard(); } catch (_) { /* 尽力 */ }
    }
    backend.close();
    field.cleanup();
  }
}

// ---------------------------------------------------------------------------
// 幕 2:双 Worker 并行,独立参数根互不串材料(AW-04 进程级)
// ---------------------------------------------------------------------------

async function sceneDualWorkers(binary) {
  console.log('\n---- 幕 2:双 Worker 并行隔离 ----');
  const specs = [
    { tag: 'dual-a', text: 'worker-A-的最终答复', key: 'OP-DUAL-A' },
    { tag: 'dual-b', text: 'worker-B-的最终答复', key: 'OP-DUAL-B' },
  ];
  const rigs = [];
  for (const spec of specs) {
    const field = makeField(spec.tag);
    const backend = await new FakeBackend({ mode: 'reply', replyText: spec.text }).listen();
    rigs.push({ spec, field, backend, worker: null });
  }
  try {
    // 并行起两只、各走完整闭环。
    await Promise.all(rigs.map(async (rig) => {
      rig.worker = await spawnWorker(binary, rig.field, rig.backend);
      await rig.worker.initialize('node-e2e-dual');
      const thread = await rig.worker.startThread({
        clientOperationId: 'CREATE-' + rig.spec.key, cwd: rig.field.workspace,
      });
      rig.threadId = thread.threadId;
      const accepted = await rig.worker.startTurn({
        threadId: thread.threadId, text: '各自问一句', clientOperationId: rig.spec.key,
      });
      rig.turnId = accepted.turnId;
      rig.completed = await rig.worker.waitTurnCompleted(thread.threadId, accepted.turnId, 60000);
    }));

    for (const rig of rigs) {
      ok('[' + rig.spec.tag + '] turn/completed success',
        Boolean(rig.completed) && rig.completed.executionStatus === 'success');
    }
    ok('两只 Worker 各建各场(threadId 不同)',
      rigs[0].threadId !== rigs[1].threadId, rigs[0].threadId + ' vs ' + rigs[1].threadId);

    // 各自的终态与稳定正文互不串。
    for (const rig of rigs) {
      const facts = await rig.worker.readOperation({ threadId: rig.threadId, clientOperationId: rig.spec.key });
      ok('[' + rig.spec.tag + '] operation/read final 且正文是自家后端的',
        facts.status === 'final' && facts.finalMessages && facts.finalMessages.length === 1 &&
          facts.finalMessages[0].text === rig.spec.text,
        JSON.stringify(facts.finalMessages && facts.finalMessages[0]));
      ok('[' + rig.spec.tag + '] 模型请求数恰为 1(没吃对家的流量)', rig.backend.requests.length === 1,
        String(rig.backend.requests.length));
      ok('[' + rig.spec.tag + '] 数据根各落各账', fs.existsSync(path.join(rig.field.dataRoot, 'workspaces')));
    }

    // 两边都体面收口。
    const exits = await Promise.all(rigs.map((rig) => rig.worker.close()));
    for (let i = 0; i < exits.length; ++i) {
      ok('[' + rigs[i].spec.tag + '] 收口退出码 0', exits[i].code === 0, JSON.stringify(exits[i]));
    }
    for (const rig of rigs) {
      rig.worker = null;
    }
  } finally {
    for (const rig of rigs) {
      if (rig.worker && !rig.worker.exitInfo) {
        try { await rig.worker.terminateHard(); } catch (_) { /* 尽力 */ }
      }
      rig.backend.close();
      rig.field.cleanup();
    }
  }
}

// ---------------------------------------------------------------------------
// 幕 3:回合飞行中硬杀 → 同根重启 → unknown 不冒充、零重跑(AW-14/19 进程级)
// ---------------------------------------------------------------------------

async function sceneHardKill(binary) {
  console.log('\n---- 幕 3:飞行中硬杀与重启找回 ----');
  const field = makeField('kill');
  const backend = await new FakeBackend({ mode: 'hold' }).listen();
  let worker = null;
  try {
    worker = await spawnWorker(binary, field, backend);
    await worker.initialize('node-e2e-kill');
    const CREATE_KEY = 'CREATE-KILL-1';
    const TURN_KEY = 'OP-KILL-1';
    const thread = await worker.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    const threadId = thread.threadId;
    const accepted = await worker.startTurn({ threadId, text: '问一句要被硬杀的', clientOperationId: TURN_KEY });

    // 假后端收到请求 = 回合确在飞行中(受理与派发行都已落账)。
    const inFlight = await backend.waitForRequests(1, 30000);
    ok('飞行中:假后端已收到模型请求', inFlight, String(backend.requests.length));

    // 硬杀:POSIX SIGKILL / Windows TerminateProcess,不给清理机会。
    const killed = await worker.terminateHard();
    ok('硬杀:进程确已死(信号或非零退出码)',
      Boolean(killed) && killed.timeout !== true && (Boolean(killed.signal) || killed.code !== 0),
      JSON.stringify(killed));
    worker = null;

    // 同根重启:只读面按原键核对——unknown + gaps,不冒充 cancelled/succeeded。
    const revived = await spawnWorker(binary, field, backend);
    await revived.initialize('node-e2e-kill-revive');
    const facts = await revived.readOperation({ threadId, clientOperationId: TURN_KEY });
    ok('重启核对:status=unknown(强杀后待核对,不伪造终态)', facts.status === 'unknown', JSON.stringify(facts.status));
    ok('重启核对:gaps 交代缺口(no_final_after_dispatch)',
      Array.isArray(facts.gaps) && facts.gaps.join('\n').indexOf('no_final_after_dispatch') !== -1,
      JSON.stringify(facts.gaps));
    ok('重启核对:operationId 与原受理对账', facts.operationId === accepted.operationId,
      facts.operationId + ' vs ' + accepted.operationId);
    ok('重启核对:查询零副作用(模型零重跑)', backend.requests.length === 1, String(backend.requests.length));

    // 会话创建去重跨硬杀:同键回原场,active=false 如实(续跑无门,1.x 面)。
    const dedup = await revived.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    ok('创建去重跨硬杀:回原场不建第二场',
      dedup.duplicate === true && dedup.threadId === threadId && dedup.active === false,
      JSON.stringify(dedup));
    ok('去重后模型请求数仍为 1', backend.requests.length === 1, String(backend.requests.length));

    const exit = await revived.close();
    ok('重启侧收口退出码 0', exit.code === 0, JSON.stringify(exit));
  } finally {
    if (worker && !worker.exitInfo) {
      try { await worker.terminateHard(); } catch (_) { /* 尽力 */ }
    }
    backend.close();
    field.cleanup();
  }
}

// ---------------------------------------------------------------------------
// 幕 4:EOF 孤儿收口(宿主消亡)→ 记录仍能查询
// ---------------------------------------------------------------------------

async function sceneEofOrphan(binary) {
  console.log('\n---- 幕 4:EOF 孤儿收口与记录可查 ----');
  const field = makeField('eof');
  const backend = await new FakeBackend({ mode: 'reply', replyText: 'EOF 幕的最终答复' }).listen();
  let worker = null;
  try {
    worker = await spawnWorker(binary, field, backend);
    await worker.initialize('node-e2e-eof');
    const CREATE_KEY = 'CREATE-EOF-1';
    const TURN_KEY = 'OP-EOF-1';
    const thread = await worker.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    const threadId = thread.threadId;
    const accepted = await worker.startTurn({ threadId, text: '问一句', clientOperationId: TURN_KEY });
    const completed = await worker.waitTurnCompleted(threadId, accepted.turnId, 60000);
    ok('EOF 幕前置:回合已到终态', Boolean(completed) && completed.executionStatus === 'success');

    // 不发 shutdown,直接毁掉 stdin——stdio 宿主消亡的孤儿形态。
    const exit = await worker.closeStdinAndWait(20000);
    ok('EOF:Worker 自退,退出码 0', Boolean(exit) && exit.code === 0, JSON.stringify(exit));
    worker = null;

    // 记录仍能查询(AW-20 尾巴):同根新进程按原键取回终态与创建身份。
    const revived = await spawnWorker(binary, field, backend);
    await revived.initialize('node-e2e-eof-revive');
    const facts = await revived.readOperation({ threadId, clientOperationId: TURN_KEY });
    ok('EOF 后记录可查:status=final + 稳定正文',
      facts.status === 'final' && facts.finalMessages && facts.finalMessages.length === 1 &&
        facts.finalMessages[0].text === 'EOF 幕的最终答复',
      JSON.stringify(facts.finalMessages && facts.finalMessages[0]));
    const dedup = await revived.startThread({ clientOperationId: CREATE_KEY, cwd: field.workspace });
    ok('EOF 后创建去重:回原场 active=false',
      dedup.duplicate === true && dedup.threadId === threadId && dedup.active === false,
      JSON.stringify(dedup));
    ok('EOF 后模型零重跑', backend.requests.length === 1, String(backend.requests.length));

    const reviveExit = await revived.close();
    ok('重启侧收口退出码 0', reviveExit.code === 0, JSON.stringify(reviveExit));
  } finally {
    if (worker && !worker.exitInfo) {
      try { await worker.closeStdinAndWait(5000); } catch (_) { /* 尽力 */ }
      if (!worker.exitInfo) {
        try { await worker.terminateHard(); } catch (_) { /* 尽力 */ }
      }
    }
    backend.close();
    field.cleanup();
  }
}

// ---------------------------------------------------------------------------
// 幕 5:空值 env 跨进程 envblock(启动门明拒)
// ---------------------------------------------------------------------------

async function sceneEmptyEnv(binary) {
  console.log('\n---- 幕 5:空值 env 的跨进程传递(envblock)----');
  const cases = [
    {
      name: 'LUBANCODE_DATA_HOME 空串',
      envOverrides: { LUBANCODE_DATA_HOME: '' },
      expect: 'LUBANCODE_DATA_HOME 不能设为空串',
    },
    {
      name: 'LUBANCODE_HOME 空串',
      envOverrides: { LUBANCODE_HOME: '' },
      expect: 'LUBANCODE_HOME 不能设为空串',
    },
  ];
  for (const testCase of cases) {
    const field = makeField('env');
    const backend = await new FakeBackend({ mode: 'reply' }).listen();
    let worker = null;
    try {
      worker = await spawnWorker(binary, field, backend, { envOverrides: testCase.envOverrides });
      const exit = await worker.waitExit(15000);
      ok('[' + testCase.name + '] 启动门明拒(退出码 1)',
        Boolean(exit) && exit.code === 1, JSON.stringify(exit));
      ok('[' + testCase.name + '] stderr 人话点名列出',
        worker.stderrText.indexOf(testCase.expect) !== -1 && worker.stderrText.indexOf('[config]') !== -1,
        worker.stderrText.split('\n').slice(0, 3).join(' | '));
      worker = null;
    } finally {
      if (worker && !worker.exitInfo) {
        try { await worker.terminateHard(); } catch (_) { /* 尽力 */ }
      }
      backend.close();
      field.cleanup();
    }
  }
}

// ---------------------------------------------------------------------------

async function main() {
  console.log('应用Worker接入单 P4:Node 外部客户端端到端(' + process.platform + ')');
  const args = process.argv.slice(2);
  let binary = null;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--binary') {
      binary = args[++i];
    }
  }
  binary = findBinary(binary);
  if (!binary) {
    console.log('SKIP 找不到 lubancode 可执行文件(--binary 指路或先构建)');
    return;
  }

  await sceneMainLoop(binary);
  await sceneDualWorkers(binary);
  await sceneHardKill(binary);
  await sceneEofOrphan(binary);
  await sceneEmptyEnv(binary);

  console.log('\n---- 汇总 ----');
  console.log('PASS=' + passed + ' FAIL=' + failed);
  if (failures.length > 0) {
    console.log('失败清单:');
    for (const line of failures) {
      console.log('  - ' + line);
    }
    process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error('e2e 跑挂:', error && error.stack ? error.stack : error);
  dumpLiveWorkers();
  process.exitCode = 1;
});
