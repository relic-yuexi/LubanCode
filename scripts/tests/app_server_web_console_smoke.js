#!/usr/bin/env node
// 参考前端冒烟(多前端外壳单阶段 D 的验收单)。
//
// 与 stdio/WS 两册不同,这册不手搓协议客户端——直接 require 参考前端的
// 内核(examples/web-console/web_console_core.js,与浏览器页吃同一份代码):
// 页上怎么走协议,冒烟就怎么验。"全程只走协议"由同一份内核作证,不是
// 测试另开一条路。
//
// 一幕一幕:
//   1. 起真 `lubancode app-server --app-server-ws <port>`(隔离 HOME +
//      本地假 Anthropic 后端),参考前端内核连上、握手 1.1;
//   2. 聊天流(四件套之一):thread/start → turn/start → item/* →
//      turn/completed,事件账(ConsoleState)里正文/usage 有账;
//   3. artifact 字节口子(镜像的前置):预放的截图按名取字节(200/
//      Content-Type/字节一致),没这枚/穿越一律 404;
//   4. token 门:另一台配 token 的服务,GET 没带/带错 403,查询串/Bearer
//      带对 200;token 不落任何应答;
//   5. 浏览器四件套(playwright 在场才跑,否则明打 SKIP 不冒充):
//      起场 → 开页 → console/network 账 + query 补账 → 审批弹层
//      (permission/request → acceptForSession,二次免问)→ 镜像流
//      (screencast/start → frame → artifact 字节经 HTTP 口子真是图 →
//      stop)→ 端到端一幕(§八:开页 → 看账 → 点镜像 → Agent 收
//      browser.stale_ref)→ 暂停门(browser/paused + Agent 动作
//      受理不执行);
//   6. 纪律:整条入站流没有 base64 图片正文;exit 收线退出码 0。
//
// 用法:node scripts/tests/app_server_web_console_smoke.js [--binary <lubancode.exe>]
// 缺 node/构建产物打印 SKIP 退 0,不冒充通过。
'use strict';

const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const core = require(path.resolve(__dirname, '..', '..', 'examples', 'web-console', 'web_console_core.js'));

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
    path.join(REPO, 'build', 'debug', 'Debug', 'lubancode.exe'),
    path.join(REPO, 'build', 'lubancode.exe'),
    path.join(REPO, 'build', 'Debug', 'lubancode.exe'),
    path.join(REPO, 'build', 'Release', 'lubancode.exe'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

// ---------------------------------------------------------------------------
// 本地回环的家伙什:假后端 / 隔离 HOME / 起服务 / 等端口 / HTTP GET
// ---------------------------------------------------------------------------

async function startFakeBackend() {
  const server = http.createServer((req, res) => {
    if (req.method !== 'POST' || !req.url.endsWith('/v1/messages')) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
    const sse = (name, object) => res.write('event: ' + name + '\ndata: ' + JSON.stringify(object) + '\n\n');
    sse('message_start', { type: 'message_start', message: { id: 'msg_web_console', model: 'fake-model' } });
    sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
    sse('content_block_delta', {
      type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: '参考前端冒烟的假后端回话' },
    });
    sse('content_block_stop', { type: 'content_block_stop', index: 0 });
    sse('message_delta', {
      type: 'message_delta', delta: { stop_reason: 'end_turn' }, usage: { input_tokens: 21, output_tokens: 13 },
    });
    sse('message_stop', { type: 'message_stop' });
    res.end();
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { server, port: server.address().port };
}

function makeHome(fakeBackendPort) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'lubancode-web-console-home-'));
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

// HTTP GET(artifact 字节口子;token 可走 Bearer 头或查询串)。
function httpGet(port, target, headers) {
  return new Promise((resolve, reject) => {
    const request = http.get(
      { host: '127.0.0.1', port: port, path: target, headers: headers || {} },
      (response) => {
        const chunks = [];
        response.on('data', (chunk) => chunks.push(chunk));
        response.on('end', () => resolve({
          status: response.statusCode,
          contentType: String(response.headers['content-type'] || ''),
          cors: String(response.headers['access-control-allow-origin'] || ''),
          body: Buffer.concat(chunks),
        }));
      });
    request.on('error', reject);
    request.setTimeout(8000, () => { request.destroy(new Error('HTTP GET 超时')); });
  });
}

// 预放一枚 artifact(内容寻址名由冒烟自定;字节用已知合法的 8x8 PNG,
// 与 browser/test/site.js 同一张图——镜像帧/截图落盘就是这个形状)。
const PLANTED_NAME = 'art-0123456789abcdef.png';
const PLANTED_BYTES = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAFUlEQVR4nGP8z8Dwn4GBgYGJ' +
    'gYEBAAEoAQP8ZwLNAAAAAElFTkSuQmCC', 'base64');

