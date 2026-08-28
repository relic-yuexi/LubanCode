// Session 层:BrowserRuntime 的本体。Playwright 生命周期、页签账、
// ref 体系、下载账、崩溃终态、profile 进程锁、动作串行队列,全归这里。
//
// 边界(单子:内嵌浏览器调试工作台 阶段 1):
//   - 本文件不碰 stdio、不碰 JSON-RPC、不组 MCP 文案——那是 transport
//     与 tool adapter 的事。方法返回纯数据,错误一律抛 BrowserError。
//   - 页面状态只有这一本账:this.pages。MCP adapter、App Server、直调
//     测试宿主都调这套方法,谁也不许另建第二份 page registry。
//   - 一份浏览器状态一位主人:并发调用经 enqueue 排进同一队列串行跑。
//
// 术语(与 docs/reference/browser-runtime.md 冻结版一致):
//   session_id 一场浏览器会话;page_id 形如 p1,不复用;generation 主框架
//   每次导航 +1;snapshot_id 形如 p1-g2-s3;ref 形如 e12,绑定
//   page_id + generation + snapshot_id,导航换页即 stale。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const { log } = require('./config');

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

// 错误回形:BrowserError 用自带 code;其余算 internal。MCP adapter 与
// 直调宿主共用,保证两条路的错误语义一致。
function errorShape(error) {
  if (error instanceof BrowserError) return { code: error.code, message: error.message };
  return { code: 'browser.internal_error', message: describeError(error) };
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

// ---------------------------------------------------------------------------
// 内部助手
// ---------------------------------------------------------------------------

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

function describeError(error) {
  if (error && error.message) return String(error.message).split('\n').slice(0, 3).join(' ');
  return String(error);
}

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

// ---------------------------------------------------------------------------
// BrowserSession
// ---------------------------------------------------------------------------

class BrowserSession {
  constructor(config) {
    this.config = config;   // buildConfig 的输出,唯一一份常量
    this.browser = null;    // Playwright Browser(persistent 档为 null,context 即全部)
    this.context = null;    // BrowserContext
    this.pages = new Map(); // page_id -> { page, generation, snapshotSeq, closed }
    this.nextPageNumber = 1;
    this.downloads = [];    // { id, state, suggested, filename, path, mime, bytes, sha256 }
    this.crashed = false;
    this.crashReason = '';
    this.queue = Promise.resolve(); // actor 队列:并发动作串行
    this.releaseLock = null;
  }

  // actor 队列:一份浏览器状态一位主人,两只并发调用不得同时抢一只 page。
  enqueue(job) {
    const run = this.queue.then(job, job);
    this.queue = run.then(() => undefined, () => undefined);
    return run;
  }

  // persistent 档拿 profile 进程锁(幂等)。抢不到抛 browser.profile_locked,
  // 宿主决定怎么退场。shutdown 时自动释放。
  lockProfile() {
    if (this.releaseLock) return;
    this.releaseLock = acquireProfileLock(this.config.userDataDir);
  }

  // 同步兜底:process exit 钩子里也调它,异步关不掉至少把锁摘了。
  releaseLockNow() {
    if (this.releaseLock) {
      const release = this.releaseLock;
      this.releaseLock = null;
      release();
    }
  }

  async ensureLaunched() {
    if (this.context) {
      if (this.crashed) {
        throw toolError('browser.crashed', '浏览器已崩溃(' + this.crashReason + ')。旧 page id 全部失效;调 browser_status 查看重启状态,再重新 browser_open。');
      }
      return;
    }
    const pw = loadPlaywright();
    const engine = pw[this.config.engine];
    if (!engine) throw toolError('browser.engine_missing', 'engine "' + this.config.engine + '" 不可用(需要 npm install 装齐浏览器: npx playwright install ' + this.config.engine + ')。');
    try {
      if (this.config.profileMode === 'persistent') {
        fs.mkdirSync(this.config.userDataDir, { recursive: true });
        this.context = await engine.launchPersistentContext(this.config.userDataDir, {
          headless: this.config.headless,
          viewport: this.config.viewport,
          acceptDownloads: true,
        });
        this.browser = this.context.browser() || null;
      } else {
        this.browser = await engine.launch({ headless: this.config.headless });
        this.context = await this.browser.newContext({ viewport: this.config.viewport, acceptDownloads: true });
      }
    } catch (error) {
      this.context = null;
      this.browser = null;
      if (error instanceof BrowserError) throw error;
      throw toolError('browser.launch_failed', '浏览器起不来(engine=' + this.config.engine + ',headless=' + this.config.headless + '):' + String(error.message || error) + '。浏览器没装时先跑 npx playwright install ' + this.config.engine + '。');
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
        fs.mkdirSync(this.config.downloadsDir, { recursive: true });
        const safe = sanitizeFilename(download.suggestedFilename() || 'download.bin');
        const target = uniquePath(path.join(this.config.downloadsDir, safe));
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
    this.releaseLockNow();
    if (errors.length > 0) log('shutdown 清理有失败项:', errors.join('; '));
  }

  countOpenPages() {
    let n = 0;
    for (const entry of this.pages.values()) {
      if (!entry.closed) ++n;
    }
    return n;
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

  // 动作目标:给了 page_id 就按账本解析,没给就取活动页。
  async target(pageId) {
    return pageId ? { id: pageId, entry: this.resolvePage(pageId) } : await this.activePage();
  }

  // ref 解析:自建 ref 体系——snapshot 在页面里登记 ref -> CSS
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

  // 单次动作的墙钟:没给用 config 默认,给了就夹在 [1000, 60000]。
  callTimeout(options) {
    const asked = Number(options && options.timeoutMs);
    if (!Number.isFinite(asked)) return this.config.defaultActionTimeoutMs;
    return Math.min(Math.max(Math.trunc(asked), 1000), this.config.maxActionTimeoutMs);
  }

  // -------------------------------------------------------------------------
  // 能力面:open / snapshot / click / type / select / wait / tabs /
  // selectPage / closePage / screenshot / listDownloads / status。
  // 返回纯数据(camelCase 字段),错误一律抛 BrowserError。
  // -------------------------------------------------------------------------

  async status() {
    let pid = null;
    if (this.browser) {
      try {
        const proc = this.browser.process();
        pid = proc ? proc.pid : null; // webkit 拿不到 process 就置空
      } catch (_) { /* 崩溃途中不纠结 */ }
    }
    return {
      engine: this.config.engine,
      headless: this.config.headless,
      profile: this.config.profileMode,
      profileName: this.config.profileMode === 'persistent' ? this.config.profileName : null,
      userDataDir: this.config.profileMode === 'persistent' ? this.config.userDataDir : null,
      launched: Boolean(this.context),
      crashed: this.crashed,
      crashReason: this.crashed ? this.crashReason : '',
      browserPid: pid,
      pages: this.countOpenPages(),
      downloadsDir: this.config.downloadsDir,
    };
  }

  async open(url, options) {
    options = options || {};
    url = String(url || '');
    checkUrl(url);
    await this.ensureLaunched();
    const timeoutMs = this.callTimeout(options);
    const waitUntil = ['load', 'domcontentloaded', 'networkidle'].includes(options.waitUntil) ? options.waitUntil : 'load';
    let page;
    let pageId;
    if (options.newPage === false) {
      const active = await this.activePage();
      pageId = active.id;
      page = active.entry.page;
    } else {
      page = await withDeadline(this.context.newPage(), timeoutMs, '开新页');
      pageId = this.registerPage(page).id;
    }
    const response = await withDeadline(page.goto(url, { waitUntil, timeout: timeoutMs }), timeoutMs, '打开 ' + url);
    const entry = this.pages.get(pageId);
    return {
      pageId,
      url: page.url(),
      title: await safeTitle(page),
      httpStatus: response ? String(response.status()) : '',
      generation: entry.generation,
    };
  }

  async snapshot(pageId, options) {
    options = options || {};
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
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
    const maxChars = Math.min(Math.max(Number(options.maxChars) || 20000, 500), 200000);
    const truncated = yaml.length > maxChars;
    return {
      pageId: target.id,
      generation: target.entry.generation,
      url: page.url(),
      snapshotId,
      truncated,
      text: truncated ? yaml.slice(0, maxChars) + '\n[快照超字符帽,已截断;要更全给 max_chars]' : yaml,
    };
  }

  async click(pageId, ref, options) {
    options = options || {};
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
    ref = String(ref || '');
    const locator = await this.refLocator(target.id, target.entry, ref, options.snapshotId);
    const count = await withDeadline(locator.count(), timeoutMs, '数目标');
    if (count === 0) throw toolError('browser.target_not_found', 'ref ' + ref + ' 在当前页面找不到(0 个目标)。页面变了就重新 browser_snapshot。');
    if (count > 1) throw toolError('browser.target_not_unique', 'ref ' + ref + ' 解析到 ' + count + ' 个目标,拒绝乱点第一个。重新 browser_snapshot 拿唯一 ref。');
    const before = target.entry.generation;
    await withDeadline(locator.click({ timeout: timeoutMs }), timeoutMs, '点击 ' + ref);
    // 点击后等"真稳态":framenavigated 异步注册,waitForLoadState 对已
    // 载完的页会立即返回——先给换页一小口气(1.5s 内代数变了才算导航),
    // 再等 domcontentloaded 收尾。
    for (let i = 0; i < 15 && target.entry.generation === before; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    await withDeadline(pageSettled(target.entry.page), Math.min(timeoutMs, 3000), '等页面稳定').catch(() => null);
    const entry = target.entry;
    return {
      pageId: target.id,
      clickedRef: ref,
      navigated: entry.generation !== before,
      generationBefore: before,
      generation: entry.generation,
      url: entry.page.url(),
    };
  }

  async type(pageId, ref, text, options) {
    options = options || {};
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
    ref = String(ref || '');
    const locator = await this.refLocator(target.id, target.entry, ref, options.snapshotId);
    const count = await withDeadline(locator.count(), timeoutMs, '数目标');
    if (count !== 1) {
      throw toolError(count === 0 ? 'browser.target_not_found' : 'browser.target_not_unique', 'ref ' + ref + ' 解析到 ' + count + ' 个目标(要恰一个)。');
    }
    let isPassword = false;
    try {
      isPassword = await locator.evaluate((el) => el instanceof HTMLElement && (el.type === 'password' || el.getAttribute('type') === 'password'));
    } catch (_) { /* 评不了就当普通框 */ }
    if (options.mode === 'type') {
      await withDeadline(locator.pressSequentially(String(text ?? ''), { timeout: timeoutMs, delay: 10 }), timeoutMs, '逐键输入');
    } else {
      await withDeadline(locator.fill(String(text ?? ''), { timeout: timeoutMs }), timeoutMs, '填入');
    }
    if (options.pressEnter) {
      await withDeadline(locator.press('Enter', { timeout: timeoutMs }), timeoutMs, '按回车');
    }
    // 密码值永不出账:typed 只回 '[password]'。
    return {
      pageId: target.id,
      typed: isPassword ? '[password]' : String(text ?? ''),
      password: isPassword,
      url: target.entry.page.url(),
      generation: target.entry.generation,
    };
  }

  async select(pageId, ref, options) {
    options = options || {};
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
    ref = String(ref || '');
    const locator = await this.refLocator(target.id, target.entry, ref, options.snapshotId);
    const count = await withDeadline(locator.count(), timeoutMs, '数目标');
    if (count !== 1) {
      throw toolError(count === 0 ? 'browser.target_not_found' : 'browser.target_not_unique', 'ref ' + ref + ' 解析到 ' + count + ' 个目标(要恰一个)。');
    }
    const asList = (v) => {
      if (Array.isArray(v)) return v.map(String);
      if (v === undefined || v === null || v === '') return [];
      return [String(v)];
    };
    const wantedValues = asList(options.value);
    const wantedLabels = asList(options.label);
    if (wantedValues.length === 0 && wantedLabels.length === 0) {
      throw toolError('browser.schema', '给 value(option 的 value 属性)或 label(option 可见文本)至少一样;快照里下拉行带"选项: value=文本"清单。');
    }
    // 先验明是 <select> 并读全部选项:按不到值时把候选整个回给调用方,
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
      throw toolError('browser.bad_target', 'ref ' + ref + ' 不是 <select> 下拉框(browser_select 只管下拉;文本框用 browser_type)。');
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
      timeoutMs, '选 ' + ref,
    );
    // 选完读回实际选中项:回执以页面为准,不以请求为准。
    const chosen = await withDeadline(
      locator.evaluate((el) => Array.from(el.selectedOptions).map((o) => ({ value: o.value, label: String(o.label || o.text || '').trim() }))),
      timeoutMs, '读回选中项',
    ).catch(() => picked.map((o) => ({ value: o.value, label: o.label })));
    return {
      pageId: target.id,
      selectedRef: ref,
      selected: chosen,
      multiple: info.multiple,
      url: target.entry.page.url(),
      generation: target.entry.generation,
    };
  }

  async wait(pageId, options) {
    options = options || {};
    const token = options.token;
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
    const page = target.entry.page;
    if (options.forText) {
      // waitForSelector 自身的超时给宽一拍(墙钟先到先收口);它偶发的
      // 非超时错(页面忙/上下文换新)也按等待未成口径收口,不抖成
      // internal_error——等待失败对调用方就是一个语义:没等到。
      try {
        await withDeadline(
          page.waitForSelector('text=' + JSON.stringify(String(options.forText)), { timeout: timeoutMs + 5000 }),
          timeoutMs, '等文本 "' + options.forText + '"');
      } catch (error) {
        if (error instanceof BrowserError) throw error;
        throw toolError('browser.timeout', '等文本 "' + options.forText + '" 在 ' + timeoutMs + 'ms 内没等到(' + describeError(error) + '),当前 ' + page.url());
      }
      return { pageId: target.id, waitedFor: 'text', forText: String(options.forText), url: page.url(), generation: target.entry.generation };
    }
    if (options.urlContains) {
      const started = Date.now();
      while (Date.now() - started < timeoutMs) {
        if (token && token.cancelled) {
          throw toolError('browser.cancelled', '等待已取消(页面未判死,URL 当前 ' + page.url() + ')。');
        }
        if (page.url().includes(String(options.urlContains))) {
          return { pageId: target.id, waitedFor: 'url', urlContains: String(options.urlContains), url: page.url(), generation: target.entry.generation };
        }
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
      throw toolError('browser.timeout', '等 URL 含 "' + options.urlContains + '" 超过 ' + timeoutMs + 'ms,当前 ' + page.url());
    }
    const ms = Math.min(Math.max(Number(options.ms) || 0, 0), this.config.maxActionTimeoutMs);
    if (ms > 0) {
      const sliceEnd = Date.now() + ms;
      while (Date.now() < sliceEnd) {
        if (token && token.cancelled) {
          throw toolError('browser.cancelled', '等待已取消(页面未判死)。');
        }
        await new Promise((resolve) => setTimeout(resolve, Math.min(100, sliceEnd - Date.now())));
      }
      return { pageId: target.id, waitedFor: 'fixed', ms, url: page.url(), generation: target.entry.generation };
    }
    throw toolError('browser.schema', '给一个条件:for_text / url_contains / ms。');
  }

  async tabs() {
    await this.ensureLaunched();
    const rows = [];
    for (const [id, entry] of this.pages) {
      rows.push({
        page_id: id,
        title: await safeTitle(entry.page),
        url: entry.page.url(),
        active: !entry.closed,
        generation: entry.generation,
      });
    }
    return rows;
  }

  async selectPage(pageId, options) {
    options = options || {};
    await this.ensureLaunched();
    const entry = this.resolvePage(String(pageId || ''));
    await withDeadline(entry.page.bringToFront(), this.callTimeout(options), '切页');
    return { pageId: String(pageId), url: entry.page.url(), generation: entry.generation };
  }

  async closePage(pageId, options) {
    options = options || {};
    await this.ensureLaunched();
    const entry = this.resolvePage(String(pageId || ''));
    await withDeadline(entry.page.close({ runBeforeUnload: false }), this.callTimeout(options), '关页');
    entry.closed = true;
    return { closedPageId: String(pageId), openPages: this.countOpenPages() };
  }

  async screenshot(pageId, options) {
    options = options || {};
    await this.ensureLaunched();
    const target = await this.target(pageId);
    const timeoutMs = this.callTimeout(options);
    const page = target.entry.page;
    const pwOptions = { timeout: timeoutMs, type: 'png' };
    if (options.fullPage) pwOptions.fullPage = true;
    let buffer;
    if (options.ref) {
      const locator = await this.refLocator(target.id, target.entry, String(options.ref), options.snapshotId);
      buffer = await withDeadline(locator.screenshot(pwOptions), timeoutMs, '截元素');
    } else {
      buffer = await withDeadline(page.screenshot(pwOptions), timeoutMs, '截图');
    }
    const sha256 = crypto.createHash('sha256').update(buffer).digest('hex');
    return {
      pageId: target.id,
      url: page.url(),
      generation: target.entry.generation,
      fullPage: Boolean(options.fullPage),
      buffer,
      sha256,
    };
  }

  listDownloads() {
    const rows = this.downloads.map((d) => ({ id: d.id, state: d.state, suggested: d.suggested, filename: d.filename, path: d.path, mime: d.mime, bytes: d.bytes, sha256: d.sha256 }));
    return { rows, downloadsDir: this.config.downloadsDir };
  }
}

module.exports = {
  BrowserSession,
  BrowserError,
  toolError,
  errorShape,
  acquireProfileLock,
  SNAPSHOT_SCRIPT,
  withDeadline,
  describeError,
};
