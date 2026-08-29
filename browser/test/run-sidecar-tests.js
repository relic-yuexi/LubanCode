// Sidecar 自测(单子:内嵌浏览器调试工作台 阶段 3):App Server 的
// BrowserRuntime sidecar——真起 browser/sidecar.js 子进程,走 stdio
// JSON-RPC,钉死:
//
//   进程纪律(不需要 playwright):
//     - 起:session/status 未起浏览器直答(launched=false);
//     - 复用:同一只进程连吃多笔调用;
//     - 收尸:stdin 关闭进程自退(退出码 0),persistent 档 profile 锁释放;
//     - 协议错误:未知方法 -32601;错误信封带 data.browserCode。
//
//   E2E(需要 playwright,缺则逐项 SKIP):
//     - session/start / session/stopped 事件;
//     - page/open -> page/created + page/navigation + journal/console 批量事件;
//     - page/navigate -> generation 递增;
//     - console/network query 的 since_seq 补账与 dropped 明账;
//     - action(click/type/wait)与 cancelled 通知的贯通;
//     - screenshot 回 dataBase64 + sha256(内部通道);
//     - 崩溃终态:杀浏览器子进程 -> session/crashed 事件,旧 page_id 作废。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, execFileSync } = require('child_process');

const SIDECAR = path.resolve(__dirname, '..', 'sidecar.js');

let playwrightAvailable = true;
try {
  require('playwright');
} catch (_) {
  playwrightAvailable = false;
}

function tempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// ---------------------------------------------------------------------------
// SidecarClient:起 sidecar 子进程,说 JSON-RPC,收事件。
// ---------------------------------------------------------------------------

class SidecarClient {
  constructor(extraArgs) {
    this.extraArgs = extraArgs || [];
    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    this.buffer = '';
    this.exitCode = null;
  }

  async start() {
    this.child = spawn(process.execPath, [SIDECAR, ...this.extraArgs], { stdio: ['pipe', 'pipe', 'pipe'] });
    this.child.stdout.setEncoding('utf8');
    this.child.stdout.on('data', (chunk) => this.onData(chunk));
    this.stderr = '';
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (chunk) => { this.stderr += chunk; });
    this.exitPromise = new Promise((resolve) => this.child.on('exit', (code) => resolve(code)));
    // 就绪探测:发一笔 status,回得了就是起来了(返回折好的 result)。
    const message = await this.request('session/status', {});
    return message.result;
  }

  onData(chunk) {
    this.buffer += chunk;
    let index;
    while ((index = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      if (!line) continue;
      let message;
      try {
        message = JSON.parse(line);
      } catch (_) {
        continue;
      }
      if (message.method === 'event') {
        this.events.push(message.params || {});
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
        reject(new Error('sidecar 请求超时: ' + method));
      }, timeoutMs || 30000);
      this.pending.set(id, (message) => {
        clearTimeout(timer);
        resolve(message);
      });
      this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params: params || {} }) + '\n');
    });
  }

  notify(method, params) {
    this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params: params || {} }) + '\n');
  }

  // 成功路:result;错误路抛 { code, message }。
  async call(method, params, timeoutMs) {
    const message = await this.request(method, params, timeoutMs);
    if (message.error) {
      const error = new Error(message.error.message);
      error.code = (message.error.data && message.error.data.browserCode) || String(message.error.code);
      throw error;
    }
    if (message.result && message.result.cancelled) {
      const error = new Error(message.result.message || '已取消');
      error.code = message.result.code || 'browser.cancelled';
      error.cancelled = true;
      throw error;
    }
    return message.result;
  }

  closeStdin() {
    this.child.stdin.end();
  }

  async waitExit(timeoutMs) {
    const timer = sleep(timeoutMs || 15000).then(() => 'timeout');
    const code = await Promise.race([this.exitPromise, timer]);
    this.exitCode = code === 'timeout' ? null : code;
    return this.exitCode;
  }

  // 等一条 type 匹配的事件(带超时)。
  async waitForEvent(type, predicate, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 10000);
    for (;;) {
      const hit = this.events.find((e) => e.type === type && (!predicate || predicate(e)));
      if (hit) return hit;
      if (Date.now() > deadline) return null;
      await sleep(20);
    }
  }
}