function plantArtifact(home, name, bytes) {
  const dir = path.join(home, '.lubancode', 'browser-artifacts');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, name), bytes);
}

// ---------------------------------------------------------------------------
// 一幕一幕
// ---------------------------------------------------------------------------

async function main() {
  console.log('app-server 参考前端冒烟(阶段 D:examples/web-console)');
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
  plantArtifact(home, PLANTED_NAME, PLANTED_BYTES);

  let child = null;
  let gateChild = null;
  let channel = null;
  try {
    const port = 20000 + Math.floor(Math.random() * 20000);
    child = spawnAppServer(binary, home, port, [], { LUBAN_BROWSER_SIDECAR: SIDECAR });
    if (!(await waitReachable(port))) {
      ok('WS 服务起得来', false, '端口等不到: ' + (child.stderrText || '').split('\n').slice(-3).join(' '));
      return;
    }

    // ---- 1. 握手(参考前端内核连真服务) ----
    channel = new core.ProtocolChannel({ port: port, name: 'web-console-smoke' });
    const state = new core.ConsoleState();
    state.attach(channel);
    const init = await channel.connect();
    ok('握手:protocolVersion 1.1(承载同协议)', Boolean(init) && init.protocolVersion === '1.1',
      JSON.stringify(init && init.protocolVersion));
    const methods = (init && init.capabilities && init.capabilities.methods) || [];
    ok('能力表:浏览器方法面在(browser/screencast/start|stop、browser/pause)',
      methods.includes('browser/screencast/start') && methods.includes('browser/screencast/stop') &&
      methods.includes('browser/pause'));

    // ---- 2. 聊天流(四件套之一,事件账作证) ----
    const threadStart = await channel.request('thread/start', {});
    state.currentThreadId = threadStart.result.threadId;
    ok('thread/start:事件账记下会话', state.threads.includes(state.currentThreadId));
    await channel.request('turn/start', { threadId: state.currentThreadId, text: '参考前端冒烟问一句' });
    const turnDone = await channel.waitForEvent('turn/completed',
      (e) => e.threadId === state.currentThreadId, 30000);
    ok('turn/completed status=success(假后端一幕终)',
      Boolean(turnDone) && turnDone.params.status === 'success', JSON.stringify(turnDone && turnDone.params));
    const assistantText = state.transcript()
      .filter((item) => item.type === 'text')
      .map((item) => item.text).join('');
    ok('事件账:正文条目攒齐(item/started+delta+completed)', assistantText.includes('参考前端冒烟的假后端回话'),
      'text=' + JSON.stringify(assistantText));
    ok('事件账:usage 有账(turn/usage 累计)', state.usageTotal.inputTokens >= 21 && state.usageTotal.outputTokens >= 13,
      JSON.stringify(state.usageTotal));

    // ---- 3. artifact 字节口子(镜像的前置;不依赖 playwright) ----
    {
      const good = await httpGet(port, '/artifact/' + PLANTED_NAME);
      ok('artifact GET:字节口子回 200 image/png', good.status === 200 && good.contentType.startsWith('image/png'),
        good.status + ' ' + good.contentType);
      ok('artifact GET:字节与盘上一致', good.body.equals(PLANTED_BYTES),
        good.body.length + 'B vs ' + PLANTED_BYTES.length + 'B');
      ok('artifact GET:CORS 头在(file:// 开页跨源取字节)', good.cors === '*');
      const missing = await httpGet(port, '/artifact/art-ffffffffffff.png');
      ok('artifact GET:没这枚 404', missing.status === 404, String(missing.status));
      const traversal = await httpGet(port, '/artifact/..%2F..%2Fconfig.json');
      ok('artifact GET:穿越一律 404', traversal.status === 404, String(traversal.status));
      const shape = await httpGet(port, '/artifact/not-a-name.png');
      ok('artifact GET:坏形状一律 404', shape.status === 404, String(shape.status));
    }

    // ---- 4. token 门(口子与 WS 同规矩;另一台短命服务) ----
    {
      const gatePort = 20000 + Math.floor(Math.random() * 20000);
      gateChild = spawnAppServer(binary, home, gatePort, ['--app-server-ws-token', 'gate-t0ken']);
      if (await waitReachable(gatePort)) {
        const denied = await httpGet(gatePort, '/artifact/' + PLANTED_NAME);
        ok('token 门:GET 没带 token 403', denied.status === 403, String(denied.status));
        const wrong = await httpGet(gatePort, '/artifact/' + PLANTED_NAME + '?token=wrong');
        ok('token 门:GET 带错 403', wrong.status === 403, String(wrong.status));
        const viaQuery = await httpGet(gatePort, '/artifact/' + PLANTED_NAME + '?token=gate-t0ken');
        ok('token 门:查询串带对 200', viaQuery.status === 200 && viaQuery.body.equals(PLANTED_BYTES),
          String(viaQuery.status));
        const viaBearer = await httpGet(gatePort, '/artifact/' + PLANTED_NAME,
          { Authorization: 'Bearer gate-t0ken' });
        ok('token 门:Bearer 头带对 200', viaBearer.status === 200, String(viaBearer.status));
        ok('token 门:token 不落应答', !viaQuery.body.includes('gate-t0ken') && !denied.body.includes('gate-t0ken'));
        // 参考前端内核走首帧 token 门照样连得上。
        const gated = new core.ProtocolChannel({ port: gatePort, token: 'gate-t0ken', name: 'web-console-gated' });
        const gatedInit = await gated.connect();
        ok('token 门:内核首帧 auth 过门,业务照常', Boolean(gatedInit) && gatedInit.protocolVersion === '1.1');
        gated.notify('exit'); // 这台只验口子,收线
        gateChild.kill();
        gateChild = null;
      } else {
        ok('token 门:配 token 的服务起得来', false, '端口等不到');
      }
    }

    // ---- 5. 浏览器四件套 + 端到端一幕(playwright 在场才跑) ----
    let playwrightAvailable = false;
    try {
      require(path.join(REPO, 'browser', 'node_modules', 'playwright'));
      playwrightAvailable = fs.existsSync(SIDECAR);
    } catch (_) {
      playwrightAvailable = false;
    }
    if (playwrightAvailable) {
      const continued = await runBrowserScenes(channel, state, port, home, ok);
      channel = continued;
    } else {
      console.log('  SKIP 浏览器四件套与端到端一幕(browser/ 没装 playwright;1-4 已钉协议与口子)');
    }

    // ---- 6. 纪律与收线 ----
    ok('纪律:整条入站流没有 base64 图片正文', !channel.sawBase64);
    channel.notify('exit');
    const exitCode = await Promise.race([child.exitPromise, sleep(15000).then(() => 'timeout')]);
    ok('exit 通知:进程自退(退出码 0)', exitCode === 0, 'exit=' + exitCode);
    const stderrTail = child.stderrText;
    channel.close();
    child = null; // 已退,finally 不再杀

    console.log('\n---- 汇总 ----');
    console.log('PASS=' + passed + ' FAIL=' + failed);
    if (failures.length > 0) {
      console.log('失败清单:');
      for (const line of failures) console.log('  - ' + line);
      console.log('---- app-server stderr 尾巴(诊断)----');
      console.log((child && child.stderrText || '').split('\n').slice(-15).join('\n'));
      process.exitCode = 1;
    }
  } finally {
    if (channel && !channel.closed) channel.close();
    if (child && child.exitCode === null) {
      try { child.kill(); } catch (_) { /* 已退 */ }
    }
    if (gateChild && gateChild.exitCode === null) {
      try { gateChild.kill(); } catch (_) { /* 已退 */ }
    }
    fakeBackend.server.close();
    try { fs.rmSync(home, { recursive: true, force: true }); } catch (_) { /* 收尾尽力 */ }
  }
}

