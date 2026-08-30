#!/usr/bin/env node
// LubanCode Browser Runtime sidecar(单子:内嵌浏览器调试工作台 阶段 3)。
//
// App Server 的 C++ 侧是协议转发层,真 Runtime 住在这只 Node 进程里:
//   - 复用 browser/ 的进程协议:stdio 上一行一条 JSON-RPC 2.0(分帧与
//     JSON 形状同 lib/transport.js,与 MCP server 一条路数);
//   - 复用 browser/lib/session.js 的 BrowserSession——Playwright 生命
//     周期、页签账、ref、journal、崩溃终态只有这一份,绝不另起第二份;
//   - 在 MCP 工具面之外多开两条口:服务端主动通知(事件)与取消通知。
//
// 报文形状(host -> sidecar):
//   请求   {"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
//   通知   {"jsonrpc":"2.0","method":"cancelled","params":{"requestId":N}}
// 报文形状(sidecar -> host):
//   应答   {"jsonrpc":"2.0","id":N,"result":{...}} / {"error":{code,message}}
//   事件   {"jsonrpc":"2.0","method":"event","params":{"type":"...","...":...}}
//
// 事件清单(params.type):
//   session/started {sessionId,engine,headless,profile}
//   session/stopped {sessionId}
//   session/crashed {reason}              崩溃终态:旧 page id 全作废
//   page/created {pageId}
//   page/closed {pageId,reason}           reason: closed|crashed
//   page/navigation {pageId,url,generation}
//   page/selected {pageId,url,generation}
//   user_epoch {pageId,userEpoch}         用户动了页面,观察代 +1
//   download/event {...下载账单条}
//   journal/console {pageId,entries:[...],dropped,lastSeq}   批量,溢出丢老明记
//   journal/network {pageId,entries:[...],dropped,lastSeq}
//
// journal 批量规矩:session 每递一条 console/network entry,先攒进该页的
// pending 账;40ms 一冲(或攒满 kJournalBatchCap 即冲)。冲出去的批次再排
// 进发送账,发送账也有帽(kFlightCap)——host 读得慢、账撞帽,丢最老的
// 整批并计数,下一批的 dropped 里明说,不冒充全账。客户端随时可用
// console/query / network/query 凭 since_seq 对账。
//
// stdout 只写协议行;日志全走 stderr(与 server.js 同一条纪律)。
//
// 用法:
//   node sidecar.js [--engine chromium|webkit] [--headed|--headless]
//                   [--profile persistent|ephemeral] [--profile-name <名>]
//                   [--user-data-dir <目录>] [--downloads-dir <目录>]
//                   [--viewport WxH] [--action-timeout-ms <毫秒>]
//                   [--journal-cap <每页账帽>]
// 参数只作缺省;session/start 请求可以逐项覆盖(engine/headless/profile/
// profileName/viewport/journalCap/actionTimeoutMs),覆盖只在起会话时生效。

'use strict';

const { parseArgs, buildConfig, rejectDailyProfileDir, log } = require('./lib/config');
const { BrowserSession, BrowserError, errorShape, describeError } = require('./lib/session');
const { startStdioTransport } = require('./lib/transport');

// journal 批量参数:单批条帽与在飞批数帽。撞在飞帽丢最老整批,明记 dropped。
const JOURNAL_BATCH_CAP = 200;   // 单批最多条数
const JOURNAL_FLUSH_MS = 40;     // 冲批间隔
const JOURNAL_FLIGHT_CAP = 64;   // 每页每账的在飞批数帽(host 读慢时的闸)

const args = parseArgs(process.argv.slice(2));
let config = buildConfig(args);

const dailyClash = rejectDailyProfileDir(config.userDataDir);
if (dailyClash) {
  log('拒绝启动:user-data-dir 指向用户日常浏览器 profile(' + dailyClash + ')。');
  process.stderr.write('user-data-dir 指向日常浏览器 profile,拒绝启动\n');
  process.exit(2);
}

let session = new BrowserSession(config);
// 会话活性旗:start 置位,stop/崩溃清零。不能用 session.context 判活——
// 浏览器是懒启动的,配置好但还没开时 context 仍是 null。
let sessionActive = false;
// persistent 档提前拿锁:第二只 sidecar 抢同目录立即失败,不损坏目录。
if (config.profileMode === 'persistent') {
  try {
    session.lockProfile();
  } catch (error) {
    const message = error instanceof BrowserError ? error.message : String(error.message || error);
    process.stderr.write(message + '\n');
    process.exit(3);
  }
}