// 杀 sidecar 名下的浏览器子进程(崩溃矩阵用;与 run-tests.js 同款)。
function killBrowserChildrenOf(pid) {
  try {
    if (process.platform === 'win32') {
      execFileSync('powershell', [
        '-NoProfile', '-Command',
        "Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq " + pid + " -and $_.Name -match 'chrome|Playwright|WebKit' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }",
      ], { encoding: 'utf8', timeout: 15000 });
      return true;
    }
    execFileSync('pkill', ['-KILL', '-P', String(pid)]);
    return true;
  } catch (_) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// 矩阵本体:helpers 由 run-tests.js 递进来(ok/skip/section)。
// ---------------------------------------------------------------------------

async function runSidecarMatrix(baseUrl, helpers) {
  const { ok, skip, section } = helpers;

  // ---- 进程纪律(不需要 playwright) ----
  section('sidecar 进程纪律(协议级,不需要 playwright)');
  {
    const client = new SidecarClient(['--headless', '--profile', 'ephemeral']);
    const status = await client.start();
    ok('sidecar 起:status 直答且未起浏览器', status.launched === false, JSON.stringify(status));

    const again = await client.call('session/status', {});
    ok('sidecar 复用:第二笔调用同一只进程直答', again.launched === false);

    const unknown = await client.request('no/such-method', {});
    ok('未知方法回 -32601', unknown.error && unknown.error.code === -32601, JSON.stringify(unknown));

    const badPage = await client.request('console/query', { pageId: 'p99' });
    ok('查不存在的页报 browserCode(browser.unknown_page)',
      badPage.error && badPage.error.data && badPage.error.data.browserCode === 'browser.unknown_page',
      JSON.stringify(badPage.error));

    // 取消一只不存在的请求:记账不炸,进程照活。
    client.notify('cancelled', { requestId: 424242 });
    await sleep(50);
    const alive = await client.call('session/status', {});
    ok('取消空通知不炸进程', alive.launched === false);

    client.closeStdin();
    const code = await client.waitExit(15000);
    ok('收尸:stdin 关闭进程自退,退出码 0', code === 0, 'exit=' + code);
  }

  // persistent 档的锁释放(收尸的第二判据)。
  {
    const userData = tempDir('lubancode-sidecar-lock-');
    const client = new SidecarClient(['--headless', '--profile', 'persistent', '--user-data-dir', userData]);
    await client.start();
    ok('persistent 档锁文件在', fs.existsSync(path.join(userData, 'lock')));
    client.closeStdin();
    await client.waitExit(15000);
    // exit 钩子摘锁;给一点文件系统喘气。
    let gone = false;
    for (let i = 0; i < 50 && !gone; ++i) {
      gone = !fs.existsSync(path.join(userData, 'lock'));
      if (!gone) await sleep(50);
    }
    ok('收尸:exit 钩子释放 profile 锁', gone);
    fs.rmSync(userData, { recursive: true, force: true });
  }

  // ---- E2E(需要 playwright) ----
  section('sidecar E2E(engine=chromium,真 Playwright)');
  if (!playwrightAvailable) {
    skip('sidecar E2E 矩阵', 'playwright 依赖未安装');
    return;
  }

  const downloads = tempDir('lubancode-sidecar-dl-');
  const client = new SidecarClient(['--headless', '--profile', 'ephemeral', '--downloads-dir', downloads]);
  await client.start();

  try {
    const started = await client.call('session/start', { engine: 'chromium', headless: true, journalCap: 12 });
    ok('session/start 回 sessionId', typeof started.sessionId === 'string' && started.sessionId.length > 0,
      JSON.stringify(started));
    const startedEvent = await client.waitForEvent('session/started');
    ok('session/started 事件', Boolean(startedEvent));

    // 活会话上二次 start 明拒。
    const second = await client.request('session/start', {});
    ok('活会话二次 start 报 browser.session_running',
      second.error && second.error.data && second.error.data.browserCode === 'browser.session_running',
      JSON.stringify(second.error));

    const opened = await client.call('page/open', { url: baseUrl + '/journal.html', timeoutMs: 30000 });
    ok('page/open 回 pageId 与 generation=1', /^p\d+$/.test(opened.pageId) && opened.generation === 1,
      JSON.stringify(opened));
    const pageId = opened.pageId;

    ok('page/created 事件', Boolean(await client.waitForEvent('page/created', (e) => e.pageId === pageId)));
    ok('page/navigation 事件', Boolean(await client.waitForEvent('page/navigation', (e) => e.pageId === pageId)));

    // journal 批量事件:journal.html 刷 ~21 条 console + 3 发网络请求。
    const consoleBatch = await client.waitForEvent('journal/console', (e) => e.pageId === pageId && e.entries.length > 0, 15000);
    ok('journal/console 批量事件(带 entries)', Boolean(consoleBatch));
    if (consoleBatch) {
      ok('批量事件条目带 seq 与 level',
        consoleBatch.entries.every((e) => Number.isFinite(e.seq) && typeof e.level === 'string'));
      ok('脱敏:token 值被遮', consoleBatch.entries.every((e) => !String(e.text).includes('abc123-should-be-redacted')),
        JSON.stringify(consoleBatch.entries.map((e) => e.text).slice(0, 8)));
    }
    ok('journal/network 批量事件', Boolean(await client.waitForEvent('journal/network', (e) => e.pageId === pageId, 15000)));

    // 等页面安静(未捕获异常与三发请求收尾)。
    await sleep(600);

    // cursor 补账:凭事件里见过的最大 seq 查余账;环形帽(12)小,丢老明记。
    const seenSeq = Math.max(0, ...client.events
      .filter((e) => e.type === 'journal/console' && e.pageId === pageId)
      .flatMap((e) => e.entries.map((entry) => entry.seq)));
    const account = await client.call('console/query', { pageId, sinceSeq: seenSeq });
    ok('console/query 补账:回执带 lastSeq 与 dropped',
      Number.isFinite(account.lastSeq) && Number.isFinite(account.dropped), JSON.stringify(account));
    ok('console/query 补账:补回的条目 seq 全大于 cursor',
      account.rows.every((row) => row.seq > seenSeq), 'cursor=' + seenSeq + ' rows=' + JSON.stringify(account.rows.map((r) => r.seq)));
    ok('环形帽溢出明记 dropped>0(journalCap=12,页面刷了 21 条)', account.dropped > 0, 'dropped=' + account.dropped);

    const networkAccount = await client.call('network/query', { pageId, sinceSeq: 0 });
    ok('network/query 补账:失败请求也在账上',
      networkAccount.rows.some((r) => r.failed) || networkAccount.rows.some((r) => r.status === 500 || r.status === 404),
      JSON.stringify(networkAccount.rows.map((r) => r.status)));

    // 导航:generation +1。
    const navigated = await client.call('page/navigate', { pageId, url: baseUrl + '/', timeoutMs: 30000 });
    ok('page/navigate 回 generation=2', navigated.generation === 2, JSON.stringify(navigated));
    ok('导航事件 generation 递增',
      Boolean(await client.waitForEvent('page/navigation', (e) => e.pageId === pageId && e.generation === 2)));

    // 动作:回首页后点"名字"输入框填字。
    const snapshot = await client.call('snapshot', { pageId, timeoutMs: 30000 });
    const nameRef = (String(snapshot.text || '').split('\n')
      .find((line) => line.includes('名字') && line.includes('[ref=')) || '');
    const refMatch = /\[ref=(e\d+)\]/.exec(nameRef);
    if (refMatch) {
      const typed = await client.call('action', { pageId, kind: 'type', ref: refMatch[1], text: 'sidecar', snapshotId: snapshot.snapshotId, timeoutMs: 30000 });
      ok('action type 回 typed', typed.typed === 'sidecar', JSON.stringify(typed));
    } else {
      ok('action type:快照里找到输入框 ref', false, '没找到 名字 行');
    }

    // 取消贯通:长等待 + cancelled 通知。
    const waitPromise = client.request('action', { pageId, kind: 'wait', ms: 15000, timeoutMs: 20000 });
    await sleep(300);
    const inFlightId = client.nextId - 1;
    client.notify('cancelled', { requestId: inFlightId });
    const waitOutcome = await waitPromise;
    ok('取消:wait 动作按取消收口',
      (waitOutcome.result && waitOutcome.result.cancelled === true) ||
      (waitOutcome.error && waitOutcome.error.data && waitOutcome.error.data.browserCode === 'browser.cancelled'),
      JSON.stringify(waitOutcome));

    // 截图:字节与 sha 对得上(内部通道才有 base64)。
    const shot = await client.call('screenshot', { pageId, timeoutMs: 30000 });
    const bytes = Buffer.from(shot.dataBase64, 'base64');
    ok('screenshot 回字节与 sha256 对得上',
      bytes.length > 0 && crypto.createHash('sha256').update(bytes).digest('hex') === shot.sha256,
      'bytes=' + bytes.length + ' sha=' + shot.sha256);

    // 页签族。
    const tabs = await client.call('page/list', {});
    ok('page/list 回行数组(Array.isArray 且含 page_id)', Array.isArray(tabs) && tabs.some((t) => t.page_id === pageId),
      JSON.stringify(tabs));
    const selected = await client.call('page/select', { pageId });
    ok('page/select 明切', selected.pageId === pageId);
    ok('page/selected 事件', Boolean(await client.waitForEvent('page/selected', (e) => e.pageId === pageId)));

    // 崩溃终态:杀浏览器子进程。
    const pidField = (await client.call('session/status', {})).browserPid;
    if (pidField) {
      killBrowserChildrenOf(pidField);
      const crashed = await client.waitForEvent('session/crashed', null, 20000);
      ok('杀浏览器后 session/crashed 事件', Boolean(crashed), JSON.stringify(crashed));
      const after = await client.call('session/status', {});
      ok('崩溃后 status 如实报 crashed', after.crashed === true);
      const stale = await client.request('snapshot', { pageId });
      ok('崩溃后旧 page_id 明报失效',
        stale.error && stale.error.data && stale.error.data.browserCode === 'browser.page_closed',
        JSON.stringify(stale.error));
      // 换场重启:旧场已崩,start 直接换新。
      const restarted = await client.call('session/start', { engine: 'chromium', headless: true });
      ok('崩溃后 session/start 换新场', typeof restarted.sessionId === 'string');
    } else {
      skip('崩溃矩阵', 'headless-shell 档拿不到 browserPid');
    }

    // 停场:session/stop 事件 + 再起(换场语义)。
    const stopped = await client.call('session/stop', {});
    ok('session/stop 回 stopped', stopped.stopped === true);
    ok('session/stopped 事件', Boolean(await client.waitForEvent('session/stopped')));
    const third = await client.call('session/start', { engine: 'chromium', headless: true });
    ok('停后 session/start 再起', typeof third.sessionId === 'string');

    client.closeStdin();
    const exitCode = await client.waitExit(15000);
    ok('收线:进程退码 0', exitCode === 0, 'exit=' + exitCode);
  } finally {
    try { client.child.kill(); } catch (_) { /* 已退 */ }
    fs.rmSync(downloads, { recursive: true, force: true });
  }
}

module.exports = { runSidecarMatrix, SidecarClient };