// 浏览器四件套与端到端一幕(§八:开页 → 看账 → 点镜像 → Agent 收 stale)。
async function runBrowserScenes(channel, state, port, home, okFn) {
  const { startSite } = require(SITE_STARTER);
  const { server, port: sitePort } = await startSite();
  const baseUrl = 'http://127.0.0.1:' + sitePort;
  const actionDone = (reply, timeout) => channel.waitForEvent('browser/action/completed',
    (e) => e.actionId === reply.result.actionId, timeout || 30000);
  // 审批答复按 requestId 排重:waitForEvent 翻的是全量事件账,答过的
  // 悬起件再答按 stale 收口,而真悬着的那枚就没人答了(动作卡死)。
  const answeredIds = new Set();
  const answerNextPermission = async (decision) => {
    const asked = await channel.waitForEvent('permission/request',
      (e) => !answeredIds.has(e.requestId));
    if (asked) {
      answeredIds.add(asked.params.requestId);
      channel.answerServerRequest(asked.params.requestId, { decision: decision });
    }
    return asked;
  };
  try {
    // 开页(四件套之二:页签账 + Console/Network 面板)。
    await channel.request('browser/start', { engine: 'chromium', headed: false, journalCap: 12 });
    const startedDone = await channel.waitForEvent('browser/action/completed', (e) => e.ok === true, 60000);
    okFn('起场:browser/action/completed ok + 事件账 launched', Boolean(startedDone) && state.browser.launched);
    const openReply = await channel.request('browser/page/open', { url: baseUrl + '/journal.html', timeoutMs: 45000 });
    const openDone = await actionDone(openReply, 60000);
    const pageId = openDone && openDone.params.ok ? openDone.params.result.pageId : '';
    okFn('开页:事件账页签在(page/created)', Boolean(pageId) && Boolean(state.browser.pages[pageId]));
    const consoleBatch = await channel.waitForEvent('browser/console/event', (e) => e.pageId === pageId, 20000);
    okFn('看账:console 批量事件进账(有界/丢帧明记的路)', Boolean(consoleBatch));
    await sleep(800);
    okFn('看账:事件账 console 行数 > 0', (state.browser.console[pageId] || { rows: [] }).rows.length > 0);
    await state.backfillJournals(channel, pageId);
    const networkJournal = state.browser.network[pageId] || { lastSeq: 0 };
    okFn('看账:network/query 补账口径在(lastSeq 有限)', Number.isFinite(networkJournal.lastSeq),
      'lastSeq=' + networkJournal.lastSeq);
    const pageList = await channel.request('browser/page/list', {});
    okFn('页签账:page/list 回行数组且含开的那页',
      Array.isArray(pageList.result) && pageList.result.some((page) => page.pageId === pageId),
      JSON.stringify(pageList.result && pageList.result.length));

    // 审批弹层(四件套之四:permission/request 反向请求)。
    const arbThread = await channel.request('thread/start', {});
    const arbThreadId = arbThread.result.threadId;
    const navReply = await channel.request('browser/page/navigate', {
      pageId: pageId, url: baseUrl + '/', owner: 'agent', threadId: arbThreadId, timeoutMs: 45000,
    });
    const permission = await answerNextPermission('acceptForSession');
    okFn('审批:permission/request 进事件账(弹层吃这个)',
      Boolean(permission) && state.approvals.some((a) => a.requestId === permission.params.requestId));
    // 参考前端的答法(与弹层按钮同一条通道口);答完从弹层账上摘掉。
    const at = state.approvals.findIndex((a) => a.requestId === permission.params.requestId);
    if (at >= 0) {
      state.approvals.splice(at, 1);
    }
    const navDone = await actionDone(navReply, 60000);
    okFn('审批:acceptForSession 放行,导航完成', Boolean(navDone) && navDone.params.ok === true,
      JSON.stringify(navDone && navDone.params.error));
    // 二次同法子免问。
    const nav2 = await channel.request('browser/page/navigate', {
      pageId: pageId, url: baseUrl + '/journal.html', owner: 'agent', threadId: arbThreadId, timeoutMs: 45000,
    });
    const nav2Done = await actionDone(nav2, 60000);
    const askedAgain = channel.events
      .filter((e) => e.method === 'permission/request' &&
        e.params.tool === 'browser/page/navigate' && e.params.threadId === arbThreadId).length;
    okFn('审批:会话级放行,二次免问', askedAgain === 1 && Boolean(nav2Done) && nav2Done.params.ok === true,
      'asked=' + askedAgain);

    // 镜像(四件套之三:screencast 流 + artifact 字节)。
    const castStart = await channel.request('browser/screencast/start', { pageId: pageId, fps: 5 });
    const castStartDone = await actionDone(castStart, 15000);
    okFn('镜像:screencast/start 完成', Boolean(castStartDone) && castStartDone.params.ok === true,
      JSON.stringify(castStartDone && castDoneError(castStartDone)));
    if (castStartDone && castStartDone.params.ok) {
      const frame = await channel.waitForEvent('browser/screencast/frame', (e) => e.pageId === pageId, 8000);
      okFn('镜像:frame 事件只带 artifact 引用(无 base64)', Boolean(frame) && Boolean(frame.params.artifact),
        JSON.stringify(frame && Object.keys(frame.params)));
      const mirror = state.browser.mirror[pageId];
      okFn('镜像:事件账记下最新帧(frameSeq/dropped)', Boolean(mirror) && Number.isFinite(mirror.frameSeq),
        JSON.stringify(mirror && { frameSeq: mirror.frameSeq, dropped: mirror.dropped }));
      if (frame && frame.params.artifact) {
        const bytes = await httpGet(port, '/artifact/' + frame.params.artifact.filename);
        const magic = bytes.body.length > 3 &&
          ((bytes.body[0] === 0xff && bytes.body[1] === 0xd8) ||
            (bytes.body[0] === 0x89 && bytes.body[1] === 0x50));
        okFn('镜像:frame 字节经 /artifact 口子取回且真是图(jpeg/png 魔数)',
          bytes.status === 200 && magic, bytes.status + ' ' + bytes.contentType + ' ' + bytes.body.length + 'B');
      }
    }
    const castStop = await channel.request('browser/screencast/stop', { pageId: pageId });
    const castStopDone = await actionDone(castStop, 15000);
    okFn('镜像:screencast/stop 完成', Boolean(castStopDone) && castStopDone.params.ok === true);

    // 端到端一幕(§八):开页(已开)→ 看账(已看)→ 点镜像 → Agent 收 stale。
    const snapReply = await channel.request('browser/snapshot', { pageId: pageId, timeoutMs: 45000 });
    const snapDone = await actionDone(snapReply, 60000);
    const snapshotId = snapDone && snapDone.params.ok ? snapDone.params.result.snapshotId : '';
    const refs = snapDone && snapDone.params.ok ? core.parseSnapshotRefs(snapDone.params.result.text) : [];
    const noop = refs.find((row) => row.text.includes('按一下'));
    okFn('一幕:快照拿到 snapshotId 与元素清单(参考前端的镜像面板吃它)',
      Boolean(snapshotId) && refs.length > 0, 'snapshotId=' + snapshotId + ' refs=' + refs.length);
    if (snapshotId && noop) {
      // 点镜像:用户路(owner 由内核裁定,不带 threadId、不问审批)。
      const userClick = await channel.request('browser/action',
        { pageId: pageId, kind: 'click', ref: noop.ref, snapshotId: snapshotId, timeoutMs: 45000 });
      const userClickDone = await actionDone(userClick, 45000);
      okFn('一幕:点镜像(用户路)受理即执行,owner=user',
        Boolean(userClickDone) && userClickDone.params.ok === true && userClickDone.params.owner === 'user',
        JSON.stringify(userClickDone && userClickDone.params.error));
      const epoch = await channel.waitForEvent('browser/user_epoch',
        (e) => e.pageId === pageId && e.userEpoch >= 1, 10000);
      okFn('一幕:userEpoch 递(事件账在)', Boolean(epoch) && Number.isFinite(state.browser.userEpoch[pageId]));
      // Agent 拿旧观察动作:stale。
      const agentClick = await channel.request('browser/action', {
        pageId: pageId, kind: 'click', ref: noop.ref, snapshotId: snapshotId,
        owner: 'agent', threadId: arbThreadId, timeoutMs: 45000,
      });
      await answerNextPermission('accept');
      const agentClickDone = await actionDone(agentClick, 45000);
      okFn('一幕:Agent 拿旧 snapshot 动作,收 browser.stale_ref',
        Boolean(agentClickDone) && agentClickDone.params.ok === false &&
          agentClickDone.params.error.code === 'browser.stale_ref',
        JSON.stringify(agentClickDone && agentClickDone.params.error));
    }

    // 暂停门(阶段 B 的灯在参考前端上就有账)。
    await channel.request('browser/pause', {});
    const pausedEvent = await channel.waitForEvent('browser/paused');
    okFn('暂停:browser/paused 进事件账(状态灯吃它)', Boolean(pausedEvent) && state.browser.paused === true);
    const pausedNav = await channel.request('browser/page/navigate', {
      pageId: pageId, url: baseUrl + '/journal.html', owner: 'agent', threadId: arbThreadId, timeoutMs: 45000,
    });
    const pausedNavDone = await actionDone(pausedNav, 30000);
    okFn('暂停:Agent 动作受理不执行(browser.paused)',
      Boolean(pausedNavDone) && pausedNavDone.params.ok === false &&
        pausedNavDone.params.error.code === 'browser.paused',
      JSON.stringify(pausedNavDone && pausedNavDone.params.error));
    await channel.request('browser/resume', {});
    okFn('暂停:resume 后状态灯落', (await channel.waitForEvent('browser/resumed')) !== null &&
      state.browser.paused === false);

    // 收场:浏览器会话收掉,服务留着给主流程 exit。
    await channel.request('browser/stop', {});
    await channel.waitForEvent('browser/stopped', null, 15000);
    okFn('收场:browser/stopped 事件账(页签账清空)', Object.keys(state.browser.pages).length === 0);
    return channel;
  } finally {
    server.close();
  }
}

function castDoneError(done) {
  return done && done.params && done.params.error ? done.params.error.code : null;
}

main().catch((error) => {
  console.error('冒烟跑挂:', error);
  process.exitCode = 1;
});
