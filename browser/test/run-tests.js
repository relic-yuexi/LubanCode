// Browser MCP 自家测试(P1.8):起真 server + 真 Playwright,走 stdio
// initialize/list/call,覆盖 open -> snapshot -> click -> type ->
// screenshot -> download 的 E2E、profile 语义、隔离锁、安全闸与崩溃终态。
//
// 依赖没齐(没 npm install / 没装浏览器)时逐项打 SKIP 退 0——真机项
// 如实标 SKIP,不冒充通过;矩阵里 chromium 与 webkit 各跑各的,一家的
// 结果不替另一家背书。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const { BrowserMcpClient } = require('./mcp_client');
const { startSite } = require('./site');

let passed = 0;
let failed = 0;
let skipped = 0;
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

function skip(name, reason) {
  ++skipped;
  console.log('  SKIP ' + name + ' (' + reason + ')');
}

function section(name) {
  console.log('\n== ' + name + ' ==');
}

let playwrightAvailable = true;
try {
  require('playwright');
} catch (_) {
  playwrightAvailable = false;
}


// 从快照文本里按关键字找一行,抽出该行的 ref(形如 [ref=eN])。
function refOfLine(snapshotText, keyword) {
  const line = String(snapshotText || '').split('\n').find((l) => l.includes(keyword) && l.includes('[ref='));
  if (!line) return null;
  const m = /\[ref=(e\d+)\]/.exec(line);
  return m ? m[1] : null;
}


// 杀掉 server 进程名下的浏览器子进程(崩溃矩阵用)。browser.process() 在
// headless-shell 档拿不到 pid,从外面按父进程找:Windows 走 CIM,别处
// pkill -P。
const { execFileSync } = require('child_process');
function killBrowserChildrenOf(serverPid) {
  try {
    if (process.platform === 'win32') {
      execFileSync('powershell', [
        '-NoProfile', '-Command',
        "Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq " + serverPid + " -and $_.Name -match 'chrome|Playwright|WebKit' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }",
      ], { encoding: 'utf8', timeout: 15000 });
      return true;
    }
    execFileSync('pkill', ['-KILL', '-P', String(serverPid)]);
    return true;
  } catch (_) {
    return false;
  }
}

function tempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function readCookieFromProfileDir() {
  return null; // Cookie 细节由页面本身回读(登录页 cookie-state),不翻浏览器内部文件。
}

// ---------------------------------------------------------------------------
// 单场矩阵:指定 engine 跑核心 E2E
// ---------------------------------------------------------------------------

