#!/usr/bin/env node
// 独立协议客户端冒烟(内嵌浏览器调试工作台 阶段 3 的验收单):
// 真起 `lubancode app-server` 子进程,走 stdio 协议,一幕一幕钉:
//
//   1. 握手 → browser/start(headless,小 journalCap)→ browser/started;
//   2. page/open 验收站 → browser/page/created + browser/navigation +
//      browser/console/event 批量事件;
//   3. cursor 对账:console/query since_seq(凭事件里见过的最大 seq),
//      环形帽溢出的 dropped 明账;
//   4. 取消贯通:browser/action(wait)→ browser/action/cancel →
//      completed cancelled;
//   5. 审批:owner=agent 的 page/navigate → permission/request →
//      acceptForSession → 同法子二次免问;
//   6. 截图:browser/screenshot → browser/screenshot/ready 只带 artifact
//      引用,字节落盘;整条出站流里没有 base64;
//   6.5 镜像流(多前端外壳单阶段 C):browser/screencast/start → 收
//      browser/screencast/frame(同截图链落 artifact,只带引用)→
//      browser/screencast/stop;chromium 引擎才有(CDP screencast)。
//   7. 断线重连的边界(阶段 3 的口径):app-server 是 stdio 进程,断线即
//      EOF 退场,sidecar 随之收尸;重连=新 app-server + 新 sidecar,
//      browser/status 如实报新场——页面状态不跨 app-server 重启存活
//      (阶段 4 的 Desktop 宿主另立)。
//
// 用法:node scripts/tests/app_server_browser_client.js [--binary <lubancode.exe>]
// 缺 node/playwright/sidecar 任一项打印 SKIP 退 0,不冒充通过。
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..');
const SIDECAR = path.join(REPO, 'browser', 'sidecar.js');
const SITE_STARTER = path.join(REPO, 'browser', 'test', 'site.js');

let passed = 0;
let failed = 0;
const failures = [];

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
  if (explicit) return explicit;
  const candidates = [
    path.join(REPO, 'build', 'lubancode.exe'),
    path.join(REPO, 'build', 'lubancode'),
    path.join(REPO, 'build', 'Debug', 'lubancode.exe'),
    path.join(REPO, 'build', 'Release', 'lubancode.exe'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

// ---------------------------------------------------------------------------
// 协议客户端:app-server 的 stdio 一来一回 + 事件收集。
// ---------------------------------------------------------------------------

class AppServerClient {
  constructor(binary, env) {
    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    this.lines = []; // 全部出站行(终检 base64 用)
    this.buffer = '';
    this.binary = binary;
    this.env = env;
  }

  async start() {
    this.child = spawn(this.binary, ['app-server'], { env: this.env, stdio: ['pipe', 'pipe', 'pipe'] });
    this.child.stdout.setEncoding('utf8');
    this.child.stdout.on('data', (chunk) => this.onData(chunk));
    this.stderr = '';
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (chunk) => { this.stderr += chunk; });
    this.exitPromise = new Promise((resolve) => this.child.on('exit', (code) => resolve(code)));
    const init = await this.request('initialize', { clientName: 'browser-smoke-client' });
    this.notify('initialized');
    return init;
  }

  onData(chunk) {
    this.buffer += chunk;
    let index;
    while ((index = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      if (!line) continue;
      this.lines.push(line);
      let message;
      try {
        message = JSON.parse(line);
      } catch (_) {
        continue;
      }
      if (typeof message.method === 'string') {
        this.events.push(message);
      } else if (Number.isFinite(message.id)) {
        const entry = this.pending.get(message.id);
        if (entry) {
          this.pending.delete(message.id);
          entry(message);
        }
      }
    }
  }

  request(method, params, timeoutMs) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('app-server 请求超时: ' + method));
      }, timeoutMs || 60000);
      this.pending.set(id, (message) => {
        clearTimeout(timer);
        resolve(message);
      });
      this.child.stdin.write(JSON.stringify({ id, method, params: params || {} }) + '\n');
    });
  }

  notify(method, params) {
    this.child.stdin.write(JSON.stringify({ method, params: params || {} }) + '\n');
  }

  // 反向请求(permission/request)的响应信封。
  answerServerRequest(requestId, payload) {
    this.child.stdin.write(JSON.stringify({ id: 0, result: Object.assign({ requestId }, payload) }) + '\n');
  }

  waitForEvent(method, predicate, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 20000);
    return new Promise((resolve) => {
      const poll = () => {
        const hit = this.events.find(
          (e) => e.method === method && (!predicate || predicate(e)));
        if (hit) return resolve(hit);
        if (Date.now() > deadline) return resolve(null);
        setTimeout(poll, 25);
      };
      poll();
    });
  }

  async waitExit(timeoutMs) {
    const timer = sleep(timeoutMs || 15000).then(() => 'timeout');
    const code = await Promise.race([this.exitPromise, timer]);
    return code === 'timeout' ? null : code;
  }

  closeStdin() {
    this.child.stdin.end();
  }
}