// ---------------------------------------------------------------------------
// journal 批量账:per (pageId, kind) 的 pending entries + 在飞批次队列
// ---------------------------------------------------------------------------

const journalState = new Map(); // pageId -> { console: {...}, network: {...} }

function journalSlot(pageId, kind) {
  let perPage = journalState.get(pageId);
  if (!perPage) {
    perPage = { console: makeSlot(), network: makeSlot() };
    journalState.set(pageId, perPage);
  }
  return perPage[kind];
}

function makeSlot() {
  return {
    pending: [],      // 攒着的 entries
    flights: [],      // 已冲出、还没写完 stdout 的批次(send 的返回账)
    dropped: 0,       // 在飞帽溢出累计丢掉的条数(下一批明报)
  };
}

function sendLine(message) {
  // write 返回 false = 内部缓冲涨了(host 读得慢);true/false 都不算完成,
  // 真正的背压闸靠 flights 帽。写抛错(sidecar 收线途中)吞掉:协议路已经
  // 没了,批次丢了有 query 补账,不炸进程。
  try {
    return process.stdout.write(JSON.stringify(message) + '\n');
  } catch (_) {
    return false;
  }
}

function flushJournalSlot(pageId, kind, slot) {
  if (slot.pending.length === 0 && slot.dropped === 0) return;
  const entries = slot.pending;
  slot.pending = [];
  const dropped = slot.dropped;
  slot.dropped = 0;
  let lastSeq = 0;
  for (const entry of entries) {
    if (entry.seq > lastSeq) lastSeq = entry.seq;
  }
  slot.flights.push({ entries: entries.length, dropped });
  if (slot.flights.length > JOURNAL_FLIGHT_CAP) {
    // 撞在飞帽:丢最老一批,条数进 dropped 账,下一批明报。
    const evicted = slot.flights.shift();
    slot.dropped += evicted.entries + (evicted.dropped || 0);
  }
  sendLine({
    jsonrpc: '2.0',
    method: 'event',
    params: {
      type: 'journal/' + kind,
      pageId,
      entries,
      dropped,
      lastSeq,
    },
  });
}

const flushTimer = setInterval(() => {
  for (const [pageId, perPage] of journalState) {
    flushJournalSlot(pageId, 'console', perPage.console);
    flushJournalSlot(pageId, 'network', perPage.network);
  }
}, JOURNAL_FLUSH_MS);
flushTimer.unref();

// ---------------------------------------------------------------------------
// 事件口:session 事件 -> journal 进批量账,其余直发
// (换场后重挂,见 wireEventListener)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 取消账(与 server.js 同款):id -> { cancelled };轮询型动作见旗即停。
// ---------------------------------------------------------------------------

const pendingCancellations = new Set();
const activeCalls = new Map();

function tokenFor(id) {
  const state = { cancelled: false };
  activeCalls.set(id, state);
  return { get cancelled() { return state.cancelled; } };
}

// ---------------------------------------------------------------------------
// 方法表
// ---------------------------------------------------------------------------

function requireString(params, key) {
  const value = params[key];
  if (value === undefined || value === null || value === '') return '';
  return String(value);
}

// session/start:起一场会话。已有一场活会话(未停未崩)报 browser.session_running;
// 停过/崩过的旧场直接换新。params 覆盖 argv 缺省,只在此刻生效。
async function handleSessionStart(params) {
  if (sessionActive && !session.crashed) {
    throw new BrowserError('browser.session_running', '已有一场浏览器会话在跑;先 session/stop 再 start。');
  }
  const merged = { ...args };
  if (params.engine === 'chromium' || params.engine === 'webkit') merged.engine = params.engine;
  if (typeof params.headless === 'boolean') merged.headless = params.headless;
  if (params.profile === 'persistent' || params.profile === 'ephemeral') merged.profile = params.profile;
  if (params.profileName) merged.profileName = String(params.profileName);
  if (params.userDataDir) merged.userDataDir = String(params.userDataDir);
  if (params.downloadsDir) merged.downloadsDir = String(params.downloadsDir);
  if (params.viewport && Number.isFinite(Number(params.viewport.width)) && Number.isFinite(Number(params.viewport.height))) {
    merged.viewport = Number(params.viewport.width) + 'x' + Number(params.viewport.height);
  }
  if (Number.isFinite(Number(params.journalCap))) merged.journalCap = Number(params.journalCap);
  if (Number.isFinite(Number(params.actionTimeoutMs))) merged.actionTimeoutMs = Number(params.actionTimeoutMs);
  config = buildConfig(merged);
  const clash = rejectDailyProfileDir(config.userDataDir);
  if (clash) {
    throw new BrowserError('browser.profile_rejected', 'user-data-dir 指向用户日常浏览器 profile,拒绝:' + clash);
  }
  // 换场:旧 session 的锁与残余先收掉(它已停/已崩,shutdown 幂等)。
  await session.shutdown();
  journalState.clear();
  session = new BrowserSession(config);
  if (config.profileMode === 'persistent') {
    session.lockProfile(); // 抢不到直接抛 browser.profile_locked
  }
  wireEventListener();
  sessionActive = true;
  const status = await session.status();
  const payload = {
    sessionId: config.sessionId,
    engine: config.engine,
    headless: config.headless,
    profile: config.profileMode,
    profileName: config.profileName,
  };
  sendLine({ jsonrpc: '2.0', method: 'event', params: { type: 'session/started', ...payload } });
  return { ...payload, status };
}

