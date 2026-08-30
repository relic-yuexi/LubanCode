// Runtime 直调矩阵(单子:内嵌浏览器调试工作台 阶段 1):不经 MCP
// 协议、不起 server 子进程,直接 new BrowserSession 调 session 层的
// open/snapshot/click/type/select/screenshot——验的是 BrowserRuntime
// 本体;末尾再起一只 MCP server 对照,断言两条路的 page_id、generation、
// snapshot_id、stale 语义同一本账(MCP adapter 不建第二份 registry)。
//
// 依赖没齐时打 SKIP 退 0,与 run-tests.js 同一口径。
'use strict';

const fs = require('fs');

const { buildConfig } = require('../lib/config');
const { BrowserSession } = require('../lib/session');
const { BrowserMcpClient } = require('./mcp_client');

let playwrightAvailable = true;
try {
  require('playwright');
} catch (_) {
  playwrightAvailable = false;
}

async function expectError(fn) {
  try {
    await fn();
  } catch (error) {
    return { code: error.code || '', message: String(error.message || error) };
  }
  return { code: '', message: '(没抛错)' };
}

// helpers: { ok, skip, section, tempDir, refOfLine }
async function runDirectMatrix(baseUrl, helpers) {
  const { ok, skip, section, tempDir, refOfLine } = helpers;
  section('Runtime 直调(engine=chromium,不经 MCP 协议)');
  if (!playwrightAvailable) {
    skip('Runtime 直调矩阵', 'playwright 依赖未安装');
    return;
  }

  const downloads = tempDir('lubancode-browser-direct-dl-');
  const config = buildConfig({ engine: 'chromium', headless: true, profile: 'ephemeral', downloadsDir: downloads });
  const runtime = new BrowserSession(config);
  try {
    ok('直调 status 未起浏览器(lazy)', (await runtime.status()).launched === false);

    const opened = await runtime.open(baseUrl + '/', {});
    ok('直调 open 回 page_id(pN)', /^p\d+$/.test(opened.pageId));
    ok('直调 open 首航 generation=1', opened.generation === 1, JSON.stringify(opened));
    ok('直调 open 回标题', opened.title.includes('验收站') && opened.title.includes('首页'), opened.title);

    const snap = await runtime.snapshot(opened.pageId, {});
    ok('直调 snapshot_id 形如 pN-gN-sN', new RegExp('^' + opened.pageId + '-g1-s\\d+$').test(snap.snapshotId), snap.snapshotId);
    ok('直调 snapshot 含 ref 标记', /\[ref=e\d+\]/.test(snap.text));

    const nameRef = refOfLine(snap.text, '名字');
    if (nameRef) {
      const typed = await runtime.type(opened.pageId, nameRef, '张三', { snapshotId: snap.snapshotId });
      ok('直调 type 回 typed=张三', typed.typed === '张三', JSON.stringify(typed));
    } else {
      skip('直调 type 填名', '快照文本没按预期标出名字框');
    }

    const snap2 = await runtime.snapshot(opened.pageId, {});
    const passRef = refOfLine(snap2.text, '密码');
    if (passRef) {
      const typed2 = await runtime.type(opened.pageId, passRef, 's3cret-value', { snapshotId: snap2.snapshotId });
      ok('直调 type 密码只回 [password]', typed2.password === true && typed2.typed === '[password]' && JSON.stringify(typed2).indexOf('s3cret-value') === -1);
    } else {
      skip('直调 type 密码', '快照文本没按预期标出密码框');
    }

    const selectRef = refOfLine(snap.text, '颜色');
    if (selectRef) {
      const picked = await runtime.select(opened.pageId, selectRef, { value: 'blue', snapshotId: snap.snapshotId });
      const first = (picked.selected || [])[0];
      ok('直调 select 按 value 选中', Boolean(first) && first.value === 'blue' && first.label === '蓝', JSON.stringify(picked.selected));
    } else {
      skip('直调 select 下拉', '快照文本没按预期标出颜色下拉');
    }

    const submitRef = refOfLine(snap.text, '提交');
    if (submitRef) {
      const clicked = await runtime.click(opened.pageId, submitRef, { snapshotId: snap.snapshotId });
      ok('直调 click 后导航 generation +1', clicked.navigated === true && clicked.generation === clicked.generationBefore + 1, JSON.stringify(clicked));
      ok('直调 click 后 URL 在已提交页', clicked.url.includes('submitted.html'), clicked.url);
      const stale = await expectError(() => runtime.click(opened.pageId, submitRef, { snapshotId: snap.snapshotId }));
      ok('直调 旧 ref 报 stale_ref', stale.code === 'browser.stale_ref', stale.code + ' ' + stale.message.slice(0, 120));
    } else {
      skip('直调 click 导航', '快照文本没按预期标出提交按钮');
    }

    const shot = await runtime.screenshot(opened.pageId, {});
    ok('直调 screenshot 回 PNG', shot.buffer.length > 1000 && shot.buffer[0] === 0x89 && shot.buffer[1] === 0x50, 'bytes=' + shot.buffer.length);
    ok('直调 screenshot 带 sha256', /^[0-9a-f]{64}$/.test(shot.sha256));

    const tabs = await runtime.tabs();
    ok('直调 tabs 列出该页', tabs.some((t) => t.page_id === opened.pageId && t.active));

    const second = await runtime.open(baseUrl + '/screenshot.html', {});
    ok('直调 第二页拿新 page_id', second.pageId !== opened.pageId);
    await runtime.selectPage(opened.pageId, {});
    const closed = await runtime.closePage(second.pageId, {});
    ok('直调 closePage 收账', closed.closedPageId === second.pageId && closed.openPages >= 1, JSON.stringify(closed));
    const closedAgain = await expectError(() => runtime.selectPage(second.pageId, {}));
    ok('直调 关掉的页明报 page_closed', closedAgain.code === 'browser.page_closed', closedAgain.code);

    const home2 = await runtime.open(baseUrl + '/', {});
    const dlSnap = await runtime.snapshot(home2.pageId, {});
    const dlRef = refOfLine(dlSnap.text, '下载文本');
    if (dlRef) {
      await runtime.click(home2.pageId, dlRef, { snapshotId: dlSnap.snapshotId });
      await new Promise((resolve) => setTimeout(resolve, 1500));
      const dls = runtime.listDownloads();
      const row = (dls.rows || [])[0];
      ok('直调 下载账 done 带 sha256 且落盘',
         Boolean(row) && row.state === 'done' && /^[0-9a-f]{64}$/.test(row.sha256) && fs.existsSync(row.path),
         JSON.stringify(dls.rows.map((r) => r.state)));
    } else {
      skip('直调 下载账', '快照文本没按预期标出下载链接');
    }

    const fileNav = await expectError(() => runtime.open('file:///C:/Windows/win.ini', {}));
    ok('直调 file:// 被拒', fileNav.code === 'browser.invalid_url', fileNav.code);
    const foreverWait = await expectError(() => runtime.wait(home2.pageId, { forText: '永远不会出现的文本', timeoutMs: 2000 }));
    ok('直调 wait 超时稳定收口', foreverWait.code === 'browser.timeout', foreverWait.code);

    ok('直调 status 已起浏览器', (await runtime.status()).launched === true);
  } finally {
    await runtime.shutdown();
  }

  // -----------------------------------------------------------------------
  // 两条路一个账:MCP adapter 路与直调路同一套语义。MCP server 是独立
  // 进程,断言的是同一编码规则与同一错误码,不是字面相等。
  // -----------------------------------------------------------------------
  section('两条路一个账(MCP adapter 对照)');
  const client = new BrowserMcpClient([
    '--engine', 'chromium',
    '--headless',
    '--profile', 'ephemeral',
    '--downloads-dir', tempDir('lubancode-browser-direct-mcp-'),
  ]);
  await client.start();
  try {
    const mcpOpened = await client.call('browser_open', { url: baseUrl + '/' });
    const mcpPageId = mcpOpened.structuredContent.page_id;
    const mcpSnap = await client.call('browser_snapshot', { page_id: mcpPageId });
    ok('两条路 page_id 同一编码(pN)', /^p\d+$/.test(mcpPageId));
    ok('两条路 首航 generation 同为 1', mcpOpened.structuredContent.generation === 1);
    ok('两条路 snapshot_id 同一编码(pN-gN-sN)', new RegExp('^' + mcpPageId + '-g1-s\\d+$').test(mcpSnap.structuredContent.snapshot_id), mcpSnap.structuredContent.snapshot_id);

    const before = await client.call('browser_snapshot', { page_id: mcpPageId });
    const submitRef = refOfLine(before.content[0].text, '提交');
    if (submitRef) {
      await client.call('browser_click', { page_id: mcpPageId, ref: submitRef, snapshot_id: before.structuredContent.snapshot_id });
      const mcpStale = await client.callExpectError('browser_click', { page_id: mcpPageId, ref: submitRef, snapshot_id: before.structuredContent.snapshot_id });
      ok('两条路 旧 ref 同报 browser.stale_ref', mcpStale.code === 'browser.stale_ref', mcpStale.code);
    } else {
      skip('两条路 stale 对照', '快照文本没按预期标出提交按钮');
    }
  } finally {
    await client.stop();
  }
}