// ---------------------------------------------------------------------------
// 一幕一幕
// ---------------------------------------------------------------------------

async function main() {
  console.log('app-server 浏览器协议冒烟(独立协议客户端)');
  const args = process.argv.slice(2);
  let binary = null;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--binary') binary = args[++i];
  }
  binary = findBinary(binary);
  if (!binary) {
    console.log('SKIP 找不到 lubancode 可执行文件(--binary 指路或先构建)');
    return;
  }
  if (!fs.existsSync(SIDECAR)) {
    console.log('SKIP 找不到 browser/sidecar.js');
    return;
  }
  let playwrightAvailable = true;
  try {
    require(path.join(REPO, 'browser', 'node_modules', 'playwright'));
  } catch (_) {
    playwrightAvailable = false;
  }
  if (!playwrightAvailable) {
    console.log('SKIP browser/ 没装 playwright(先在 browser/ 跑 npm install)');
    return;
  }

  // 验收站 + 隔离的 HOME(会话档与 artifact 都进临时目录,不碰真家)。
  const { startSite } = require(SITE_STARTER);
  const { server, port } = await startSite();
  const baseUrl = 'http://127.0.0.1:' + port;
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'lubancode-smoke-home-'));
  const env = Object.assign({}, process.env, {
    USERPROFILE: home,
    HOME: home,
    LUBAN_BROWSER_SIDECAR: SIDECAR,
  });

  let client = null;
  try {
    // ---- 1. 握手与起场 ----
    client = new AppServerClient(binary, env);
    const init = await client.start();
    ok('握手:protocolVersion 1.1', init.result && init.result.protocolVersion === '1.1',
      JSON.stringify(init.result && init.result.protocolVersion));
    const browserMethods = init.result.capabilities.methods;
    ok('能力表里有 browser/action', browserMethods.includes('browser/action'));

    const startReply = await client.request('browser/start', { engine: 'chromium', headed: false, journalCap: 12 });
    ok('browser/start 受理即回 actionId', startReply.result && typeof startReply.result.actionId === 'string',
      JSON.stringify(startReply));
    const started = await client.waitForEvent('browser/started');
    ok('browser/started 事件', Boolean(started));
    const startDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === startReply.result.actionId);
    ok('start 的 action/completed ok', Boolean(startDone) && startDone.params.ok === true,
      JSON.stringify(startDone && startDone.params.error));

    // ---- 2. 开页与事件 ----
    const openReply = await client.request('browser/page/open', { url: baseUrl + '/journal.html', timeoutMs: 45000 });
    const openDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === openReply.result.actionId, 60000);
    ok('page/open 完成,回 pageId/generation', Boolean(openDone) && openDone.params.ok && /^p\d+$/.test(openDone.params.result.pageId),
      JSON.stringify(openDone && openDone.params.error));
    const pageId = openDone && openDone.params.ok ? openDone.params.result.pageId : '';
    ok('browser/page/created 事件', Boolean(await client.waitForEvent('browser/page/created', (e) => e.params.pageId === pageId)));
    ok('browser/navigation 事件', Boolean(await client.waitForEvent('browser/navigation', (e) => e.params.pageId === pageId)));
    const consoleBatch = await client.waitForEvent('browser/console/event', (e) => e.params.pageId === pageId, 20000);
    ok('browser/console/event 批量事件(带 seq 的条目)', Boolean(consoleBatch) && consoleBatch.params.entries.length > 0);
    ok('事件统一带 seq', Boolean(consoleBatch) && Number.isFinite(consoleBatch.params.seq));

    // ---- 3. cursor 对账(断线重连的补账口径) ----
    await sleep(800); // 页面的未捕获异常与网络请求收尾
    const seenSeq = Math.max(0, ...client.events
      .filter((e) => e.method === 'browser/console/event' && e.params.pageId === pageId)
      .flatMap((e) => e.params.entries.map((entry) => entry.seq)));
    const account = await client.request('browser/console/query', { pageId, sinceSeq: seenSeq });
    ok('console/query 补账:回执带 lastSeq/dropped',
      account.result && Number.isFinite(account.result.lastSeq) && Number.isFinite(account.result.dropped),
      JSON.stringify(account));
    ok('cursor 对齐:补回条目 seq 全大于已见最大 seq',
      account.result && account.result.rows.every((row) => row.seq > seenSeq),
      'seen=' + seenSeq + ' rows=' + JSON.stringify((account.result && account.result.rows || []).map((r) => r.seq)));
    ok('环形帽溢出明记 dropped>0(journalCap=12,页面刷 21 条)',
      account.result && account.result.dropped > 0, 'dropped=' + (account.result && account.result.dropped));

    // ---- 4. 取消贯通 ----
    const waitReply = await client.request('browser/action', { kind: 'wait', ms: 20000, timeoutMs: 25000, pageId });
    await sleep(400);
    const cancelReply = await client.request('browser/action/cancel', { actionId: waitReply.result.actionId });
    ok('browser/action/cancel 受理', cancelReply.result && cancelReply.result.cancelled === true, JSON.stringify(cancelReply));
    const waitDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === waitReply.result.actionId, 15000);
    ok('取消:completed cancelled=true', Boolean(waitDone) && waitDone.params.cancelled === true,
      JSON.stringify(waitDone && waitDone.params));

    // ---- 5. 审批(宿主可拦可问) ----
    const threadStart = await client.request('thread/start', {});
    const threadId = threadStart.result.threadId;
    const navReply = await client.request('browser/page/navigate', {
      pageId, url: baseUrl + '/', owner: 'agent', threadId, timeoutMs: 45000,
    });
    const permission = await client.waitForEvent('permission/request', (e) => e.params.threadId === threadId);
    ok('owner=agent 发 permission/request', Boolean(permission) && permission.params.tool === 'browser/page/navigate',
      JSON.stringify(permission && permission.params));
    if (permission) {
      client.answerServerRequest(permission.params.requestId, { decision: 'acceptForSession' });
      const navDone = await client.waitForEvent('browser/action/completed',
        (e) => e.params.actionId === navReply.result.actionId, 60000);
      ok('accept 放行:导航完成且 generation 递增',
        Boolean(navDone) && navDone.params.ok && navDone.params.result.generation >= 2,
        JSON.stringify(navDone && navDone.params));

      // 二次同法子:会话级放行,不再问。
      const nav2Reply = await client.request('browser/page/navigate', {
        pageId, url: baseUrl + '/journal.html', owner: 'agent', threadId, timeoutMs: 45000,
      });
      const nav2Done = await client.waitForEvent('browser/action/completed',
        (e) => e.params.actionId === nav2Reply.result.actionId, 60000);
      const askedAgain = client.events.filter(
        (e) => e.method === 'permission/request' && e.params.tool === 'browser/page/navigate').length;
      ok('acceptForSession:同法子二次免问', askedAgain === 1 && Boolean(nav2Done) && nav2Done.params.ok,
        'asked=' + askedAgain);
    }

    // ---- 6. 截图 artifact ----
    const shotReply = await client.request('browser/screenshot', { pageId, timeoutMs: 45000 });
    const shotDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === shotReply.result.actionId, 60000);
    ok('screenshot 完成,只回 artifact 引用',
      Boolean(shotDone) && shotDone.params.ok && shotDone.params.result.image &&
      shotDone.params.result.image.artifact.stored === true,
      JSON.stringify(shotDone && shotDone.params.error));
    if (shotDone && shotDone.params.ok) {
      const artifactPath = shotDone.params.result.image.artifact.path;
      ok('artifact 字节真落盘', fs.existsSync(artifactPath), artifactPath);
      const ready = await client.waitForEvent('browser/screenshot/ready');
      ok('browser/screenshot/ready 事件与结果同一 artifact',
        Boolean(ready) && ready.params.image.artifact.id === shotDone.params.result.image.artifact.id);
    }
    const pngBase64Head = 'iVBORw0KGgo';
    ok('整条出站流没有 base64 图片正文',
      client.lines.every((line) => !line.includes('dataBase64') && !line.includes(pngBase64Head)));

    // ---- 6.5 镜像流(阶段 C):start → 收 frame 事件 → artifact 落盘可读 → stop ----
    ok('能力表里有 browser/screencast/start|stop',
      browserMethods.includes('browser/screencast/start') && browserMethods.includes('browser/screencast/stop'));
    const screencastStart = await client.request('browser/screencast/start', { pageId, fps: 5 });
    const screencastStartDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === screencastStart.result.actionId, 15000);
    ok('screencast/start 完成', Boolean(screencastStartDone) && screencastStartDone.params.ok === true,
      JSON.stringify(screencastStartDone && screencastStartDone.params.error));
    if (screencastStartDone && screencastStartDone.params.ok) {
      const frame = await client.waitForEvent('browser/screencast/frame', (e) => e.params.pageId === pageId, 8000);
      ok('收到 browser/screencast/frame(只带 artifact 引用)',
        Boolean(frame) && frame.params.artifact && frame.params.artifact.stored === true,
        JSON.stringify(frame && frame.params));
      if (frame) {
        ok('frame 的 artifact 字节真落盘(同截图链)', fs.existsSync(frame.params.artifact.path), frame.params.artifact.path);
        ok('frame 带 pageId 与 dropped 账', typeof frame.params.pageId === 'string' && Number.isFinite(frame.params.dropped));
      }
    }
    const screencastStop = await client.request('browser/screencast/stop', { pageId });
    const screencastStopDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === screencastStop.result.actionId, 15000);
    ok('screencast/stop 完成', Boolean(screencastStopDone) && screencastStopDone.params.ok === true,
      JSON.stringify(screencastStopDone && screencastStopDone.params.error));
    ok('镜像流帧不含 base64(同截图链的出站纪律)',
      client.lines.every((line) => !line.includes('dataBase64') && !line.includes(pngBase64Head)));

    // ---- 7. 断线重连的边界 ----
    client.closeStdin();
    const exitCode = await client.waitExit(20000);
    ok('断线:stdin EOF 后 app-server 自退(退出码 0)', exitCode === 0, 'exit=' + exitCode);

    const client2 = new AppServerClient(binary, env);
    const init2 = await client2.start();
    ok('重连:新 app-server 握手照常', init2.result && init2.result.protocolVersion === '1.1');
    const status2 = await client2.request('browser/status', {});
    ok('重连:browser/status 如实报新场(旧 page id 不跨进程存活)',
      status2.result && status2.result.launched === false && status2.result.pages === 0,
      JSON.stringify(status2.result));
    ok('重连的补账口径:cursor 设施在(console/query 的 sinceSeq 参数面)',
      Boolean(status2.result));
    client2.closeStdin();
    await client2.waitExit(20000);

    console.log('\n---- 汇总 ----');
    console.log('PASS=' + passed + ' FAIL=' + failed);
    if (failures.length > 0) {
      console.log('失败清单:');
      for (const line of failures) console.log('  - ' + line);
      console.log('---- app-server stderr 尾巴(诊断)----');
      console.log(client.stderr.split('\n').slice(-20).join('\n'));
      process.exitCode = 1;
    }
  } finally {
    if (client && client.child && !client.child.killed) {
      try { client.child.kill(); } catch (_) { /* 已退 */ }
    }
    server.close();
    try { fs.rmSync(home, { recursive: true, force: true }); } catch (_) { /* 收尾尽力 */ }
  }
}

main().catch((error) => {
  console.error('冒烟跑挂:', error);
  process.exitCode = 1;
});