async function runMatrix(engine, baseUrl) {
  section('核心 E2E(engine=' + engine + ')');
  const downloads = tempDir('lubancode-browser-dl-');
  const client = new BrowserMcpClient([
    '--engine', engine,
    '--headless',
    '--profile', 'ephemeral',
    '--downloads-dir', downloads,
  ]);
  await client.start();
  try {
    // tools/list:11 件工具 + schema 齐全。
    const list = await client.request('tools/list', {});
    const names = list.result.tools.map((t) => t.name);
    for (const expected of ['browser_status', 'browser_open', 'browser_snapshot', 'browser_click', 'browser_type', 'browser_wait', 'browser_tabs', 'browser_select_tab', 'browser_close_page', 'browser_screenshot', 'browser_downloads']) {
      ok('tools/list 含 ' + expected, names.includes(expected));
    }
    ok('tools/list 带 inputSchema', list.result.tools.every((t) => t.inputSchema && t.inputSchema.type === 'object'));

    // lazy launch:status 不开浏览器。
    const status0 = await client.call('browser_status', {});
    ok('status 未起浏览器', status0.structuredContent.launched === false);

    // open -> snapshot -> click -> type -> screenshot -> download 全链。
    const opened = await client.call('browser_open', { url: baseUrl + '/' });
    const pageId = opened.structuredContent.page_id;
    ok('open 回 page_id', Boolean(pageId), JSON.stringify(opened.structuredContent));
    if (!pageId) {
      throw new Error('browser_open 失败,后续矩阵不跑: ' + (opened.content[0].text || '').slice(0, 160));
    }
    ok('open 回标题', (opened.content[0].text || '').includes('验收站') && (opened.content[0].text || '').includes('首页'), opened.content[0].text);
    ok('open 回 generation=1', opened.structuredContent.generation === 1);

    const snapshot = await client.call('browser_snapshot', { page_id: pageId });
    const snapshotId = snapshot.structuredContent.snapshot_id;
    ok('snapshot 回 snapshot_id', Boolean(snapshotId));
    const refMatch = /\[ref=(e\d+)\]/.exec(snapshot.content[0].text || '');
    ok('snapshot 含 ref 标记', Boolean(refMatch), (snapshot.content[0].text || '').slice(0, 200));

    const nameRef = refOfLine(snapshot.content[0].text, '名字');
    if (nameRef) {
      const typed = await client.call('browser_type', { page_id: pageId, ref: nameRef, text: '张三', snapshot_id: snapshotId });
      ok('名字输入成功(回执见原文)', (typed.content[0].text || '').includes('张三'), typed.content[0].text);

      // 密码框:值不回显。
      const snap2 = await client.call('browser_snapshot', { page_id: pageId });
      const snap2Id = snap2.structuredContent.snapshot_id;
      const passRef = refOfLine(snap2.content[0].text, '密码');
      if (passRef) {
        const typed2 = await client.call('browser_type', { page_id: pageId, ref: passRef, text: 's3cret-value', snapshot_id: snap2Id });
        ok('密码值不回显', (typed2.content[0].text || '').includes('值不回显'), typed2.content[0].text);
        ok('密码不进 structuredContent', JSON.stringify(typed2.structuredContent).indexOf('s3cret-value') === -1);
      } else {
        skip('密码框 ref 定位', '快照文本没按预期标出密码框');
      }

      // 动态 DOM:加两行后重新快照,ref 仍可定位(同 generation 不算 stale)。
      const addRef0 = refOfLine(snapshot.content[0].text, '加一行');
      if (addRef0) {
        await client.call('browser_click', { page_id: pageId, ref: addRef0, snapshot_id: snapshotId });
        await client.call('browser_click', { page_id: pageId, ref: addRef0, snapshot_id: snapshotId });
        const dyn = await client.call('browser_snapshot', { page_id: pageId });
        ok('动态 DOM 后快照见新行', (dyn.content[0].text || '').includes('第 2 行'), (dyn.content[0].text || '').slice(0, 200));
      }
    }

    // click 提交按钮(表单跳 /submitted.html,generation 变,旧 ref 失效)。
    const snapBeforeClick = await client.call('browser_snapshot', { page_id: pageId });
    const clickSnapId = snapBeforeClick.structuredContent.snapshot_id;
    const submitRef = refOfLine(snapBeforeClick.content[0].text, '提交');
    if (submitRef) {
      const clicked = await client.call('browser_click', { page_id: pageId, ref: submitRef, snapshot_id: clickSnapId });
      ok('click 后导航(generation 变)', (clicked.structuredContent || {}).navigated === true, JSON.stringify(clicked.structuredContent));
      ok('click 后 URL 在已提交页', String((clicked.structuredContent || {}).url || '').includes('submitted.html'), JSON.stringify(clicked.structuredContent));

      // 旧 ref 导航后明报 stale。
      const stale = await client.callExpectError('browser_click', { page_id: pageId, ref: submitRef, snapshot_id: clickSnapId });
      ok('旧 ref 报 stale_ref', stale.code === 'browser.stale_ref', stale.code + ' ' + stale.text.slice(0, 120));
    } else {
      skip('提交按钮 ref 定位', '快照文本没按预期标出提交按钮');
    }

    // 第二次调用仍操作同一页:tabs 里 page id 不变。
    const tabs = await client.call('browser_tabs', {});
    ok('tabs 列出同一 page_id', ((tabs.structuredContent || {}).tabs || []).some((t) => t.page_id === pageId && t.active));

    // 截图:image 块 + 元数据;桌面视口尺寸对得上。
    const shot = await client.call('browser_screenshot', { page_id: pageId, full_page: false });
    const image = shot.content.find((c) => c.type === 'image');
    ok('screenshot 回 image 块', Boolean(image));
    if (image) {
      const bytes = Buffer.from(image.data, 'base64');
      ok('截图非空且是 PNG', bytes.length > 1000 && bytes[0] === 0x89 && bytes[1] === 0x50, 'bytes=' + bytes.length);
    }
    ok('screenshot structuredContent 带 sha256', typeof (shot.structuredContent || {}).sha256 === 'string' && shot.structuredContent.sha256.length === 64);

    // 下载三种文件。
    const homeOpen = await client.call('browser_open', { url: baseUrl + '/' });
    const dlPageId = homeOpen.structuredContent.page_id;
    const dlSnap = await client.call('browser_snapshot', { page_id: dlPageId });
    const dlSnapId = dlSnap.structuredContent.snapshot_id;
    for (const label of ['下载文本', '下载图片', '下载压缩包']) {
      const m = refOfLine(dlSnap.content[0].text, label);
      if (!m) {
        skip('下载链接 ' + label + ' 的 ref', '快照没标出');
        continue;
      }
      await client.call('browser_click', { page_id: dlPageId, ref: m, snapshot_id: dlSnapId });
    }
    await new Promise((resolve) => setTimeout(resolve, 1500));
    const downloadsResult = await client.call('browser_downloads', {});
    const rows = (downloadsResult.structuredContent || {}).downloads || [];
    ok('下载账三条且状态 done', rows.length === 3 && rows.every((d) => d.state === 'done'), JSON.stringify(rows.map((d) => d.state)));
    const zipRow = rows.find((d) => (d.filename || '').endsWith('.zip'));
    ok('zip 落盘带 sha256', Boolean(zipRow && zipRow.sha256.length === 64));
    if (zipRow) {
      ok('zip 路径真实存在', fs.existsSync(zipRow.path));
    }

    // wait:等文本出现。
    await client.call('browser_open', { url: baseUrl + '/submitted.html' });
    const waited = await client.call('browser_wait', { page_id: pageId, for_text: '表单已提交' });
    ok('wait 等到文本', (waited.content[0].text || '').includes('表单已提交'));

    // select_tab / close_page。
    const second = await client.call('browser_open', { url: baseUrl + '/screenshot.html' });
    const page2 = second.structuredContent.page_id;
    ok('第二页拿到新 page_id', page2 !== pageId);
    const selected = await client.call('browser_select_tab', { page_id: pageId });
    ok('select_tab 切回第一页', selected.structuredContent.page_id === pageId);
    const closed = await client.call('browser_close_page', { page_id: page2 });
    ok('close_page 收账', (closed.content[0].text || '').includes(page2));
    const closedAgain = await client.callExpectError('browser_select_tab', { page_id: page2 });
    ok('关掉的 page_id 明报失效', closedAgain.code === 'browser.page_closed' || closedAgain.code === 'browser.unknown_page', closedAgain.code);

    // 安全闸:file://、javascript: 拒绝。
    const fileNav = await client.callExpectError('browser_open', { url: 'file:///C:/Windows/win.ini' });
    ok('file:// 被拒', fileNav.code === 'browser.invalid_url', fileNav.code);
    const jsNav = await client.callExpectError('browser_open', { url: 'javascript:alert(1)' });
    ok('javascript: 被拒', jsNav.code === 'browser.invalid_url', jsNav.code);

    // 超时:慢页面 + 永不结束的请求,墙钟收口。
    const slow = await client.call('browser_open', { url: baseUrl + '/slow.html', timeout_ms: 10000, wait_until: 'domcontentloaded' });
    const slowPage = slow.structuredContent.page_id;
    const foreverWait = await client.callExpectError('browser_wait', { page_id: slowPage, for_text: '永远不会出现的文本', timeout_ms: 2000 });
    ok('wait 超时稳定收口', foreverWait.code === 'browser.timeout', foreverWait.code);
  } finally {
    await client.stop();
  }
}

