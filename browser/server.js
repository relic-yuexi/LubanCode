#!/usr/bin/env node
// LubanCode 专属 Browser MCP(常驻 stdio server,MCP 富结果单 P1)。
//
// 规矩(单子定案):
//   - stdout 只写 MCP JSON-RPC(一行一条);Playwright 与自家的日志只进 stderr。
//   - 启动时不开浏览器,第一次 browser 工具调用才 lazy launch。
//   - 一份浏览器状态一位主人:BrowserSession actor,所有页面动作排进同一
//     事件队列串行跑,两只 tools/call 不得同时抢一只 page。
//   - 专属来自 profile:独立 user-data 目录 + 进程锁,不碰用户日常
//     Chrome/Edge/Safari profile;ephemeral 档每次新 context,不落盘。
//   - engine 可选 chromium(默认)/ webkit;Windows 首发默认 chromium,
//     兼容面与排错工具更全(单子:默认值须过本机站点矩阵,不凭喜好定)。
//   - 只收 http/https/about:blank;file://、javascript:、data: 一律拒绝。
//   - 每件工具回 page_id、URL、标题与页面 generation;ref 绑定
//     page_id + generation + snapshot_id,导航后旧 ref 明报 stale。
//   - 每次动作有墙钟(默认 15s,可调至 60s);浏览器崩溃后旧 page id
//     明报失效,不吊死会话。
//   - 会话结束(SIGINT/SIGTERM/exit/stdin 关闭)收 page/context/browser,
//     异常退出也杀净子进程树。
//
// 用法:
//   node server.js [--engine chromium|webkit] [--headed|--headless]
//                  [--profile persistent|ephemeral] [--profile-name <名>]
//                  [--user-data-dir <目录>] [--downloads-dir <目录>]
//                  [--viewport WxH] [--action-timeout-ms <毫秒>]
//
// 配进 LubanCode(示例,~/.lubancode/settings.json 或项目 .lubancode/config):
//   "mcpServers": {
//     "browser": {
//       "command": "node",
//       "args": ["<repo>/browser/server.js", "--engine", "chromium", "--headless"]
//     }
//   }

'use strict';

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

// ---------------------------------------------------------------------------
// 参数与环境
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === '--engine') out.engine = next();
    else if (a === '--headed') out.headless = false;
    else if (a === '--headless') out.headless = true;
    else if (a === '--profile') out.profile = next();
    else if (a === '--profile-name') out.profileName = next();
    else if (a === '--user-data-dir') out.userDataDir = next();
    else if (a === '--downloads-dir') out.downloadsDir = next();
    else if (a === '--viewport') out.viewport = next();
    else if (a === '--action-timeout-ms') out.actionTimeoutMs = Number(next());
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
const ENGINE = args.engine === 'webkit' ? 'webkit' : 'chromium';
const HEADLESS = args.headless !== undefined ? args.headless : Boolean(process.env.CI);
const PROFILE_MODE = args.profile === 'persistent' ? 'persistent' : 'ephemeral';
const PROFILE_NAME = args.profileName || 'default';
const HOME_BROWSER_DIR = path.join(os.homedir(), '.lubancode', 'browser');
const USER_DATA_DIR =
  args.userDataDir || path.join(HOME_BROWSER_DIR, 'profiles', PROFILE_MODE === 'persistent' ? PROFILE_NAME : 'ephemeral-' + process.pid);
const SESSION_ID = 's' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
const DOWNLOADS_DIR = args.downloadsDir || path.join(HOME_BROWSER_DIR, 'downloads', SESSION_ID);
const VIEWPORT = (() => {
  const m = /^(\d{2,5})x(\d{2,5})$/.exec(args.viewport || '');
  return m ? { width: Math.min(Number(m[1]), 3840), height: Math.min(Number(m[2]), 4320) } : { width: 1280, height: 720 };
})();
const DEFAULT_ACTION_TIMEOUT_MS = Math.min(Math.max(args.actionTimeoutMs || 15000, 1000), 60000);
const MAX_ACTION_TIMEOUT_MS = 60000;

function log(...parts) {
  process.stderr.write('[browser-mcp] ' + parts.join(' ') + '\n');
}

// 拒绝指向用户日常浏览器 profile:显式给的 user-data-dir 若与常见安装路径
// 重合,直接拒绝启动(单子 P1.2:禁止指向用户日常 Chrome/Edge profile)。
function rejectDailyProfileDir(dir) {
  const normalized = path.resolve(dir).toLowerCase();
  const suspects = [
    path.join(os.homedir(), 'appdata', 'local', 'google', 'chrome', 'user data'),
    path.join(os.homedir(), 'appdata', 'local', 'microsoft', 'edge', 'user data'),
    path.join(os.homedir(), 'library', 'application support', 'google', 'chrome'),
    path.join(os.homedir(), '.config', 'google-chrome'),
    path.join(os.homedir(), 'library', 'application support', 'mozilla', 'firefox'),
  ];
  for (const suspect of suspects) {
    const s = path.resolve(suspect).toLowerCase();
    if (normalized === s || normalized.startsWith(s + path.sep)) {
      return suspect;
    }
  }
  return null;
}

const dailyClash = rejectDailyProfileDir(USER_DATA_DIR);
if (dailyClash) {
  log('拒绝启动:user-data-dir 指向用户日常浏览器 profile(' + dailyClash + '),LubanCode 浏览器须用专属 profile。');
  process.stderr.flush ? process.stderr.flush() : null;
  process.exit(2);
}

// ---------------------------------------------------------------------------
// JSON-RPC stdio
// ---------------------------------------------------------------------------

const PROTOCOL_VERSION = '2025-06-18';
const SUPPORTED_VERSIONS = new Set(['2025-06-18', '2025-03-26', '2024-11-05']);

let buffer = '';
let nextId = 1;

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

function sendResult(id, result) {
  send({ jsonrpc: '2.0', id, result });
}

function sendError(id, code, message) {
  send({ jsonrpc: '2.0', id, error: { code, message } });
}

// ---------------------------------------------------------------------------
// BrowserSession actor:一份浏览器状态一位主人
// ---------------------------------------------------------------------------