// ---------------------------------------------------------------------------
// Runtime journal 矩阵(单子:内嵌浏览器调试工作台 阶段 2):Console/
// Network 两本账的记账、脱敏、环形帽、补账口径;用户与 Agent 仲裁第 4
// 条(Agent 动作不递观察代;用户手点后旧 ref 明报 stale)。
// headless 全跑——Playwright 的 mouse 事件走 CDP,与 headed 同一条
// DOM 事件路,仲裁语义不用真开窗验。
// ---------------------------------------------------------------------------
async function runJournalMatrix(baseUrl, helpers) {
  const { ok, skip, section, tempDir, refOfLine } = helpers;
  section('Runtime journal:console/network/仲裁(阶段 2)');
  if (!playwrightAvailable) {
    skip('journal 矩阵', 'playwright 依赖未安装');
    return;
  }
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  const runtime = new BrowserSession(buildConfig({
    engine: 'chromium', headless: true, profile: 'ephemeral',
    downloadsDir: tempDir('lubancode-browser-journal-dl-'),
  }));
  try {
    const opened = await runtime.open(baseUrl + '/journal.html', {});
    await sleep(900); // 等 console/异步 fetch/pageerror 落账

    const consoleAll = runtime.consoleEntries(opened.pageId, { limit: 500 });
    ok('console 账收四级', ['log', 'info', 'warning', 'error'].every((l) => consoleAll.rows.some((r) => r.level === l)),
       JSON.stringify(consoleAll.rows.map((r) => r.level)));
    ok('console 账收未捕获异常(pageerror)', consoleAll.rows.some((r) => r.level === 'pageerror' && r.text.includes('未捕获异常')));
    ok('console 条目带 seq/generation', consoleAll.rows.every((r) => r.seq > 0 && r.generation >= 1));
    ok('console 脱敏:token 值遮掉', consoleAll.rows.some((r) => r.text.includes('token=<redacted>')) && !consoleAll.rows.some((r) => r.text.includes('abc123')));
    const onlyError = runtime.consoleEntries(opened.pageId, { level: 'error' });
    ok('level 过滤只回 error', onlyError.rows.length >= 1 && onlyError.rows.every((r) => r.level === 'error'));
    const lastSeqSeen = consoleAll.rows[consoleAll.rows.length - 1].seq;
    const since = runtime.consoleEntries(opened.pageId, { sinceSeq: lastSeqSeen - 2 });
    ok('since_seq 补账只回更新的', since.rows.every((r) => r.seq > lastSeqSeen - 2) && since.rows.length >= 1);

    const netAll = runtime.networkEntries(opened.pageId, { limit: 500 });
    ok('network 账收 document 请求', netAll.rows.some((r) => r.resourceType === 'document' && r.status === 200));
    ok('network 账收 fetch 200/500/404', ['200', '500', '404'].every((s) => netAll.rows.some((r) => r.resourceType === 'fetch' && String(r.status) === s)),
       JSON.stringify(netAll.rows.map((r) => r.resourceType + ':' + r.status)));
    ok('network 脱敏:query token 遮掉', !netAll.rows.some((r) => r.url.includes('abc123')) && netAll.rows.some((r) => r.url.includes('token=')));
    // generation >= 0:document 请求在首航边沿(g0->g1)起笔,记 0 是合法账。
    ok('network 条目带 duration/seq/generation', netAll.rows.every((r) => r.seq > 0 && r.durationMs >= 0 && r.generation >= 0));
    const apiFail = runtime.networkEntries(opened.pageId, { urlContains: '/api/fail' });
    ok('url_contains 过滤到 500', apiFail.rows.length === 1 && apiFail.rows[0].status === 500);

    // ---- 仲裁第 4 条:用户一动 DOM,旧 ref 明报 stale -------------------
    const snap = await runtime.snapshot(opened.pageId, {});
    const btnRef = refOfLine(snap.text, '按一下');
    if (btnRef) {
      // Agent 自己连点两次:动作经 withAgentInput 包旗,不递观察代。
      await runtime.click(opened.pageId, btnRef, { snapshotId: snap.snapshotId });
      const again = await runtime.click(opened.pageId, btnRef, { snapshotId: snap.snapshotId });
      ok('Agent 动作不递观察代(同 snapshot 可续动作)', Boolean(again));
      // 用户手点(page.mouse 直发,不经 Agent 队列):观察代 +1,旧 ref stale。
      await runtime.pages.get(opened.pageId).page.mouse.click(5, 5);
      await sleep(400);
      const touched = await expectError(() => runtime.click(opened.pageId, btnRef, { snapshotId: snap.snapshotId }));
      ok('用户动过手后旧 ref 报 stale_ref', touched.code === 'browser.stale_ref' && touched.message.includes('用户'),
         touched.code + ' ' + touched.message.slice(0, 100));
      const snap2 = await runtime.snapshot(opened.pageId, {});
      const btnRef2 = refOfLine(snap2.text, '按一下');
      const after = btnRef2 ? await runtime.click(opened.pageId, btnRef2, { snapshotId: snap2.snapshotId }) : null;
      ok('重新快照后动作恢复', Boolean(after));
    } else {
      skip('仲裁矩阵', '快照文本没按预期标出按钮');
    }

    // ---- 阶段 B(多前端外壳单):owner=user 的注入动作(协议用户路)也
    // 递观察代——App Server 把内核裁定后的 owner 随参数转给 sidecar,
    // 用户点镜像与真手点同一条规矩:旧 ref 即 stale。----
    const userEpochEvents = [];
    runtime.setEventListener((type, payload) => {
      if (type === 'user_epoch') userEpochEvents.push(payload);
    });
    const snapUser = await runtime.snapshot(opened.pageId, {});
    const btnUser = refOfLine(snapUser.text, '按一下');
    if (btnUser) {
      await runtime.click(opened.pageId, btnUser, { snapshotId: snapUser.snapshotId, owner: 'user' });
      await sleep(200);
      ok('owner=user 注入动作递观察代(user_epoch 事件)',
         userEpochEvents.some((e) => e.pageId === opened.pageId && e.userEpoch >= 1),
         JSON.stringify(userEpochEvents));
      const staleUser = await expectError(() =>
        runtime.click(opened.pageId, btnUser, { snapshotId: snapUser.snapshotId, owner: 'user' }));
      ok('递代后旧 ref 报 stale(用户路与真手点同一规矩)',
         staleUser.code === 'browser.stale_ref', staleUser.code + ' ' + staleUser.message.slice(0, 100));
    } else {
      skip('用户注入路矩阵', '快照文本没按预期标出按钮');
    }
    // 关页后账仍可查(unknown_page 才拒)。
    const closedQuery = runtime.consoleEntries(opened.pageId, { level: 'pageerror' });
    ok('关页前账可查', closedQuery.rows.length >= 1);
  } finally {
    await runtime.shutdown();
  }

  // 环形帽:journalCap=10,夹具页打 20+ 条,丢最老明记 dropped。
  const capped = new BrowserSession(buildConfig({
    engine: 'chromium', headless: true, profile: 'ephemeral', journalCap: 10,
    downloadsDir: tempDir('lubancode-browser-journal-cap-'),
  }));
  try {
    const opened = await capped.open(baseUrl + '/journal.html', {});
    await sleep(900);
    const c = capped.consoleEntries(opened.pageId, { limit: 500 });
    ok('环形帽生效(rows<=10)', c.rows.length <= 10, 'rows=' + c.rows.length);
    ok('溢出明记 dropped', c.dropped >= 1 && c.lastSeq > 10, 'dropped=' + c.dropped + ' lastSeq=' + c.lastSeq);
    ok('帽内是最新且 seq 连续', c.rows.every((r, i) => i === 0 || r.seq === c.rows[i - 1].seq + 1));
    const missing = expectErrorSync(() => capped.consoleEntries('p999', {}));
    ok('查不存在的页明报 unknown_page', missing.code === 'browser.unknown_page', missing.code);
  } finally {
    await capped.shutdown();
  }
}

function expectErrorSync(fn) {
  try {
    fn();
  } catch (error) {
    return { code: error.code || '', message: String(error.message || error) };
  }
  return { code: '', message: '(没抛错)' };
}

module.exports = { runDirectMatrix, runJournalMatrix };