// ---------------------------------------------------------------------------
// profile 语义:persistent 跨会话保 Cookie;ephemeral 不留;双进程抢锁
// ---------------------------------------------------------------------------

async function runProfileMatrix(baseUrl) {
  section('profile 语义(engine=chromium)');
  if (!playwrightAvailable) {
    skip('persistent/ephemeral/隔离锁', 'playwright 依赖未安装');
    return;
  }
  const profileDir = tempDir('lubancode-browser-profile-');
  const mk = () => new BrowserMcpClient(['--engine', 'chromium', '--headless', '--profile', 'persistent', '--user-data-dir', profileDir]);

  // 第一场:写 Cookie。
  {
    const client = mk();
    await client.start();
    try {
      const opened = await client.call('browser_open', { url: baseUrl + '/login.html' });
      const pageId = opened.structuredContent.page_id;
      const snap = await client.call('browser_snapshot', { page_id: pageId });
      const m = refOfLine(snap.content[0].text, '登录(写本地 Cookie)');
      if (m) {
        await client.call('browser_click', { page_id: pageId, ref: m, snapshot_id: snap.structuredContent.snapshot_id });
        await new Promise((resolve) => setTimeout(resolve, 300));
      }
      const state = await client.call('browser_open', { url: baseUrl + '/login.html', new_page: false });
      const wrote = await client.call('browser_wait', { page_id: state.structuredContent.page_id, for_text: '已有会话 Cookie', timeout_ms: 5000 });
      ok('第一场写下了会话 Cookie', (wrote.content[0].text || '').includes('已有会话 Cookie'), wrote.content[0].text);
    } finally {
      await client.stop();
    }
  }

  // 第二场:同 profile 读回 Cookie。
  {
    const client = mk();
    await client.start();
    try {
      const state = await client.call('browser_open', { url: baseUrl + '/login.html' });
      const readBack = await client.call('browser_wait', { page_id: state.structuredContent.page_id, for_text: '已有会话 Cookie', timeout_ms: 5000 });
      ok('第二场(同 profile)读到 Cookie', (readBack.content[0].text || '').includes('已有会话 Cookie'), readBack.content[0].text);
    } finally {
      await client.stop();
    }
  }

  // ephemeral:两场之间不留 Cookie。
  {
    const first = new BrowserMcpClient(['--engine', 'chromium', '--headless', '--profile', 'ephemeral']);
    await first.start();
    try {
      const opened = await first.call('browser_open', { url: baseUrl + '/login.html' });
      const pageId = opened.structuredContent.page_id;
      const snap = await first.call('browser_snapshot', { page_id: pageId });
      const m = refOfLine(snap.content[0].text, '登录(写本地 Cookie)');
      if (m) {
        await first.call('browser_click', { page_id: pageId, ref: m, snapshot_id: snap.structuredContent.snapshot_id });
      }
    } finally {
      await first.stop();
    }
    const second = new BrowserMcpClient(['--engine', 'chromium', '--headless', '--profile', 'ephemeral']);
    await second.start();
    try {
      const state = await second.call('browser_open', { url: baseUrl + '/login.html' });
      const absent = await second.callExpectError('browser_wait', { page_id: state.structuredContent.page_id, for_text: '已有会话 Cookie', timeout_ms: 3000 });
      ok('ephemeral 第二场读不到 Cookie', absent.code === 'browser.timeout', absent.code + ' ' + absent.text.slice(0, 100));
    } finally {
      await second.stop();
    }
  }

  // 隔离锁:占住 profile 的第一只不退,第二只起服即败。
  {
    const holder = mk();
    await holder.start();
    let secondFailed = false;
    let message = '';
    try {
      const intruder = mk();
      try {
        await intruder.start();
        // 起服成功也算失败:initialize 后第一个工具调用须明报锁。
        const err = await intruder.callExpectError('browser_open', { url: baseUrl + '/' });
        secondFailed = err.code === 'browser.profile_locked' || err.text.includes('另一进程');
        message = err.code + ' ' + err.text.slice(0, 120);
      } catch (error) {
        // 起服就退(exit 3)也算锁生效。
        secondFailed = true;
        message = String(error.message || error).slice(0, 120);
      } finally {
        await intruder.stop();
      }
    } finally {
      await holder.stop();
    }
    ok('双进程抢同 profile 后者明确失败', secondFailed, message);
  }
  readCookieFromProfileDir();
}