let playwright = null;
function loadPlaywright() {
  if (playwright) return playwright;
  try {
    playwright = require('playwright');
    return playwright;
  } catch (error) {
    throw toolError('browser.playwright_missing', '找不到 playwright 依赖:先在 browser/ 目录跑 npm install(依赖锁在本地,不进 C++ 主程序)。' + String(error.message || error));
  }
}

class BrowserError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

function toolError(code, message) {
  return new BrowserError(code, message);
}

// profile 进程锁:第二只进程抢同目录明确失败,不损坏目录。
function acquireProfileLock(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const lockPath = path.join(dir, 'lock');
  try {
    const existing = fs.readFileSync(lockPath, 'utf8').trim();
    if (existing) {
      const pid = Number(existing.split(' ')[0]);
      if (Number.isFinite(pid) && pid !== process.pid) {
        // Windows: process.kill(pid, 0) 对不存在进程抛错;活着则锁被占。
        try {
          process.kill(pid, 0);
          throw toolError('browser.profile_locked', 'profile 正被另一进程(PID ' + pid + ')使用:' + dir + '。换 --profile-name 或先关另一场会话。');
        } catch (error) {
          if (error instanceof BrowserError) throw error;
          // 进程已死:锁是陈的,接管。
        }
      }
    }
  } catch (error) {
    if (error instanceof BrowserError) throw error;
    if (error.code !== 'ENOENT') throw error;
  }
  fs.writeFileSync(lockPath, process.pid + ' ' + new Date().toISOString() + '\n');
  return () => {
    try {
      fs.unlinkSync(lockPath);
    } catch (_) {
      /* 收尾失败不拦退出 */
    }
  };
}

let releaseLock = null;

class BrowserSession {
  constructor() {
    this.browser = null;       // Playwright Browser(persistent 档为 null,context 即全部)
    this.context = null;       // BrowserContext
    this.pages = new Map();    // page_id -> { page, generation, snapshotSeq, closed }
    this.nextPageNumber = 1;
    this.downloads = [];       // { id, state, suggested, filename, path, mime, bytes, sha256 }
    this.crashed = false;
    this.crashReason = '';
  }

  async ensureLaunched() {
    if (this.context) {
      if (this.crashed) {
        throw toolError('browser.crashed', '浏览器已崩溃(' + this.crashReason + ')。旧 page id 全部失效;调 browser_status 查看重启状态,再重新 browser_open。');
      }
      return;
    }
    const pw = loadPlaywright();
    const engine = pw[ENGINE];
    if (!engine) throw toolError('browser.engine_missing', 'engine "' + ENGINE + '" 不可用(需要 npm install 装齐浏览器: npx playwright install ' + ENGINE + ')。');
    try {
      if (PROFILE_MODE === 'persistent') {
        fs.mkdirSync(USER_DATA_DIR, { recursive: true });
        this.context = await engine.launchPersistentContext(USER_DATA_DIR, {
          headless: HEADLESS,
          viewport: VIEWPORT,
          acceptDownloads: true,
        });
        this.browser = this.context.browser() || null;
      } else {
        this.browser = await engine.launch({ headless: HEADLESS });
        this.context = await this.browser.newContext({ viewport: VIEWPORT, acceptDownloads: true });
      }
    } catch (error) {
      this.context = null;
      this.browser = null;
      if (error instanceof BrowserError) throw error;
      throw toolError('browser.launch_failed', '浏览器起不来(engine=' + ENGINE + ',headless=' + HEADLESS + '):' + String(error.message || error) + '。浏览器没装时先跑 npx playwright install ' + ENGINE + '。');
    }
    this.crashed = false;
    this.crashReason = '';
    // 下载账:建议名只作参考,安全文件名本地起。
    this.context.on('download', async (download) => {
      const record = {
        id: 'd' + (this.downloads.length + 1),
        state: 'in_progress',
        suggested: download.suggestedFilename() || 'download',
        filename: '',
        path: '',
        mime: '',
        bytes: 0,
        sha256: '',
      };
      this.downloads.push(record);
      try {
        fs.mkdirSync(DOWNLOADS_DIR, { recursive: true });
        const safe = sanitizeFilename(download.suggestedFilename() || 'download.bin');
        const target = uniquePath(path.join(DOWNLOADS_DIR, safe));
        await download.saveAs(target);
        record.path = target;
        record.filename = path.basename(target);
        record.state = 'done';
        const bytes = fs.readFileSync(target);
        record.bytes = bytes.length;
        record.sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
      } catch (error) {
        record.state = 'failed';
        record.error = String(error.message || error);
      }
    });
    // 崩溃与断连:所有在飞调用只收一次终态,不吊死。
    const onGone = (reason) => {
      if (!this.crashed) {
        this.crashed = true;
        this.crashReason = reason;
        for (const [pageId, entry] of this.pages) {
          entry.closed = true;
        }
        log('browser gone:', reason);
      }
    };
    if (this.browser) {
      this.browser.on('disconnected', () => onGone('browser process disconnected'));
    }
    this.context.on('close', () => onGone('context closed'));
  }

  async shutdown() {
    const errors = [];
    try {
      if (this.context) await this.context.close();
    } catch (error) {
      errors.push(String(error.message || error));
    }
    try {
      if (this.browser) await this.browser.close();
    } catch (error) {
      errors.push(String(error.message || error));
    }
    this.context = null;
    this.browser = null;
    this.pages.clear();
    if (errors.length > 0) log('shutdown 清理有失败项:', errors.join('; '));
  }

  registerPage(page) {
    const id = 'p' + this.nextPageNumber++;
    // generation 从 0 起:首次 goto 之后恰为 1;再导航才 +1。
    const entry = { page, generation: 0, snapshotSeq: 0, closed: false };
    this.pages.set(id, entry);
    // 主框架导航 = 换页:generation +1,旧 ref 即刻失效(单子 P1.4)。
    page.on('framenavigated', (frame) => {
      if (frame === page.mainFrame()) {
        entry.generation += 1;
      }
    });
    page.on('close', () => {
      entry.closed = true;
    });
    page.on('crash', () => {
      entry.closed = true;
    });
    return { id, entry };
  }

  async activePage() {
    await this.ensureLaunched();
    for (const [id, entry] of this.pages) {
      if (!entry.closed) return { id, entry };
    }
    throw toolError('browser.no_page', '当前没有打开的页面,先调 browser_open。');
  }

