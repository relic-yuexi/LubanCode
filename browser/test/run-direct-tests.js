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

module.exports = { runDirectMatrix };