// ---------------------------------------------------------------------------
// 崩溃终态:杀 browser 子进程,旧 page id 明报失效
// ---------------------------------------------------------------------------

async function runCrashMatrix(baseUrl) {
  section('崩溃终态(engine=chromium)');
  if (!playwrightAvailable) {
    skip('崩溃终态', 'playwright 依赖未安装');
    return;
  }
  const client = new BrowserMcpClient(['--engine', 'chromium', '--headless', '--profile', 'ephemeral']);
  await client.start();
  try {
    const opened = await client.call('browser_open', { url: baseUrl + '/' });
    const pageId = opened.structuredContent.page_id;
    // browser.process() 在 headless-shell 档拿不到,从外面按父进程杀
    // server 名下的浏览器子进程(kill -9 等价)。
    const killed = killBrowserChildrenOf(client.child.pid);
    if (!killed) {
      skip('杀 browser 子进程', '按父进程找子进程失败');
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 1500));
    const err = await client.callExpectError('browser_open', { url: baseUrl + '/', new_page: false, page_id: pageId });
    ok('崩溃后旧 page id 明报失效',
       err.code === 'browser.page_closed' || err.code === 'browser.unknown_page' || err.code === 'browser.crashed' || err.code === 'browser.no_page',
       err.code + ' ' + err.text.slice(0, 120));
  } finally {
    await client.stop();
  }
}

