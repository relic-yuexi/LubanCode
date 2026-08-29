#!/usr/bin/env node
// 独立 WS 协议客户端冒烟(多前端外壳单阶段 A 的验收单):
// 真起 `lubancode app-server --app-server-ws <port>`,走 WebSocket 承载,
// 一幕一幕钉(与 stdio 版 app_server_browser_client.js 同路数):
//
//   1. token 门(负例):配了 token 的服务,首帧不给/给错——即断;
//   2. 连 WS → HTTP 升级 → initialize(1.1 能力表)→ initialized;
//   3. 开 thread → 发一 turn(本地假 Anthropic 后端,SSE 按脚本吐)→
//      收 turn/started、item/*、turn/completed,事件逐条带 seq 且单调;
//   4. 硬断线(不发 close 帧,直接毁 socket)——服务进程必须活着;
//   5. 重连:重新握手(连接级 dispatcher 重铸),seq 不回卷;
//   6. cursor 补账:workflow/query lastSeq 增量(盘上 journal 夹具);
//   7. 浏览器面等价集:有 playwright 才跑(与 stdio 版同源断言,只换承载);
//   8. exit 通知 → 整场收线,进程自退(退出码 0)。
//
// WS 客户端是手搓的(net + crypto,客户端帧掩码/RFC accept 对账),不引
// 第三方包。缺 node/构建产物打印 SKIP 退 0,不冒充通过。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const net = require('net');
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
    path.join(REPO, 'build', 'debug', 'lubancode.exe'),
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
// 手搓 WS 客户端:一条 TCP + HTTP 升级 + 掩码帧收发。
// ---------------------------------------------------------------------------

class WsClient {
  constructor() {
    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    this.frames = []; // 全部出站前的入站帧原文(终检用)
    this.closed = false;
    this.buffer = Buffer.alloc(0);
    this.inbox = [];
    this.waiters = [];
    this.socket = null;
  }

  async connect(port, token) {
    this.key = crypto.randomBytes(16).toString('base64');
    await new Promise((resolve, reject) => {
      this.socket = net.connect({ host: '127.0.0.1', port }, (err) => (err ? reject(err) : resolve()));
      this.socket.on('error', reject);
    });
    this.socket.on('data', (chunk) => this.onData(chunk));
    this.socket.on('close', () => { this.closed = true; this.wake(); });
    // ---- HTTP 升级 ----
    this.socket.write(
      'GET /ws HTTP/1.1\r\n' +
      'Host: 127.0.0.1:' + port + '\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      'Sec-WebSocket-Key: ' + this.key + '\r\n' +
      'Sec-WebSocket-Version: 13\r\n' +
      '\r\n');
    const header = await this.readUntil('\r\n\r\n');
    if (!/^HTTP\/1\.1 101/.test(header)) {
      throw new Error('升级不是 101: ' + header.split('\r\n')[0]);
    }
    const expectAccept = crypto
      .createHash('sha1')
      .update(this.key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');
    if (!header.includes('Sec-WebSocket-Accept: ' + expectAccept)) {
      throw new Error('Sec-WebSocket-Accept 对不上');
    }
    if (header.includes('permessage-deflate')) {
      throw new Error('服务端不该谈压缩扩展');
    }
    this.upgraded = true;
    this.parseFrames(); // 升级应答之后残余的字节(若有)现在按帧解
    // ---- 首帧 token 门 ----
    if (token !== undefined) {
      this.sendJson({ method: 'app_server/auth', params: { token } });
    }
  }

  readUntil(marker) {
    return new Promise((resolve, reject) => {
      const poll = () => {
        const index = this.buffer.indexOf(Buffer.from(marker));
        if (index >= 0) {
          const text = this.buffer.slice(0, index).toString('utf8');
          const rest = this.buffer.slice(index + marker.length);
          this.buffer = rest;
          resolve(text + marker);
          return;
        }
        if (this.closed) {
          reject(new Error('连接断了(等 ' + marker + ')'));
          return;
        }
        setTimeout(poll, 10);
      };
      poll();
    });
  }

  // 客户端出帧:必须掩码。
  frame(opcode, payload) {
    const mask = crypto.randomBytes(4);
    const length = payload.length;
    let header;
    if (length <= 125) {
      header = Buffer.from([(0x80 | opcode) | 0x00, 0x80 | length]);
    } else if (length <= 0xffff) {
      header = Buffer.alloc(4);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 126;
      header.writeUInt16BE(length, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 127;
      header.writeBigUInt64BE(BigInt(length), 2);
    }
    const masked = Buffer.alloc(length);
    for (let i = 0; i < length; ++i) masked[i] = payload[i] ^ mask[i % 4];
    return Buffer.concat([header, mask, masked]);
  }

  sendJson(message) {
    this.socket.write(this.frame(0x1, Buffer.from(JSON.stringify(message), 'utf8')));
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
      this.sendJson({ id, method, params: params || {} });
    });
  }

