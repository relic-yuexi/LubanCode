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

// Console/Network journal(单子:内嵌浏览器调试工作台 阶段 2):
//   - 每页两本环形账(console/network),容量帽 config.journalCap(缺省
//     500/页/账),溢出丢最老并计 dropped——查询回执明说,不冒充全账。
//   - 每条带 seq(页内单调)与 generation,断线后按 since_seq 补账去重。
//   - Network 只记元数据(method/url/status/资源类型/耗时/失败原因),
//     响应体不收——正文明说是元数据账,不冒充抓包。
//   - Console 文本与 URL query 默认脱敏:疑似 token/Authorization/Cookie/
//     密码的键值遮 <redacted>,超长截断。
const JOURNAL_TEXT_CAP = 2000;
const SENSITIVE_KEY_RE = /(authorization|cookie|set-cookie|token|secret|password|passwd|api[-_]?key|session[-_]?id)/i;

function sanitizeJournalText(raw) {
  let text = String(raw ?? '');
  // Bearer 头与"key: value"样式的敏感对遮值。
  text = text.replace(/(Bearer\s+)\S+/gi, '$1<redacted>');
  text = text.replace(new RegExp('(' + SENSITIVE_KEY_RE.source + ')\\s*[:=]\\s*\\S+', 'gi'), '$1=<redacted>');
  if (text.length > JOURNAL_TEXT_CAP) {
    text = text.slice(0, JOURNAL_TEXT_CAP) + '…(超长截断,原 ' + text.length + ' 字符)';
  }
  return text;
}

function sanitizeJournalUrl(raw) {
  try {
    const parsed = new URL(String(raw ?? ''));
    for (const [key, value] of parsed.searchParams.entries()) {
      if (SENSITIVE_KEY_RE.test(key)) parsed.searchParams.set(key, '<redacted>');
    }
    return parsed.toString();
  } catch (_) {
    return String(raw ?? '');
  }
}