async function handleSessionStop() {
  if (!sessionActive && !session.context) {
    return { stopped: true, sessionId: config.sessionId, launched: false };
  }
  const sessionId = config.sessionId;
  sessionActive = false;
  await session.shutdown();
  journalState.clear();
  sendLine({ jsonrpc: '2.0', method: 'event', params: { type: 'session/stopped', sessionId } });
  return { stopped: true, sessionId };
}

// 事件口在换场后要重挂(session 对象换了)。
function wireEventListener() {
  session.setEventListener((type, payload) => {
    if (type === 'session/crashed') {
      sessionActive = false; // 崩溃终态:换场不须先 stop
    }
    if (type === 'console/entry' || type === 'network/entry') {
      const kind = type === 'console/entry' ? 'console' : 'network';
      const slot = journalSlot(payload.pageId, kind);
      slot.pending.push(payload.entry);
      if (slot.pending.length >= JOURNAL_BATCH_CAP) {
        flushJournalSlot(payload.pageId, kind, slot);
      }
      return;
    }
    sendLine({ jsonrpc: '2.0', method: 'event', params: { type, ...payload } });
  });
}

const methods = {
  'session/start': handleSessionStart,
  'session/stop': handleSessionStop,
  'session/status': async () => ({ sessionId: config.sessionId, ...(await session.status()) }),
  'page/open': (params) => session.open(requireString(params, 'url'), {
    newPage: params.newPage,
    waitUntil: params.waitUntil,
    timeoutMs: params.timeoutMs,
  }),
  'page/navigate': (params) => session.navigate(requireString(params, 'pageId'), requireString(params, 'url'), {
    waitUntil: params.waitUntil,
    timeoutMs: params.timeoutMs,
  }),
  'page/back': (params) => session.historyNav(requireString(params, 'pageId'), 'back', { timeoutMs: params.timeoutMs }),
  'page/forward': (params) => session.historyNav(requireString(params, 'pageId'), 'forward', { timeoutMs: params.timeoutMs }),
  'page/reload': (params) => session.historyNav(requireString(params, 'pageId'), 'reload', { timeoutMs: params.timeoutMs }),
  'page/list': () => session.tabs(),
  'page/select': (params) => session.selectPage(requireString(params, 'pageId'), { timeoutMs: params.timeoutMs }),
  'page/close': (params) => session.closePage(requireString(params, 'pageId'), { timeoutMs: params.timeoutMs }),
  'snapshot': (params) => session.snapshot(params.pageId || null, {
    maxChars: params.maxChars,
    timeoutMs: params.timeoutMs,
  }),
  'screenshot': async (params) => {
    const shot = await session.screenshot(params.pageId || null, {
      fullPage: params.fullPage,
      ref: params.ref,
      snapshotId: params.snapshotId,
      timeoutMs: params.timeoutMs,
    });
    // 字节只走 sidecar 内部通道(host 落 artifact 后给前端发引用,绝不把
    // base64 发到 App Server 协议上)。
    return {
      pageId: shot.pageId,
      url: shot.url,
      generation: shot.generation,
      fullPage: shot.fullPage,
      sha256: shot.sha256,
      bytes: shot.buffer.length,
      dataBase64: shot.buffer.toString('base64'),
    };
  },
  'action': (params, token) => {
    const pageId = params.pageId || null;
    const opts = {
      snapshotId: params.snapshotId,
      timeoutMs: params.timeoutMs,
      // owner 原样转给 session(多前端外壳单阶段 B):owner=user 的输入
      // 动作收尾时递观察代——Agent 拿旧 snapshot 的 ref 即 stale。host
      // 侧按连接裁定后才发,这里只信 host。
      owner: params.owner,
    };
    switch (params.kind) {
      case 'click':
        return session.click(pageId, requireString(params, 'ref'), opts);
      case 'type':
        return session.type(pageId, requireString(params, 'ref'), requireString(params, 'text'), {
          ...opts,
          mode: params.mode,
          pressEnter: params.pressEnter,
        });
      case 'select':
        return session.select(pageId, requireString(params, 'ref'), {
          ...opts,
          value: params.value,
          label: params.label,
        });
      case 'wait':
        return session.wait(pageId, {
          forText: params.forText,
          urlContains: params.urlContains,
          ms: params.ms,
          timeoutMs: params.timeoutMs,
          token,
        });
      default:
        throw new BrowserError('browser.schema', 'action.kind 只认 click|type|select|wait,收到: ' + JSON.stringify(params.kind));
    }
  },
  'console/query': (params) => session.consoleEntries(requireString(params, 'pageId'), {
    sinceSeq: params.sinceSeq,
    level: params.level,
    limit: params.limit,
  }),
  'network/query': (params) => session.networkEntries(requireString(params, 'pageId'), {
    sinceSeq: params.sinceSeq,
    urlContains: params.urlContains,
    status: params.status,
    failedOnly: params.failedOnly,
    limit: params.limit,
  }),
  'downloads/query': () => session.listDownloads(),
};