  resolvePage(pageId) {
    const entry = this.pages.get(pageId);
    if (!entry) {
      throw toolError('browser.unknown_page', 'page_id "' + pageId + '" 不存在(从未开过或浏览器已重启,旧句柄全部失效)。用 browser_tabs 列当前页。');
    }
    if (entry.closed) {
      throw toolError('browser.page_closed', 'page_id "' + pageId + '" 已关闭,旧句柄不可复用。用 browser_tabs 列当前页,重新 browser_open。');
    }
    return entry;
  }

  // ref 解析:自建 ref 体系——browser_snapshot 在页面里登记 ref -> CSS
  // 选择器(window.__lubanRefSelectors),动作时翻登记表拿选择器再
  // locator。绑定 generation + snapshot_id:导航换页后登记表随页面销毁,
  // 旧 ref 一律明报 stale;DOM 改动但未导航时,选择器重新数一遍目标,
  // 数量不是恰一个就拒绝,不点第一个凑数(单子 P1.4)。
  async refLocator(pageId, entry, ref, snapshotId) {
    if (!/^e\d+$/.test(String(ref || ''))) {
      throw toolError('browser.bad_ref', 'ref 形如 "e12"(browser_snapshot 返回),收到: ' + JSON.stringify(ref));
    }
    if (snapshotId !== undefined && snapshotId !== null && snapshotId !== '') {
      // snapshot_id 是快照返回的全名(page-g代-序):它已经把 page 与
      // generation 编进去了;换页/换代即过期。
      const prefix = pageId + '-g' + entry.generation + '-';
      if (!String(snapshotId).startsWith(prefix)) {
        throw toolError('browser.stale_ref', 'ref 已过期(snapshot ' + snapshotId + ',当前页代前缀应为 ' + prefix + ')。页面导航或重启后须重新 browser_snapshot 再动作。');
      }
    }
    const page = entry.page;
    let selector = null;
    try {
      selector = await Promise.race([
        page.evaluate((r) => (window.__lubanRefSelectors || {})[r] || null, String(ref)),
        new Promise((resolve) => setTimeout(() => resolve(undefined), 5000)),
      ]);
    } catch (error) {
      throw toolError('browser.stale_ref', '页面已换(' + describeError(error) + '),旧 ref 全部失效;重新 browser_snapshot。');
    }
    if (selector === null) {
      throw toolError('browser.stale_ref', 'ref ' + ref + ' 在当前页面没有登记(导航换页后登记表清空)。重新 browser_snapshot 再动作。');
    }
    if (selector === undefined) {
      throw toolError('browser.timeout', '到页面里查 ref 登记表超时。');
    }
    return page.locator(selector);
  }
}

// 快照脚本:主框架 DOM 走一遍,认交互元素与标题,发 eN ref 并把
// ref -> CSS 选择器登记进页面(window.__lubanRefSelectors)。首版不进
// iframe 与 shadow DOM,快照头部明说(单子 P1.4:不支持的形状须明报);
// 密码值永不出账(回 ***)。
const SNAPSHOT_SCRIPT = `(() => {
  const hidden = (el) => {
    if (!el.isConnected) return true;
    if (el.hidden || el.getAttribute('aria-hidden') === 'true') return true;
    const style = getComputedStyle(el);
    return style.display === 'none' || style.visibility === 'hidden';
  };
  const cssPath = (el) => {
    if (el.id) {
      const id = CSS.escape(el.id);
      try {
        if (document.querySelectorAll('#' + id).length === 1) return '#' + id;
      } catch (_) { /* 坏 id 就走路径 */ }
    }
    const parts = [];
    let node = el;
    while (node && node.nodeType === 1 && node !== document.body && node.tagName !== 'HTML') {
      const parent = node.parentElement;
      const tag = node.tagName.toLowerCase();
      let part = tag;
      if (parent) {
        const sameTag = Array.prototype.filter.call(parent.children, (c) => c.tagName === node.tagName);
        if (sameTag.length > 1) part += ':nth-of-type(' + (sameTag.indexOf(node) + 1) + ')';
      }
      parts.unshift(part);
      node = parent;
    }
    return 'body > ' + parts.join(' > ');
  };
  const roleOf = (el) => {
    const explicit = el.getAttribute('role');
    if (explicit) return explicit;
    const tag = el.tagName.toLowerCase();
    if (tag === 'a') return el.getAttribute('href') != null ? 'link' : null;
    if (tag === 'button') return 'button';
    if (tag === 'input') {
      const type = (el.getAttribute('type') || 'text').toLowerCase();
      if (type === 'submit' || type === 'button' || type === 'reset') return 'button';
      if (type === 'checkbox') return 'checkbox';
      if (type === 'radio') return 'radio';
      return 'textbox';
    }
    if (tag === 'select') return 'combobox';
    if (tag === 'textarea') return 'textbox';
    if (/^h[1-6]$/.test(tag)) return 'heading';
    if (el.hasAttribute && el.hasAttribute('data-row')) return 'text';
    return null;
  };
  const nameOf = (el) => {
    const viaLabel = el.labels && el.labels[0] ? String(el.labels[0].textContent || '').trim() : '';
    const text = String(el.textContent || '').trim().replace(/\\s+/g, ' ');
    // 表单控件(<select> 的 textContent 是一堆 option 文本,不是名)优先取
    // <label> 关联名;aria-label 仍居首(ARIA 命名计算如此)。
    const control = el instanceof HTMLInputElement || el instanceof HTMLSelectElement || el instanceof HTMLTextAreaElement;
    return String(el.getAttribute('aria-label') || (control && viaLabel) || el.getAttribute('title') || el.getAttribute('placeholder') ||
      text || viaLabel || el.value || '').slice(0, 80);
  };
  const selector = 'a[href], button, input, select, textarea, [role], h1, h2, h3, h4, [data-row]';
  const refs = {};
  const lines = [];
  let n = 0;
  for (const el of document.body.querySelectorAll(selector)) {
    if (hidden(el)) continue;
    const role = roleOf(el);
    if (!role) continue;
    n += 1;
    const ref = 'e' + n;
    refs[ref] = cssPath(el);
    let line = '- ' + role + ' ' + JSON.stringify(nameOf(el));
    if (el instanceof HTMLInputElement || el instanceof HTMLTextAreaElement || el instanceof HTMLSelectElement) {
      if (el.type === 'password') {
        line += ' =(密码,值不回显)';
      } else if (el.value) {
        line += ' =' + JSON.stringify(String(el.value).slice(0, 40));
      }
    }
    if (el instanceof HTMLSelectElement) {
      // 下拉带选项清单(value=文本):模型选值得有菜单可看,不然只能瞎猜
      // 再拿键盘箭头凑——那是 browser_type 干不了的活,browser_select 才管。
      const entries = Array.prototype.map.call(el.options, (o) => (o.value === '' ? '' : o.value + '=') + String(o.label || o.text || '').trim());
      let menu = entries.slice(0, 12).join(' | ');
      if (entries.length > 12) menu += ' | …(共 ' + entries.length + ' 项)';
      if (menu.length > 240) menu = menu.slice(0, 240) + '…';
      line += ' 选项: ' + menu;
    }
    lines.push(line + ' [ref=' + ref + ']');
  }
  window.__lubanRefSelectors = refs;
  return { snapshot: lines.join('\\n'), count: n, url: location.href };
})()`;