// 用户输入监听脚本(仲裁第 4 条:用户一动 DOM,观察代递增,旧 ref 报
// stale)。exposeBinding 装的 window.__lubanOnUserInput 由 session 供给;
// 每份文档挂一次 document 捕获监听,幂等。
const USER_INPUT_WATCH_SCRIPT = `(() => {
  if (window.__lubanUserWatch) return;
  window.__lubanUserWatch = true;
  const touch = () => {
    try { if (typeof window.__lubanOnUserInput === 'function') window.__lubanOnUserInput(); } catch (_) { /* 不拦页面 */ }
  };
  document.addEventListener('pointerdown', touch, true);
  document.addEventListener('keydown', touch, true);
})()`;

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
    this.agentActing = false; // Agent 自家动作进行中:用户观察代不误记(仲裁第 4 条)
    // 事件口(阶段 3:App Server sidecar 用):宿主挂上监听器后,页签开合、
    // 导航、journal 入账、userEpoch 递增、崩溃终态都会从这儿递出去。不挂
    // 就是一根空管——MCP 路与直调路的行为一分不改。监听器抛错一律吞掉:
    // 事件是旁路账,不许把浏览器动作拖死。
    this.eventListener = null;
  }

  // 挂事件监听器(sidecar 专用;fn(type, payload) 同步调)。
  setEventListener(fn) {
    this.eventListener = typeof fn === 'function' ? fn : null;
  }

  emitEvent(type, payload) {
    if (!this.eventListener) return;
    try {
      this.eventListener(type, payload || {});
    } catch (_) {
      /* 旁路账出错不拦正路 */
    }
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
      this.emitEvent('download/event', { ...record });
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
      this.emitEvent('download/event', { ...record });
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
        this.emitEvent('session/crashed', { reason });
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
    const entry = {
      page,
      generation: 0,
      snapshotSeq: 0,
      closed: false,
      // 用户观察代(仲裁第 4 条):用户手点/按键一次 +1;snapshot 记下当时
      // 的代,动作带旧 snapshot 时对表,不等当前代就明报 stale。
      userEpoch: 0,
      snapshotEpochs: new Map(), // snapshot_id -> 快照时的 userEpoch
      // Console journal:环形账,帽 config.journalCap。
      consoleJournal: [],
      consoleSeq: 0,
      consoleDropped: 0,
      // Network journal:同帽;pendingRequests 记在飞请求(request ->
      // 起笔元数据),拿到终态(response/requestfailed)才入账。
      networkJournal: [],
      networkSeq: 0,
      networkDropped: 0,
      pendingRequests: new Map(),
    };
    this.pages.set(id, entry);
    const cap = Math.max(10, Math.trunc(this.config.journalCap) || 500);
    const pushRing = (journal, record, droppedKey) => {
      journal.push(record);
      if (journal.length > cap) {
        // 环形:丢最老,明记 dropped——查询回执如实报,不冒充全账。
        journal.splice(0, journal.length - cap);
        entry[droppedKey] += 1;
      }
    };
    // 主框架导航 = 换页:generation +1,旧 ref 即刻失效(单子 P1.4)。
    page.on('framenavigated', (frame) => {
      if (frame === page.mainFrame()) {
        entry.generation += 1;
        this.emitEvent('page/navigation', {
          pageId: id,
          url: frame.url(),
          generation: entry.generation,
        });
      }
    });
    // Console 与未捕获异常(阶段 2):level 取 Playwright 的 type,pageerror
    // 单列一级;source 行列尽力给,拿不到留空。
    page.on('console', (message) => {
      const location = message.location() || {};
      entry.consoleSeq += 1;
      const record = {
        seq: entry.consoleSeq,
        level: String(message.type() || 'log'),
        text: sanitizeJournalText(message.text()),
        sourceUrl: sanitizeJournalUrl(location.url || ''),
        line: Number(location.lineNumber) || 0,
        column: Number(location.columnNumber) || 0,
        ts: new Date().toISOString(),
        generation: entry.generation,
      };
      pushRing(entry.consoleJournal, record, 'consoleDropped');
      this.emitEvent('console/entry', { pageId: id, entry: record });
    });
    page.on('pageerror', (error) => {
      entry.consoleSeq += 1;
      const record = {
        seq: entry.consoleSeq,
        level: 'pageerror',
        text: sanitizeJournalText(error && error.message ? error.message : String(error)),
        sourceUrl: '',
        line: 0,
        column: 0,
        ts: new Date().toISOString(),
        generation: entry.generation,
      };
      pushRing(entry.consoleJournal, record, 'consoleDropped');
      this.emitEvent('console/entry', { pageId: id, entry: record });
    });
    // Network journal:起笔记 method/url/资源类型,终态补 status 或失败
    // 原因;duration 用起笔到终态的墙钟差。响应体不收(明说,不冒充)。
    page.on('request', (request) => {
      entry.networkSeq += 1;
      entry.pendingRequests.set(request, {
        seq: entry.networkSeq,
        method: request.method(),
        url: sanitizeJournalUrl(request.url()),
        resourceType: request.resourceType() || '',
        startedAt: Date.now(),
        ts: new Date().toISOString(),
        generation: entry.generation,
      });
    });
    page.on('response', (response) => {
      const pending = entry.pendingRequests.get(response.request());
      if (!pending) return;
      entry.pendingRequests.delete(response.request());
      pending.status = response.status();
      pending.durationMs = Math.max(0, Date.now() - pending.startedAt);
      const record = { ...pending, failed: false, error: '' };
      pushRing(entry.networkJournal, record, 'networkDropped');
      this.emitEvent('network/entry', { pageId: id, entry: record });
    });
    page.on('requestfailed', (request) => {
      const pending = entry.pendingRequests.get(request);
      if (!pending) return;
      entry.pendingRequests.delete(request);
      const failure = request.failure() || {};
      pending.status = 0;
      pending.durationMs = Math.max(0, Date.now() - pending.startedAt);
      pending.failed = true;
      pending.error = String(failure.errorText || 'request failed');
      const record = { ...pending };
      pushRing(entry.networkJournal, record, 'networkDropped');
      this.emitEvent('network/entry', { pageId: id, entry: record });
    });
    // 用户输入监听:exposeBinding 供给回调,init script 每份新文档挂监听;
    // 已加载的文档再补挂一次(open(new_page=false) 的老页)。
    page.exposeBinding('__lubanOnUserInput', () => {
      if (this.agentActing) return; // Agent 自家动作不误记
      entry.userEpoch += 1;
      log('user input observed on', id, '-> userEpoch', entry.userEpoch);
      this.emitEvent('user_epoch', { pageId: id, userEpoch: entry.userEpoch });
    }).catch(() => { /* 挂不上不拦功能 */ });
    page.addInitScript(USER_INPUT_WATCH_SCRIPT).catch(() => { /* 同上 */ });
    page.evaluate(USER_INPUT_WATCH_SCRIPT).catch(() => { /* about:blank 等挂不上就算了 */ });
    page.on('close', () => {
      entry.closed = true;
      this.emitEvent('page/closed', { pageId: id, reason: 'closed' });
    });
    page.on('crash', () => {
      entry.closed = true;
      this.emitEvent('page/closed', { pageId: id, reason: 'crashed' });
    });
    this.emitEvent('page/created', { pageId: id });
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
      // 用户观察代对表(仲裁第 4 条):快照之后用户在页面上动过手
      //(手点/按键),旧 ref 一律作废——Agent 重新 snapshot 再动作。
      const epoch = entry.snapshotEpochs.get(String(snapshotId));
      if (epoch !== undefined && epoch !== entry.userEpoch) {
        throw toolError('browser.stale_ref', '用户在页面上动过手(观察代 ' + epoch + ' -> ' + entry.userEpoch + '),snapshot ' + snapshotId + ' 的旧 ref 已过期。重新 browser_snapshot 再动作。');
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

  // Agent 自家的输入动作(click/type/select 的键盘鼠标段)包一层旗:
  // 期间页面监听到的输入事件不算用户动作,不误递观察代。竞态窗口若仍有
  // 迟到事件溜进来,只会多递一代——方向是"多一次快照",安全侧。
  // 协议注入的用户动作(多前端外壳单阶段 B)也包这层旗——注入的
  // pointerdown/keydown 若不遮,监听器会在动作中途多记一代;遮掉之后
  // 由 observeUserInput 在动作收尾时明递恰一代。
  async withAgentInput(job) {
    this.agentActing = true;
    try {
      return await job();
    } finally {
      this.agentActing = false;
    }
  }

  // 用户输入动作的收尾账(阶段 B):owner=user 的注入动作成功了,观察代
  // 明递一代——Agent 拿旧 snapshot 的 ref 再动作,refLocator 对表即报
  // stale(仲裁第 4 条,与真手点同一条规矩)。
  observeUserInput(pageId, entry) {
    entry.userEpoch += 1;
    this.emitEvent('user_epoch', { pageId, userEpoch: entry.userEpoch });
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
    // 快照记下当时的用户观察代:动作带旧 snapshot_id 时对表,用户动过手
    // 就明报 stale(仲裁第 4 条)。只留最近 32 份,更老的靠 generation
    // 前缀校验兜着。
    target.entry.snapshotEpochs.set(snapshotId, target.entry.userEpoch);
    if (target.entry.snapshotEpochs.size > 32) {
      const oldest = target.entry.snapshotEpochs.keys().next().value;
      target.entry.snapshotEpochs.delete(oldest);
    }
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
    await this.withAgentInput(() => withDeadline(locator.click({ timeout: timeoutMs }), timeoutMs, '点击 ' + ref));
    if (options.owner === 'user') {
      this.observeUserInput(target.id, target.entry); // 用户路:观察代 +1,旧 ref 即 stale
    }
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
      await this.withAgentInput(() => withDeadline(locator.pressSequentially(String(text ?? ''), { timeout: timeoutMs, delay: 10 }), timeoutMs, '逐键输入'));
    } else {
      await this.withAgentInput(() => withDeadline(locator.fill(String(text ?? ''), { timeout: timeoutMs }), timeoutMs, '填入'));
    }
    if (options.pressEnter) {
      await this.withAgentInput(() => withDeadline(locator.press('Enter', { timeout: timeoutMs }), timeoutMs, '按回车'));
    }
    if (options.owner === 'user') {
      this.observeUserInput(target.id, target.entry); // 用户路:观察代 +1,旧 ref 即 stale
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
    await this.withAgentInput(() => withDeadline(
      locator.selectOption(picked.map((o) => ({ value: o.value })), { timeout: timeoutMs }),
      timeoutMs, '选 ' + ref,
    ));
    if (options.owner === 'user') {
      this.observeUserInput(target.id, target.entry); // 用户路:观察代 +1,旧 ref 即 stale
    }
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
    this.emitEvent('page/selected', { pageId: String(pageId), url: entry.page.url(), generation: entry.generation });
    return { pageId: String(pageId), url: entry.page.url(), generation: entry.generation };
  }

  // 指名导航(阶段 3:App Server sidecar 的 page/navigate 面):与 open
  // 不同,这里必须给 page_id,在既有页上 goto。generation 递增与 stale 语义
  // 由 framenavigated 监听统一管,与 open 同一本账。
  async navigate(pageId, url, options) {
    options = options || {};
    url = String(url || '');
    checkUrl(url);
    await this.ensureLaunched();
    const entry = this.resolvePage(String(pageId || ''));
    const timeoutMs = this.callTimeout(options);
    const waitUntil = ['load', 'domcontentloaded', 'networkidle'].includes(options.waitUntil) ? options.waitUntil : 'load';
    const response = await withDeadline(entry.page.goto(url, { waitUntil, timeout: timeoutMs }), timeoutMs, '导航 ' + url);
    return {
      pageId: String(pageId),
      url: entry.page.url(),
      title: await safeTitle(entry.page),
      httpStatus: response ? String(response.status()) : '',
      generation: entry.generation,
    };
  }

  // 历史导航:back/forward/reload。Playwright 的 goBack/goForward 在没有
  // 历史时回 null(不算错);回执里 navigated 如实说,不冒充。
  async historyNav(pageId, direction, options) {
    options = options || {};
    await this.ensureLaunched();
    const entry = this.resolvePage(String(pageId || ''));
    const timeoutMs = this.callTimeout(options);
    const before = entry.generation;
    const beforeUrl = entry.page.url();
    let response = null;
    if (direction === 'reload') {
      response = await withDeadline(entry.page.reload({ timeout: timeoutMs }), timeoutMs, '刷新');
    } else if (direction === 'back') {
      response = await withDeadline(entry.page.goBack({ timeout: timeoutMs }), timeoutMs, '后退');
    } else {
      response = await withDeadline(entry.page.goForward({ timeout: timeoutMs }), timeoutMs, '前进');
    }
    await withDeadline(pageSettled(entry.page), Math.min(timeoutMs, 3000), '等页面稳定').catch(() => null);
    const afterUrl = entry.page.url();
    return {
      pageId: String(pageId),
      navigated: direction === 'reload' ? true : afterUrl !== beforeUrl,
      url: afterUrl,
      title: await safeTitle(entry.page),
      httpStatus: response ? String(response.status()) : '',
      generation: entry.generation,
      generationBefore: before,
    };
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

  // Console journal 查询(阶段 2):page_id 对账(unknown_page 明报),
  // 已关页的账仍可查——账在页对象名下,关页不清。since_seq 取"大于"该
  // 号的条目(断线补账口径);level 过滤(log/info/warning/error/debug/
  // pageerror);limit 默认 50、帽 500。
  consoleEntries(pageId, options) {
    options = options || {};
    const entry = this.pages.get(String(pageId || ''));
    if (!entry) {
      throw toolError('browser.unknown_page', 'page_id "' + pageId + '" 不存在(从未开过或浏览器已重启)。用 browser_tabs 列当前页。');
    }
    const sinceSeq = Number(options.sinceSeq) || 0;
    const limit = Math.min(Math.max(Number(options.limit) || 50, 1), 500);
    const wantedLevel = options.level ? String(options.level) : '';
    const rows = entry.consoleJournal.filter((row) => {
      if (row.seq <= sinceSeq) return false;
      if (wantedLevel && row.level !== wantedLevel) return false;
      return true;
    });
    return {
      pageId: String(pageId),
      rows: rows.slice(-limit),
      total: entry.consoleJournal.length,
      dropped: entry.consoleDropped,
      lastSeq: entry.consoleSeq,
    };
  }

  // Network journal 查询(阶段 2):同口径;只记元数据(响应体不收),
  // url_contains/status 过滤;failed=true 只看失败请求。
  networkEntries(pageId, options) {
    options = options || {};
    const entry = this.pages.get(String(pageId || ''));
    if (!entry) {
      throw toolError('browser.unknown_page', 'page_id "' + pageId + '" 不存在(从未开过或浏览器已重启)。用 browser_tabs 列当前页。');
    }
    const sinceSeq = Number(options.sinceSeq) || 0;
    const limit = Math.min(Math.max(Number(options.limit) || 50, 1), 500);
    const urlContains = options.urlContains ? String(options.urlContains) : '';
    const wantedStatus = Number(options.status) || 0;
    const failedOnly = Boolean(options.failedOnly);
    const rows = entry.networkJournal.filter((row) => {
      if (row.seq <= sinceSeq) return false;
      if (urlContains && !row.url.includes(urlContains)) return false;
      if (wantedStatus && row.status !== wantedStatus) return false;
      if (failedOnly && !row.failed) return false;
      return true;
    });
    return {
      pageId: String(pageId),
      rows: rows.slice(-limit),
      total: entry.networkJournal.length,
      dropped: entry.networkDropped,
      lastSeq: entry.networkSeq,
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