// ---------------------------------------------------------------------------
// 传输与生命周期
// ---------------------------------------------------------------------------

// 初始场也挂上事件口(换场时 handleSessionStart 会重挂)。
wireEventListener();

const transport = startStdioTransport((message) => {
  const method = message && message.method;
  if (method === 'cancelled') {
    const requestId = message.params && Number(message.params.requestId);
    if (Number.isFinite(requestId)) {
      pendingCancellations.add(requestId);
      const inflight = activeCalls.get(requestId);
      if (inflight) inflight.cancelled = true;
      log('收到取消通知:requestId=', requestId, inflight ? '(轮询型动作见旗即停)' : '(无在飞调用,记账)');
    }
    return;
  }
  const id = message && message.id;
  if (typeof method !== 'string' || id === undefined) return;
  const handler = methods[method];
  if (!handler) {
    transport.sendError(id, -32601, 'unknown method: ' + method);
    return;
  }
  const token = tokenFor(id);
  // handler 的同步抛错(如 consoleEntries 的 unknown_page)也要折进
  // Promise 链,不能让它掀了 stdin 的 data 处理器。
  Promise.resolve().then(() => handler(message.params || {}, token)).then(
    (result) => {
      if (pendingCancellations.delete(id)) {
        transport.sendResult(id, { cancelled: true, code: 'browser.cancelled', message: '动作已取消(页面未判死,可继续操作)。' });
        return;
      }
      transport.sendResult(id, result === undefined ? {} : result);
    },
    (error) => {
      if (pendingCancellations.delete(id)) {
        transport.sendResult(id, { cancelled: true, code: 'browser.cancelled', message: '动作已取消(页面未判死,可继续操作)。' });
        return;
      }
      const { code, message } = errorShape(error);
      log('method', method, 'failed:', code, message);
      // 错误码走 data.browserCode(线上的 code 段是 JSON-RPC 号,浏览器
      // 稳定码另递一层,host 侧照它折协议错误)。
      transport.send({ jsonrpc: '2.0', id, error: { code: -32000, message, data: { browserCode: code } } });
    },
  ).finally(() => {
    activeCalls.delete(id);
  }).catch((error) => {
    // sendResult/sendError 自己炸(收线途中):打日志,不炸进程。
    log('回执失败:', describeError(error));
  });
});

let shuttingDown = false;
async function shutdown(code) {
  if (shuttingDown) return;
  shuttingDown = true;
  clearInterval(flushTimer);
  try {
    // 冲掉攒着的 journal 批,再收浏览器。
    for (const [pageId, perPage] of journalState) {
      flushJournalSlot(pageId, 'console', perPage.console);
      flushJournalSlot(pageId, 'network', perPage.network);
    }
    await session.shutdown();
  } finally {
    session.releaseLockNow(); // shutdown 已释放过则幂等空转
    process.exit(code || 0);
  }
}

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('exit', () => {
  session.releaseLockNow();
});
process.stdin.on('end', () => shutdown(0));
process.stdin.on('close', () => shutdown(0));

log('browser sidecar ready:', { engine: config.engine, headless: config.headless, profile: config.profileMode, pid: process.pid });