const session = new BrowserSession();

function sanitizeFilename(name) {
  const base = path.basename(String(name || 'download.bin'));
  const cleaned = base.replace(/[^\w.\-一-龥]+/g, '_').replace(/^\.+/, '_');
  return cleaned || 'download.bin';
}

function uniquePath(target) {
  if (!fs.existsSync(target)) return target;
  const dir = path.dirname(target);
  const ext = path.extname(target);
  const stem = path.basename(target, ext);
  for (let i = 1; i < 1000; ++i) {
    const candidate = path.join(dir, stem + '-' + i + ext);
    if (!fs.existsSync(candidate)) return candidate;
  }
  return path.join(dir, stem + '-' + Date.now() + ext);
}

// URL 门:只收 http/https/about:blank(单子 P1.5)。
function checkUrl(raw) {
  let parsed;
  try {
    parsed = new URL(raw);
  } catch (_) {
    throw toolError('browser.invalid_url', 'URL 解析不开: ' + JSON.stringify(raw));
  }
  if (parsed.protocol === 'about:' && parsed.pathname === 'blank') return;
  if (parsed.protocol === 'http:' || parsed.protocol === 'https:') return;
  throw toolError('browser.invalid_url', '只收 http/https/about:blank,拒绝 ' + parsed.protocol + '(file://、javascript:、data: 一律不放行)。');
}

// 墙钟:每次动作有上限,不开放无限等待。
async function withDeadline(promise, timeoutMs, what) {
  let timer = null;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(toolError('browser.timeout', what + ' 超过 ' + timeoutMs + 'ms 没完成,已放弃等待(页面状态未判死,可再试或 browser_wait)。')), timeoutMs);
      }),
    ]);
  } finally {
    if (timer) clearTimeout(timer);
  }
}