  notify(method, params) {
    this.sendJson({ method, params: params || {} });
  }

  // 反向请求(permission/request)的响应信封。
  answerServerRequest(requestId, payload) {
    this.sendJson({ id: 0, result: Object.assign({ requestId }, payload) });
  }

  onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    if (this.upgraded) {
      this.parseFrames(); // 升级完成后才是 WS 帧;之前的字节归 readUntil
    }
  }

  parseFrames() {
    while (true) {
      if (this.buffer.length < 2) return;
      const first = this.buffer[0];
      const second = this.buffer[1];
      const opcode = first & 0x0f;
      let offset = 2;
      let length = second & 0x7f;
      if (length === 126) {
        if (this.buffer.length < 4) return;
        length = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) return;
        length = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      if (this.buffer.length < offset + length) return;
      const payload = this.buffer.slice(offset, offset + length);
      this.buffer = this.buffer.slice(offset + length);
      if (opcode >= 0x8) {
        if (opcode === 0x8) this.closed = true; // 对端收线
        continue;
      }
      if (opcode !== 0x1) continue; // 只收文本
      const text = payload.toString('utf8');
      this.frames.push(text);
      let message;
      try {
        message = JSON.parse(text);
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

  wake() {
    for (const waiter of this.waiters.splice(0)) waiter();
  }

  waitForEvent(method, predicate, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 20000);
    return new Promise((resolve) => {
      const poll = () => {
        const hit = this.events.find((e) => e.method === method && (!predicate || predicate(e)));
        if (hit) return resolve(hit);
        if (this.closed || Date.now() > deadline) return resolve(null);
        setTimeout(poll, 25);
      };
      poll();
    });
  }

  // 硬断线:不发 close 帧,直接毁 socket(网络掉/进程被杀的形状)。
  hardDrop() {
    this.socket.destroy();
    this.closed = true;
  }

  sendClose() {
    try { this.socket.write(this.frame(0x8, Buffer.from([0x03, 0xe8]))); } catch (_) { /* 已断 */ }
    this.socket.end();
  }
}

// ---------------------------------------------------------------------------
// 本地假 Anthropic 后端:POST /v1/messages 回一段按脚本的 SSE。
// ---------------------------------------------------------------------------

async function startFakeBackend() {
  const server = http.createServer((req, res) => {
    if (req.method !== 'POST' || !req.url.endsWith('/v1/messages')) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
    });
    const sse = (name, object) => res.write('event: ' + name + '\ndata: ' + JSON.stringify(object) + '\n\n');
    sse('message_start', { type: 'message_start', message: { id: 'msg_ws_smoke', model: 'fake-model' } });
    sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
    sse('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'WS 冒烟的假后端回话' } });
    sse('content_block_stop', { type: 'content_block_stop', index: 0 });
    sse('message_delta', {
      type: 'message_delta',
      delta: { stop_reason: 'end_turn' },
      usage: { input_tokens: 12, output_tokens: 8 },
    });
    sse('message_stop', { type: 'message_stop' });
    res.end();
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { server, port: server.address().port };
}

// ---------------------------------------------------------------------------
// 起一台 WS 模式的 app-server(隔离 HOME,假后端配置)。
// ---------------------------------------------------------------------------

function makeHome(fakeBackendPort) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'lubancode-ws-smoke-home-'));
  fs.mkdirSync(path.join(home, '.lubancode'), { recursive: true });
  fs.writeFileSync(path.join(home, '.lubancode', 'config.json'), JSON.stringify({
    wire: 'anthropic',
    base_url: 'http://127.0.0.1:' + fakeBackendPort,
    model: 'fake-model',
    api_key: 'fake-key-not-a-secret',
  }));
  return home;
}