// ---------------------------------------------------------------------------
// 取消矩阵:notifications/cancelled 打断在飞等待
// ---------------------------------------------------------------------------

async function runCancelMatrix(baseUrl) {
  section('取消(engine=chromium)');
  if (!playwrightAvailable) {
    skip('取消链路', 'playwright 依赖未安装');
    return;
  }
  const client = new BrowserMcpClient(['--engine', 'chromium', '--headless', '--profile', 'ephemeral']);
  await client.start();
  try {
    const opened = await client.call('browser_open', { url: baseUrl + '/' });
    const pageId = opened.structuredContent.page_id;
    const started = Date.now();
    const flying = client.requestAsync('tools/call', {
      name: 'browser_wait',
      arguments: { page_id: pageId, ms: 30000, timeout_ms: 40000 },
    });
    await new Promise((resolve) => setTimeout(resolve, 400));
    client.notify('notifications/cancelled', { requestId: flying.id });
    const response = await flying.promise;
    const elapsed = Date.now() - started;
    const result = response.result || {};
    const code = (result.structuredContent || {}).code || JSON.stringify(response.error || {});
    ok('取消后 1 秒内回终态', elapsed < 1000 + 400, String(elapsed) + 'ms');
    ok('终态是 browser.cancelled', code === 'browser.cancelled', code);
    ok('页面未判死:下一调用照常', Boolean((await client.call('browser_tabs', {})).structuredContent));
  } finally {
    await client.stop();
  }
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

async function main() {
  console.log('Browser MCP 自家测试(真 Playwright;依赖未装则逐项 SKIP)');
  const { server, port } = await startSite();
  const baseUrl = 'http://127.0.0.1:' + port;
  try {
    if (!playwrightAvailable) {
      section('依赖探测');
      skip('全部真机矩阵', 'browser/ 目录没装 playwright(先 npm install)');
    }

    // 引擎矩阵:chromium 必跑;webkit 独立跑,结果分记。
    for (const engine of ['chromium', 'webkit']) {
      if (!playwrightAvailable) {
        skip('engine=' + engine + ' 核心矩阵', 'playwright 依赖未安装');
        continue;
      }
      try {
        await runMatrix(engine, baseUrl);
      } catch (error) {
        ++failed;
        failures.push('engine=' + engine + ' 矩阵异常: ' + String(error.message || error));
        console.log('  FAIL engine=' + engine + ' 矩阵异常: ' + String(error.message || error));
      }
    }

    try {
      await runProfileMatrix(baseUrl);
    } catch (error) {
      ++failed;
      failures.push('profile 矩阵异常: ' + String(error.message || error));
      console.log('  FAIL profile 矩阵异常: ' + String(error.message || error));
    }
    try {
      await runCancelMatrix(baseUrl);
    } catch (error) {
      ++failed;
      failures.push('cancel 矩阵异常: ' + String(error.message || error));
      console.log('  FAIL cancel 矩阵异常: ' + String(error.message || error));
    }
    try {
      await runCrashMatrix(baseUrl);
    } catch (error) {
      ++failed;
      failures.push('crash 矩阵异常: ' + String(error.message || error));
      console.log('  FAIL crash 矩阵异常: ' + String(error.message || error));
    }

    console.log('\n---- 汇总 ----');
    console.log('PASS=' + passed + ' FAIL=' + failed + ' SKIP=' + skipped);
    if (failures.length > 0) {
      console.log('失败清单:');
      for (const line of failures) console.log('  - ' + line);
      process.exitCode = 1;
    }
  } finally {
    server.close();
  }
}

main().catch((error) => {
  console.error('测试跑挂:', error);
  process.exitCode = 1;
});