function callTimeout(input) {
  const asked = Number(input.timeout_ms);
  if (!Number.isFinite(asked)) return DEFAULT_ACTION_TIMEOUT_MS;
  return Math.min(Math.max(Math.trunc(asked), 1000), MAX_ACTION_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// 工具面(P1.3):少而稳,每件都回 page_id、URL、标题与 generation
// ---------------------------------------------------------------------------

const TOOLS = [
  {
    name: 'browser_status',
    description: '浏览器状态:engine、headed、profile、browser pid、打开的页数、下载目录。不开浏览器、不导航。',
    inputSchema: { type: 'object', properties: {} },
    handler: async () => {
      let pid = null;
      if (session.browser) {
        try {
          const proc = session.browser.process();
          pid = proc ? proc.pid : null;  // webkit 拿不到 process 就置空
        } catch (_) { /* 崩溃途中不纠结 */ }
      }
      return textResult('browser: engine=' + ENGINE + ' mode=' + (HEADLESS ? 'headless' : 'headed') + ' profile=' + PROFILE_MODE + (PROFILE_MODE === 'persistent' ? '(' + PROFILE_NAME + ')' : '') + ' launched=' + Boolean(session.context) + (session.crashed ? ' crashed(' + session.crashReason + ')' : '') + ' pages=' + countOpenPages() + ' downloads_dir=' + DOWNLOADS_DIR, {
        engine: ENGINE,
        headless: HEADLESS,
        profile: PROFILE_MODE,
        profile_name: PROFILE_MODE === 'persistent' ? PROFILE_NAME : null,
        user_data_dir: PROFILE_MODE === 'persistent' ? USER_DATA_DIR : null,
        launched: Boolean(session.context),
        crashed: session.crashed,
        crash_reason: session.crashed ? session.crashReason : '',
        browser_pid: pid,
        pages: countOpenPages(),
        downloads_dir: DOWNLOADS_DIR,
      });
    },
  },
  {
    name: 'browser_open',
    description: '打开一个 URL(只收 http/https/about:blank)。默认开新页;new_page=false 时在当前活动页导航。',
    inputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string', description: '目标 URL(http/https/about:blank)' },
        new_page: { type: 'boolean', description: 'true=开新标签页(默认);false=当前页导航' },
        wait_until: { type: 'string', description: 'load|domcontentloaded|networkidle(默认 load)' },
        timeout_ms: { type: 'number', description: '本次动作墙钟(默认 15s,上限 60s)' },
      },
      required: ['url'],
    },
    handler: async (input) => {
      const url = String(input.url || '');
      checkUrl(url);
      await session.ensureLaunched();
      const timeoutMs = callTimeout(input);
      const waitUntil = ['load', 'domcontentloaded', 'networkidle'].includes(input.wait_until) ? input.wait_until : 'load';
      let page;
      let pageId;
      if (input.new_page === false) {
        const active = await session.activePage();
        pageId = active.id;
        page = active.entry.page;
      } else {
        page = await withDeadline(session.context.newPage(), timeoutMs, '开新页');
        const registered = session.registerPage(page);
        pageId = registered.id;
      }
      const response = await withDeadline(page.goto(url, { waitUntil, timeout: timeoutMs }), timeoutMs, '打开 ' + url);
      const entry = session.pages.get(pageId);
      const status = response ? String(response.status()) : '';
      const summary = [
        '已打开 ' + page.url(),
        '标题: ' + (await safeTitle(page)),
        'page_id=' + pageId + ' generation=' + entry.generation,
        status ? 'HTTP ' + status : '(无 HTTP 状态(about:blank 或缓存导航))',
      ].join('\n');
      return textResult(summary, pageMeta(pageId, page, entry, { http_status: status }));
    },
  },
  {
    name: 'browser_snapshot',
    description: '语义快照(可访问性树,带 ref 标记):比整页 HTML 稳,模型按 ref 后续动作。ref 绑定当前 generation,导航后需重新快照。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        max_chars: { type: 'number', description: '快照字符帽(默认 20000)' },
        timeout_ms: { type: 'number' },
      },
    },
    handler: async (input) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const page = target.entry.page;
      let yaml;
      try {
        const outcome = await withDeadline(page.evaluate(SNAPSHOT_SCRIPT), timeoutMs, '快照');
        yaml = outcome.snapshot;
      } catch (error) {
        throw toolError('browser.snapshot_failed', '快照拿不到(页面可能还在跳转):' + describeError(error));
      }
      target.entry.snapshotSeq += 1;
      const snapshotId = target.id + '-g' + target.entry.generation + '-s' + target.entry.snapshotSeq;
      const maxChars = Math.min(Math.max(Number(input.max_chars) || 20000, 500), 200000);
      const truncated = yaml.length > maxChars;
      const header = 'snapshot_id=' + snapshotId + '\n[语义快照:主框架交互元素与标题;iframe 与 shadow DOM 首版不进快照(明报,不冒充);ref 绑定本页 generation,导航后重新快照]';
      const body = truncated ? yaml.slice(0, maxChars) + '\n[快照超字符帽,已截断;要更全给 max_chars]' : yaml;
      return textResult(header + '\n' + body,
        Object.assign(pageMeta(target.id, page, target.entry, {}), { snapshot_id: snapshotId, truncated }));
    },
  },
  {
    name: 'browser_click',
    description: '点击快照里的控件(首选 ref)。点击前确认页面 generation;点击后等导航/DOM 稳定。ref 过期会明报 stale_ref。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        ref: { type: 'string', description: 'browser_snapshot 里的 eN ref' },
        snapshot_id: { type: 'string', description: 'ref 来自哪次快照(建议带上,导航后防 stale)' },
        timeout_ms: { type: 'number' },
      },
      required: ['ref'],
    },
    handler: async (input) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const locator = await session.refLocator(target.id, target.entry, String(input.ref || ''), input.snapshot_id);
      const count = await withDeadline(locator.count(), timeoutMs, '数目标');
      if (count === 0) throw toolError('browser.target_not_found', 'ref ' + input.ref + ' 在当前页面找不到(0 个目标)。页面变了就重新 browser_snapshot。');
      if (count > 1) throw toolError('browser.target_not_unique', 'ref ' + input.ref + ' 解析到 ' + count + ' 个目标,拒绝乱点第一个。重新 browser_snapshot 拿唯一 ref。');
      const before = target.entry.generation;
      await withDeadline(locator.click({ timeout: timeoutMs }), timeoutMs, '点击 ' + input.ref);
      // 点击后等"真稳态":framenavigated 异步注册,waitForLoadState 对已
      // 载完的页会立即返回——先给换页一小口气(1.5s 内代数变了才算导航),
      // 再等 domcontentloaded 收尾。
      for (let i = 0; i < 15 && target.entry.generation === before; ++i) {
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
      await withDeadline(pageSettled(target.entry.page), Math.min(timeoutMs, 3000), '等页面稳定').catch(() => null);
      const entry = target.entry;
      const navigated = entry.generation !== before;
      return textResult('已点击 ref=' + input.ref + (navigated ? '(页面已导航,generation ' + before + ' -> ' + entry.generation + ',旧 ref 全部失效)' : '(未导航,generation ' + entry.generation + ')') + '\nURL: ' + entry.page.url(), pageMeta(target.id, entry.page, entry, { clicked_ref: input.ref, navigated }));
    },
  },
  {
    name: 'browser_type',
    description: '往输入框里填字。密码框的值不回显、不进日志。不支持自动读系统剪贴板。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        ref: { type: 'string' },
        snapshot_id: { type: 'string' },
        text: { type: 'string' },
        mode: { type: 'string', description: 'fill=整框替换(默认)| type=逐键输入' },
        press_enter: { type: 'boolean', description: '输入后按回车(提交表单用)' },
        timeout_ms: { type: 'number' },
      },
      required: ['ref', 'text'],
    },
    handler: async (input) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const locator = await session.refLocator(target.id, target.entry, String(input.ref || ''), input.snapshot_id);
      const count = await withDeadline(locator.count(), timeoutMs, '数目标');
      if (count !== 1) {
        throw toolError(count === 0 ? 'browser.target_not_found' : 'browser.target_not_unique', 'ref ' + input.ref + ' 解析到 ' + count + ' 个目标(要恰一个)。');
      }
      let isPassword = false;
      try {
        isPassword = await locator.evaluate((el) => el instanceof HTMLElement && (el.type === 'password' || el.getAttribute('type') === 'password'));
      } catch (_) { /* 评不了就当普通框 */ }
      if (input.mode === 'type') {
        await withDeadline(locator.pressSequentially(String(input.text ?? ''), { timeout: timeoutMs, delay: 10 }), timeoutMs, '逐键输入');
      } else {
        await withDeadline(locator.fill(String(input.text ?? ''), { timeout: timeoutMs }), timeoutMs, '填入');
      }
      if (input.press_enter) {
        await withDeadline(locator.press('Enter', { timeout: timeoutMs }), timeoutMs, '按回车');
      }
      // 密码值永不出账:回执只说"已输入(密码,值不回显)"。
      const shown = isPassword ? '(密码框,值不回显)' : JSON.stringify(String(input.text ?? ''));
      return textResult('已输入 ' + shown + (input.press_enter ? ' 并按回车' : '') + '\nURL: ' + target.entry.page.url(),
        pageMeta(target.id, target.entry.page, target.entry, { typed: isPassword ? '[password]' : String(input.text ?? ''), password: isPassword }));
    },
  },
  {
    name: 'browser_select',
    description: '选 <select> 下拉框的一项:按 option 的 value 或可见文本(label)匹配。下拉别用 browser_type 发箭头键——那是往框里打字,选不动下拉。多选框(multiple)传数组一次选多项。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        ref: { type: 'string', description: 'browser_snapshot 里的 eN ref(须指向 <select>)' },
        snapshot_id: { type: 'string', description: 'ref 来自哪次快照(建议带上,导航后防 stale)' },
        value: { type: ['string', 'array'], items: { type: 'string' }, description: '按 option 的 value 属性匹配;数组=多选框一次选多项' },
        label: { type: ['string', 'array'], items: { type: 'string' }, description: '按 option 可见文本匹配(空白折叠后整串相等)。与 value 至少给一样;都给时按 value 优先' },
        timeout_ms: { type: 'number' },
      },
      required: ['ref'],
    },
    handler: async (input) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const locator = await session.refLocator(target.id, target.entry, String(input.ref || ''), input.snapshot_id);
      const count = await withDeadline(locator.count(), timeoutMs, '数目标');
      if (count !== 1) {
        throw toolError(count === 0 ? 'browser.target_not_found' : 'browser.target_not_unique', 'ref ' + input.ref + ' 解析到 ' + count + ' 个目标(要恰一个)。');
      }
      const asList = (v) => {
        if (Array.isArray(v)) return v.map(String);
        if (v === undefined || v === null || v === '') return [];
        return [String(v)];
      };
      const wantedValues = asList(input.value);
      const wantedLabels = asList(input.label);
      if (wantedValues.length === 0 && wantedLabels.length === 0) {
        throw toolError('browser.schema', '给 value(option 的 value 属性)或 label(option 可见文本)至少一样;快照里下拉行带"选项: value=文本"清单。');
      }
      // 先验明是 <select> 并读全部选项:按不到值时把候选整个回给模型,
      // 不让它在超时里猜。
      const info = await withDeadline(
        locator.evaluate((el) => {
          if (!(el instanceof HTMLSelectElement)) return null;
          return {
            multiple: el.multiple,
            options: Array.from(el.options).map((o) => ({ value: o.value, label: String(o.label || o.text || '').trim() })),
          };
        }),
        timeoutMs, '读下拉选项',
      );
      if (!info) {
        throw toolError('browser.bad_target', 'ref ' + input.ref + ' 不是 <select> 下拉框(browser_select 只管下拉;文本框用 browser_type)。');
      }
      if (!info.multiple && wantedValues.length + wantedLabels.length > 1) {
        throw toolError('browser.schema', '这是单选下拉,只收一个 value 或 label;要一次选多项,目标须是 <select multiple>。');
      }
      const collapse = (s) => String(s).trim().replace(/\s+/g, ' ');
      const picked = [];
      const missing = [];
      for (const key of wantedValues) {
        const hit = info.options.find((o) => o.value === key);
        if (hit) picked.push(hit); else missing.push('value=' + JSON.stringify(key));
      }
      for (const key of wantedLabels) {
        const hit = info.options.find((o) => collapse(o.label) === collapse(key));
        if (hit) picked.push(hit); else missing.push('label=' + JSON.stringify(key));
      }
      if (missing.length > 0) {
        const menu = info.options.map((o) => JSON.stringify(o.value) + '(文本 ' + JSON.stringify(o.label) + ')').join(', ');
        throw toolError('browser.option_not_found', '下拉里按不到:' + missing.join(', ') + '。可选项共 ' + info.options.length + ' 个:' + menu + '。');
      }
      await withDeadline(
        locator.selectOption(picked.map((o) => ({ value: o.value })), { timeout: timeoutMs }),
        timeoutMs, '选 ' + input.ref,
      );
      // 选完读回实际选中项:回执以页面为准,不以请求为准。
      const chosen = await withDeadline(
        locator.evaluate((el) => Array.from(el.selectedOptions).map((o) => ({ value: o.value, label: String(o.label || o.text || '').trim() }))),
        timeoutMs, '读回选中项',
      ).catch(() => picked.map((o) => ({ value: o.value, label: o.label })));
      const shown = chosen.map((o) => o.value + (o.label && o.label !== o.value ? '(' + o.label + ')' : '')).join(', ');
      return textResult('已选择 ' + input.ref + ': ' + shown + '\nURL: ' + target.entry.page.url(),
        pageMeta(target.id, target.entry.page, target.entry, { selected_ref: input.ref, selected: chosen, multiple: info.multiple }));
    },
  },
  {
    name: 'browser_wait',
    description: '等一个条件成立:文本出现 / URL 含某段 / 固定毫秒(上限 60s)。不开放无限等待。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        for_text: { type: 'string', description: '等页面出现这段文本' },
        url_contains: { type: 'string', description: '等 URL 含这段' },
        ms: { type: 'number', description: '固定等待毫秒(<= 60000)' },
        timeout_ms: { type: 'number', description: '总墙钟(默认 15s,上限 60s)' },
      },
    },
    handler: async (input, token) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const page = target.entry.page;
      if (input.for_text) {
        // waitForSelector 自身的超时给宽一拍(墙钟先到先收口);它偶发的
        // 非超时错(页面忙/上下文换新)也按等待未成口径收口,不抖成
        // internal_error——等待失败对调用方就是一个语义:没等到。
        try {
          await withDeadline(
            page.waitForSelector('text=' + JSON.stringify(String(input.for_text)), { timeout: timeoutMs + 5000 }),
            timeoutMs, '等文本 "' + input.for_text + '"');
        } catch (error) {
          if (error instanceof BrowserError) throw error;
          throw toolError('browser.timeout', '等文本 "' + input.for_text + '" 在 ' + timeoutMs + 'ms 内没等到(' + describeError(error) + '),当前 ' + page.url());
        }
        return textResult('文本已出现: ' + input.for_text, pageMeta(target.id, page, target.entry, { waited_for: 'text' }));
      }
      if (input.url_contains) {
        const started = Date.now();
        while (Date.now() - started < timeoutMs) {
          if (token && token.cancelled) {
            throw toolError('browser.cancelled', '等待已取消(页面未判死,URL 当前 ' + page.url() + ')。');
          }
          if (page.url().includes(String(input.url_contains))) {
            return textResult('URL 已含 ' + input.url_contains + ': ' + page.url(), pageMeta(target.id, page, target.entry, { waited_for: 'url' }));
          }
          await new Promise((resolve) => setTimeout(resolve, 100));
        }
        throw toolError('browser.timeout', '等 URL 含 "' + input.url_contains + '" 超过 ' + timeoutMs + 'ms,当前 ' + page.url());
      }
      const ms = Math.min(Math.max(Number(input.ms) || 0, 0), MAX_ACTION_TIMEOUT_MS);
      if (ms > 0) {
        const sliceEnd = Date.now() + ms;
        while (Date.now() < sliceEnd) {
          if (token && token.cancelled) {
            throw toolError('browser.cancelled', '等待已取消(页面未判死)。');
          }
          await new Promise((resolve) => setTimeout(resolve, Math.min(100, sliceEnd - Date.now())));
        }
        return textResult('等了 ' + ms + 'ms', pageMeta(target.id, page, target.entry, { waited_for: 'fixed' }));
      }
      throw toolError('browser.schema', '给一个条件:for_text / url_contains / ms。');
    },
  },
  {
    name: 'browser_tabs',
    description: '列出全部标签页:page_id、标题、URL、是否活动、generation。',
    inputSchema: { type: 'object', properties: {} },
    handler: async () => {
      await session.ensureLaunched();
      const rows = [];
      for (const [id, entry] of session.pages) {
        rows.push({
          page_id: id,
          title: await safeTitle(entry.page),
          url: entry.page.url(),
          active: !entry.closed,
          generation: entry.generation,
        });
      }
      const body = rows.length === 0 ? '(没有页面)' : rows.map((r) => (r.active ? '*' : 'x') + ' ' + r.page_id + ' g' + r.generation + ' ' + r.title + ' -- ' + r.url).join('\n');
      return textResult(body, { tabs: rows });
    },
  },
  {
    name: 'browser_select_tab',
    description: '明确切换活动页(不靠"最后一页"猜)。',
    inputSchema: { type: 'object', properties: { page_id: { type: 'string' } }, required: ['page_id'] },
    handler: async (input) => {
      await session.ensureLaunched();
      const entry = session.resolvePage(String(input.page_id || ''));
      await withDeadline(entry.page.bringToFront(), callTimeout(input), '切页');
      return textResult('活动页已切到 ' + input.page_id + ': ' + entry.page.url(), pageMeta(input.page_id, entry.page, entry, {}));
    },
  },
  {
    name: 'browser_close_page',
    description: '关一页。关掉的 page_id 稳定报 closed,不偷偷复用 id。',
    inputSchema: { type: 'object', properties: { page_id: { type: 'string' } }, required: ['page_id'] },
    handler: async (input) => {
      await session.ensureLaunched();
      const entry = session.resolvePage(String(input.page_id || ''));
      await withDeadline(entry.page.close({ runBeforeUnload: false }), callTimeout(input), '关页');
      entry.closed = true;
      return textResult('已关闭 ' + input.page_id + '(此后引用它明报 page_closed,不复用 id)', { closed_page_id: input.page_id, open_pages: countOpenPages() });
    },
  },
  {
    name: 'browser_screenshot',
    description: '截图(viewport / full_page / 元素)。回文本摘要 + image 块 + structuredContent 元数据(尺寸/字节)。图片走 MCP 富结果链,先落 artifact 再进历史。',
    inputSchema: {
      type: 'object',
      properties: {
        page_id: { type: 'string' },
        full_page: { type: 'boolean', description: '整页(默认 false=视口)' },
        ref: { type: 'string', description: '只截某个元素(可选)' },
        snapshot_id: { type: 'string' },
        timeout_ms: { type: 'number' },
      },
    },
    handler: async (input) => {
      await session.ensureLaunched();
      const target = input.page_id ? { id: input.page_id, entry: session.resolvePage(input.page_id) } : await session.activePage();
      const timeoutMs = callTimeout(input);
      const page = target.entry.page;
      const options = { timeout: timeoutMs, type: 'png' };
      if (input.full_page) options.fullPage = true;
      let buffer;
      if (input.ref) {
        const locator = await session.refLocator(target.id, target.entry, String(input.ref), input.snapshot_id);
        buffer = await withDeadline(locator.screenshot(options), timeoutMs, '截元素');
      } else {
        buffer = await withDeadline(page.screenshot(options), timeoutMs, '截图');
      }
      const sha256 = crypto.createHash('sha256').update(buffer).digest('hex');
      const content = [
        { type: 'text', text: '截图完成: ' + page.url() + '\nPNG ' + buffer.length + '字节 sha256=' + sha256.slice(0, 12) + '…(图片字节在 image 块,宿主落 artifact 后模型与人都可看)' },
        { type: 'image', data: buffer.toString('base64'), mimeType: 'image/png' },
      ];
      return { content, structuredContent: Object.assign(pageMeta(target.id, page, target.entry, {}), { bytes: buffer.length, sha256, full_page: Boolean(input.full_page) }), isError: false };
    },
  },
  {
    name: 'browser_downloads',
    description: '列下载账:状态、建议名、安全文件名、MIME、字节、sha256、落盘路径。默认隔离目录,可执行不自动运行、压缩包不自动解压。',
    inputSchema: { type: 'object', properties: {} },
    handler: async () => {
      const rows = session.downloads.map((d) => ({ id: d.id, state: d.state, suggested: d.suggested, filename: d.filename, path: d.path, mime: d.mime, bytes: d.bytes, sha256: d.sha256 }));
      const body = rows.length === 0 ? '(还没有下载)' : rows.map((d) => d.id + ' ' + d.state + ' ' + d.filename + ' ' + d.bytes + 'B sha256=' + d.sha256.slice(0, 12) + '… -> ' + d.path).join('\n');
      return textResult(body, { downloads: rows, downloads_dir: DOWNLOADS_DIR });
    },
  },
];