function spawnAppServer(binary, home, port, extraArgs, env) {
  const args = ['app-server', '--app-server-ws', String(port)].concat(extraArgs || []);
  const child = spawn(binary, args, {
    env: Object.assign({}, process.env, { USERPROFILE: home, HOME: home }, env || {}),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stderr.setEncoding('utf8');
  child.stderrText = '';
  child.stderr.on('data', (chunk) => { child.stderrText += chunk; });
  child.exitPromise = new Promise((resolve) => child.on('exit', (code) => resolve(code)));
  return child;
}

// 等端口可连(服务起来了)。
async function waitReachable(port, tries) {
  for (let i = 0; i < (tries || 100); ++i) {
    const reached = await new Promise((resolve) => {
      const probe = net.connect({ host: '127.0.0.1', port }, () => { probe.destroy(); resolve(true); });
      probe.on('error', () => resolve(false));
    });
    if (reached) return true;
    await sleep(100);
  }
  return false;
}

// 盘上 journal 夹具:workflow/query 的 cursor 补账对账面。
function seedWorkflowRun(home, runId, eventCount) {
  const runDir = path.join(home, '.lubancode', 'workflow-runs', runId);
  fs.mkdirSync(runDir, { recursive: true });
  const lines = [];
  for (let i = 1; i <= eventCount; ++i) {
    lines.push(JSON.stringify({
      seq: i,
      ts: Date.now(),
      run_id: runId,
      workflow_id: 'wf-smoke',
      node_id: i === 1 ? '' : 'node-a',
      attempt: 1,
      type: i === 1 ? 'run_started' : (i === eventCount ? 'node_completed' : 'node_started'),
      data: {},
    }));
  }
  fs.writeFileSync(path.join(runDir, 'events.jsonl'), lines.join('\n') + '\n');
  return runDir;
}

// ---------------------------------------------------------------------------
// 一幕一幕
// ---------------------------------------------------------------------------

async function main() {
  console.log('app-server WebSocket 承载冒烟(独立协议客户端)');
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

  const fakeBackend = await startFakeBackend();
  const home = makeHome(fakeBackend.port);
  let child = null;
  let client = null;
  try {
    // ---- 1. token 门(负例):另一台短命服务 ----
    {
      const gatePort = 20000 + Math.floor(Math.random() * 20000);
      const gateChild = spawnAppServer(binary, home, gatePort, ['--app-server-ws-token', 'gate-t0ken']);
      if (await waitReachable(gatePort)) {
        const wrong = new WsClient();
        await wrong.connect(gatePort, 'wrong-token');
        await sleep(600);
        ok('token 门:错 token 即断(连接被关)', wrong.closed === true, 'closed=' + wrong.closed);
        const silent = new WsClient();
        await silent.connect(gatePort, 'gate-t0ken');
        const init = await silent.request('initialize', {});
        ok('token 门:对 token 放行,业务照常', init.result && init.result.protocolVersion === '1.1',
          JSON.stringify(init.result && init.result.protocolVersion));
        silent.sendClose();
        // token 不落日志:stderr 里不许有 token 值。
        await sleep(300);
        ok('token 门:token 不落 stderr 日志', !gateChild.stderrText.includes('gate-t0ken'));
        gateChild.kill();
      } else {
        ok('token 门:服务起得来', false, '端口等不到');
      }
    }

    // ---- 2-6:主场 ----
    const port = 20000 + Math.floor(Math.random() * 20000);
    child = spawnAppServer(binary, home, port, [], { LUBAN_BROWSER_SIDECAR: SIDECAR });
    if (!(await waitReachable(port))) {
      ok('WS 服务起得来', false, '端口等不到: ' + (child.stderrText || '').split('\n').slice(-3).join(' '));
      return;
    }

    client = new WsClient();
    await client.connect(port);
    const init = await client.request('initialize', { clientName: 'ws-smoke-client' });
    ok('握手:protocolVersion 1.1', init.result && init.result.protocolVersion === '1.1',
      JSON.stringify(init.result && init.result.protocolVersion));
    ok('能力表里有 browser/action(与 stdio 同一张)', Array.isArray(init.result.capabilities.methods) &&
      init.result.capabilities.methods.includes('browser/action'));
    client.notify('initialized');

    // ---- 3. thread + turn(假后端) ----
    const threadStart = await client.request('thread/start', {});
    const threadId = threadStart.result.threadId;
    ok('thread/start 给 threadId', typeof threadId === 'string' && threadId.length > 0, JSON.stringify(threadStart));
    const turnStart = await client.request('turn/start', { threadId, text: 'WS 冒烟问一句' });
    ok('turn/start 受理即回 turnId', turnStart.result && typeof turnStart.result.turnId === 'string',
      JSON.stringify(turnStart));
    const turnDone = await client.waitForEvent('turn/completed',
      (e) => e.params.threadId === threadId, 30000);
    ok('turn/completed status=success(假后端一幕终)', Boolean(turnDone) && turnDone.params.status === 'success',
      JSON.stringify(turnDone && turnDone.params));
    const eventsSeen = client.events.filter((e) => e.params && Number.isFinite(e.params.seq));
    const seqs = eventsSeen.map((e) => e.params.seq);
    ok('事件逐条带 seq 且单调(连接层统一盖)', seqs.length >= 3 &&
      seqs.every((seq, index) => index === 0 || seq > seqs[index - 1]), 'seqs=' + JSON.stringify(seqs));
    ok('item 事件流在(turn 事件桥原样适用)', client.events.some((e) => e.method === 'item/started') &&
      client.events.some((e) => e.method === 'item/completed'));

    // ---- 4. 硬断线:服务必须活着 ----
    const maxSeqBeforeDrop = Math.max(0, ...seqs);
    client.hardDrop();
    await sleep(800);
    const stillAlive = child.exitCode === null;
    ok('硬断线:服务进程活着(等重连,不学 stdio 的 EOF 自退)', stillAlive, 'exitCode=' + child.exitCode);

    // ---- 5. 重连:重新握手,seq 不回卷 ----
    let client2 = new WsClient();
    await client2.connect(port);
    const init2 = await client2.request('initialize', {});
    ok('重连:重新握手照常(连接级 dispatcher 重铸)', init2.result && init2.result.protocolVersion === '1.1');
    client2.notify('initialized');
    // 握手状态机是连接级的:上一条连 initialize 过,这条不过就不放业务——
    // (这里已重新 initialize,业务应放行。)
    const listAfter = await client2.request('thread/list', {});
    ok('重连:业务放行(thread/list 回表)', listAfter.result && Array.isArray(listAfter.result.threads),
      JSON.stringify(listAfter.result && listAfter.result.threads && list2Count(listAfter)));
    const threadStart2 = await client2.request('thread/start', {});
    const started2 = await client2.waitForEvent('thread/started',
      (e) => e.params.threadId === threadStart2.result.threadId, 10000);
    ok('重连:新事件 seq 越过断线前的最大值(发号不回卷,补账不撞号)',
      Boolean(started2) && started2.params.seq > maxSeqBeforeDrop,
      'before=' + maxSeqBeforeDrop + ' after=' + (started2 && started2.params.seq));

    // ---- 6. cursor 补账:workflow/query 的 lastSeq 增量 ----
    const runId = 'ws-smoke-run-' + Date.now();
    seedWorkflowRun(home, runId, 5);
    const full = await client2.request('workflow/query', { runId });
    ok('补账:workflow/query 全量(lastSeq=0)', full.result && full.result.lastSeq === 5,
      JSON.stringify(full.result && full.result.lastSeq));
    const eventsBefore = client2.events.length;
    const incremental = await client2.request('workflow/query', { runId, lastSeq: 3 });
    const fresh = client2.events.slice(eventsBefore)
      .filter((e) => e.method === 'workflow/event' && e.params.runId === runId)
      .map((e) => e.params.eventSeq);
    ok('补账:cursor(lastSeq=3)只回 4、5 两条', incremental.result && fresh.join(',') === '4,5',
      'fresh=' + JSON.stringify(fresh));

    // ---- 7. 浏览器面等价集(playwright 在才跑) ----
    let playwrightAvailable = false;
    try {
      require(path.join(REPO, 'browser', 'node_modules', 'playwright'));
      playwrightAvailable = fs.existsSync(SIDECAR);
    } catch (_) {
      playwrightAvailable = false;
    }
    if (playwrightAvailable) {
      // 与 stdio 版同源的断言,只换承载;详见 app_server_browser_client.js。
      // 这里只跑方法/事件覆盖的核心集:起场 → 开页 → console 补账 → 审批
      // 反向请求 → 截图 artifact → 断线重连后 console/query 补账。
      const continued = await runBrowserActs(client2, port, ok);
      if (continued) client2 = continued;
    } else {
      console.log('  SKIP 浏览器面等价集(browser/ 没装 playwright,核心集已由 2-6 钉)');
    }

    // ---- 8. exit:整场收线 ----
    client2.notify('exit');
    const exitCode = await Promise.race([child.exitPromise, sleep(15000).then(() => 'timeout')]);
    ok('exit 通知:进程自退(退出码 0)', exitCode === 0, 'exit=' + exitCode);
    client2.hardDrop();
    child = null; // 已退,finally 不再杀

    console.log('\n---- 汇总 ----');
    console.log('PASS=' + passed + ' FAIL=' + failed);
    if (failures.length > 0) {
      console.log('失败清单:');
      for (const line of failures) console.log('  - ' + line);
      process.exitCode = 1;
    }
  } finally {
    if (client && client.socket && !client.socket.destroyed) client.hardDrop();
    if (child && child.exitCode === null) {
      try { child.kill(); } catch (_) { /* 已退 */ }
    }
    fakeBackend.server.close();
    try { fs.rmSync(home, { recursive: true, force: true }); } catch (_) { /* 收尾尽力 */ }
  }
}

function list2Count(listResult) {
  return listResult.result && listResult.result.threads ? listResult.result.threads.length : -1;
}

// 浏览器面的等价核心集(有 playwright 才跑):复用 stdio 版的断言口径。
async function runBrowserActs(client, port, okFn) {
  const { startSite } = require(SITE_STARTER);
  const { server, port: sitePort } = await startSite();
  const baseUrl = 'http://127.0.0.1:' + sitePort;
  try {
    const startReply = await client.request('browser/start', { engine: 'chromium', headed: false, journalCap: 12 });
    const started = await client.waitForEvent('browser/started');
    okFn('browser/started 事件(WS 承载)', Boolean(started));
    const startDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === startReply.result.actionId, 60000);
    okFn('browser/start 的 action/completed ok', Boolean(startDone) && startDone.params.ok === true);

    const openReply = await client.request('browser/page/open', { url: baseUrl + '/journal.html', timeoutMs: 45000 });
    const openDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === openReply.result.actionId, 60000);
    okFn('page/open 完成', Boolean(openDone) && openDone.params.ok, JSON.stringify(openDone && openDone.params.error));
    const pageId = openDone && openDone.params.ok ? openDone.params.result.pageId : '';
    const consoleBatch = await client.waitForEvent('browser/console/event', (e) => e.params.pageId === pageId, 20000);
    okFn('browser/console/event 批量事件(带 seq)', Boolean(consoleBatch) && consoleBatch.params.entries.length > 0);

    await sleep(800);
    const seenSeq = Math.max(0, ...client.events
      .filter((e) => e.method === 'browser/console/event' && e.params.pageId === pageId)
      .flatMap((e) => e.params.entries.map((entry) => entry.seq)));
    const account = await client.request('browser/console/query', { pageId, sinceSeq: seenSeq });
    okFn('console/query 补账:cursor 只回大于已见的', account.result &&
      account.result.rows.every((row) => row.seq > seenSeq));

    const threadStart = await client.request('thread/start', {});
    const threadId = threadStart.result.threadId;
    const navReply = await client.request('browser/page/navigate', {
      pageId, url: baseUrl + '/', owner: 'agent', threadId, timeoutMs: 45000,
    });
    const permission = await client.waitForEvent('permission/request', (e) => e.params.threadId === threadId);
    okFn('审批反向请求经 WS 走通(permission/request)', Boolean(permission) &&
      permission.params.tool === 'browser/page/navigate');
    if (permission) {
      client.answerServerRequest(permission.params.requestId, { decision: 'acceptForSession' });
      const navDone = await client.waitForEvent('browser/action/completed',
        (e) => e.params.actionId === navReply.result.actionId, 60000);
      okFn('accept 放行:导航完成', Boolean(navDone) && navDone.params.ok);
    }

    const shotReply = await client.request('browser/screenshot', { pageId, timeoutMs: 45000 });
    const shotDone = await client.waitForEvent('browser/action/completed',
      (e) => e.params.actionId === shotReply.result.actionId, 60000);
    okFn('screenshot 只回 artifact 引用(无 base64)', Boolean(shotDone) && shotDone.params.ok &&
      shotDone.params.result.image.artifact.stored === true);
    okFn('出站流没有 base64 图片正文', client.frames.every(
      (text) => !text.includes('dataBase64') && !text.includes('iVBORw0KGgo')));

    // WS 专属:断线 → 重连 → console/query 凭 cursor 补账(会话不丢)。
    client.hardDrop();
    await sleep(500);
    const again = new WsClient();
    await again.connect(port);
    const init = await again.request('initialize', {});
    okFn('浏览器断线重连:握手照常', init.result && init.result.protocolVersion === '1.1');
    again.notify('initialized');
    const status = await again.request('browser/status', {});
    okFn('浏览器会话跨连接存活(sidecar 不随断线收尸)', status.result && status.result.launched === true,
      JSON.stringify(status.result));
    const backfill = await again.request('browser/console/query', { pageId, sinceSeq: 0 });
    okFn('重连后 cursor 补账:journal 全量回得来', backfill.result &&
      backfill.result.rows.length > 0 && Number.isFinite(backfill.result.lastSeq),
      JSON.stringify(backfill.result && backfill.result.lastSeq));
    return again; // 交给主流程继续(exit 用)
  } finally {
    server.close();
  }
}

main().catch((error) => {
  console.error('冒烟跑挂:', error);
  process.exitCode = 1;
});