function countOpenPages() {
  let n = 0;
  for (const entry of session.pages.values()) {
    if (!entry.closed) ++n;
  }
  return n;
}

async function safeTitle(page) {
  try {
    return await page.title({ timeout: 2000 });
  } catch (_) {
    return '';
  }
}

async function pageSettled(page) {
  await page.waitForLoadState('domcontentloaded', { timeout: 3000 }).catch(() => null);
}

function pageMeta(pageId, page, entry, extra) {
  return {
    page_id: pageId,
    url: page.url(),
    title: '',
    generation: entry.generation,
    ...extra,
  };
}

function textResult(text, structured) {
  return { content: [{ type: 'text', text }], structuredContent: structured || {}, isError: false };
}

function describeError(error) {
  if (error && error.message) return String(error.message).split('\n').slice(0, 3).join(' ');
  return String(error);
}

// ---------------------------------------------------------------------------
// Actor 队列:所有工具调用串行,一份浏览器状态一位主人
// ---------------------------------------------------------------------------

let queue = Promise.resolve();

function enqueue(job) {
  const run = queue.then(job, job);
  queue = run.then(() => undefined, () => undefined);
  return run;
}

// ---------------------------------------------------------------------------
// MCP 方法
// ---------------------------------------------------------------------------

const pendingCancellations = new Set();
// 在飞调用账(P1.6):id -> { cancelled }。取消通知命中在飞请求就置旗;
// 单发 Playwright 动作靠自身超时收口,轮询型动作(wait 的 url/固定等待)
// 见旗即停,不硬等满。
const activeCalls = new Map();

async function handleInitialize(id, params) {
  const requested = params && typeof params.protocolVersion === 'string' ? params.protocolVersion : PROTOCOL_VERSION;
  const negotiated = SUPPORTED_VERSIONS.has(requested) ? requested : PROTOCOL_VERSION;
  sendResult(id, {
    protocolVersion: negotiated,
    capabilities: { tools: {} },
    serverInfo: { name: 'lubancode-browser-mcp', version: '0.1.0' },
  });
}

async function handleToolsList(id) {
  sendResult(id, {
    tools: TOOLS.map((tool) => ({
      name: tool.name,
      description: tool.description,
      inputSchema: tool.inputSchema,
      title: tool.name.replace(/^browser_/, '').replace(/_/g, ' '),
      annotations: { readOnlyHint: tool.name === 'browser_status' || tool.name === 'browser_tabs' || tool.name === 'browser_downloads' || tool.name === 'browser_snapshot' },
    })),
  });
}

async function handleToolsCall(id, params) {
  const name = params && params.name;
  const tool = TOOLS.find((t) => t.name === name);
  if (!tool) {
    sendResult(id, { content: [{ type: 'text', text: '未知工具: ' + name }], isError: true });
    return;
  }
  const input = (params && params.arguments) || {};
  const state = { cancelled: false };
  activeCalls.set(id, state);
  const token = { get cancelled() { return state.cancelled; } };
  try {
    const result = await enqueue(() => withDeadline(Promise.resolve(tool.handler(input, token)), callTimeout(input) + 5000, '工具 ' + name));
    sendResult(id, result || { content: [], isError: false });
  } catch (error) {
    if (pendingCancellations.delete(id)) {
      sendResult(id, { content: [{ type: 'text', text: '已取消: ' + name + '(页面未判死,可继续操作)' }], isError: true, structuredContent: { code: 'browser.cancelled' } });
      return;
    }
    const code = error instanceof BrowserError ? error.code : 'browser.internal_error';
    const message = error instanceof BrowserError ? error.message : describeError(error);
    log('tool', name, 'failed:', code, message);
    sendResult(id, { content: [{ type: 'text', text: message }], isError: true, structuredContent: { code } });
  } finally {
    activeCalls.delete(id);
  }
}

const methods = {
  initialize: handleInitialize,
  'tools/list': handleToolsList,
  'tools/call': handleToolsCall,
};

process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
  buffer += chunk;
  let index;
  while ((index = buffer.indexOf('\n')) >= 0) {
    const line = buffer.slice(0, index).trim();
    buffer = buffer.slice(index + 1);
    if (!line) continue;
    let message;
    try {
      message = JSON.parse(line);
    } catch (_) {
      continue;
    }
    handleMessage(message);
  }
});

function handleMessage(message) {
  const method = message && message.method;
  if (method === 'notifications/initialized' || method === 'notifications/cancelled') {
    if (method === 'notifications/cancelled' && message.params && Number.isFinite(message.params.requestId)) {
      pendingCancellations.add(message.params.requestId);
      const inflight = activeCalls.get(message.params.requestId);
      if (inflight) inflight.cancelled = true;
      log('收到取消通知:requestId=', message.params.requestId, inflight ? '(在飞轮询型动作见旗即停)' : '(无在飞调用,记账)');
    }
    return;
  }
  const id = message && message.id;
  if (typeof method !== 'string' || id === undefined) return;
  const handler = methods[method];
  if (!handler) {
    sendError(id, -32601, 'unknown method: ' + method);
    return;
  }
  Promise.resolve(handler(id, message.params || {})).catch((error) => {
    sendError(id, -32603, describeError(error));
  });
}

// ---------------------------------------------------------------------------
// 生命周期:会话结束收 page/context/browser,异常退出也杀净
// ---------------------------------------------------------------------------

let shuttingDown = false;
async function shutdown(code) {
  if (shuttingDown) return;
  shuttingDown = true;
  try {
    await session.shutdown();
  } finally {
    if (releaseLock) releaseLock();
    process.exit(code || 0);
  }
}

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('exit', () => {
  // 同步兜底:异步关不掉就留锁文件说明,下场会话靠 pid 探活接管。
  if (releaseLock) releaseLock();
});
process.stdin.on('end', () => shutdown(0));
process.stdin.on('close', () => shutdown(0));

// persistent 档提前拿锁(首个工具调用前就拒掉抢同目录的第二只进程)。
if (PROFILE_MODE === 'persistent') {
  try {
    releaseLock = acquireProfileLock(USER_DATA_DIR);
  } catch (error) {
    const message = error instanceof BrowserError ? error.message : String(error.message || error);
    log(message);
    // 锁被占:以错误码退出,宿主按 MCP 起服失败处理。
    process.stderr.write(message + '\n');
    process.exit(3);
  }
}

log('browser mcp ready:', { engine: ENGINE, headless: HEADLESS, profile: PROFILE_MODE, user_data_dir: USER_DATA_DIR, downloads_dir: DOWNLOADS_DIR, pid: process.pid });
