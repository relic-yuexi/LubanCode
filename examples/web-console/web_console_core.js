// LubanCode 参考前端——内核(多前端外壳单阶段 D)。
//
// 纯逻辑,零 DOM:协议通道(WebSocket 上的 AppServer 协议)+ 事件账 reducer
// + 快照 ref 解析 + artifact 取址。浏览器页(web_console_app.js)与 Node
// 冒烟(scripts/tests/app_server_web_console_smoke.js)吃同一份内核——页上
// 怎么走协议,冒烟就怎么验,不许测试另开一条路。
//
// 纪律(单子阶段 D):全程只走 AppServer 协议与承载面:
//   - 请求/通知/审批反向请求:WS 文本帧,一条一帧;
//   - 截图与镜像帧的字节:GET /artifact/<内容寻址名>(承载面,与
//     app_server/auth 同级,不是协议方法);协议事件里只有引用,base64
//     永不出现——内核自己盯着(sawBase64 记账,冒烟要断言)。
// 不 import 内核头文件、不读内核盘上账、不碰 sidecar。
'use strict';

(function (root) {
  // -------------------------------------------------------------------------
  // 取址
  // -------------------------------------------------------------------------

  function buildWsUrl(port, host) {
    return 'ws://' + (host || '127.0.0.1') + ':' + port;
  }

  function buildHttpBaseUrl(port, host) {
    return 'http://' + (host || '127.0.0.1') + ':' + port;
  }

  // artifact 引用(协议事件里只有引用)→ 字节口子的 URL。token 配了就随查询
  // 串递(Bearer 头在 <img src> 里给不了;回环场景,不落任何日志)。
  function buildArtifactUrl(port, artifact, token, host) {
    if (!artifact || !artifact.filename) {
      return '';
    }
    let url = buildHttpBaseUrl(port, host) + '/artifact/' + artifact.filename;
    if (token) {
      url += '?token=' + encodeURIComponent(token);
    }
    return url;
  }

  // -------------------------------------------------------------------------
  // 快照解析:可访问性树文字里的 [ref=eN] 折成 {ref, text} 清单——镜像面板
  // 的元素清单与输入注入(ref)都吃它。协议 1.1 的快照没有坐标,参考前端的
  // "点镜像"= 点元素清单里的一行,动作照协议发 browser/action。
  // -------------------------------------------------------------------------

  function parseSnapshotRefs(snapshotText) {
    const rows = [];
    for (const line of String(snapshotText || '').split('\n')) {
      const hit = /\[ref=(e\d+)\]/.exec(line);
      if (hit) {
        rows.push({ ref: hit[1], text: line.replace(/\[ref=e\d+\]/g, '').trim() });
      }
    }
    return rows;
  }

  // -------------------------------------------------------------------------
  // 协议通道
  // -------------------------------------------------------------------------

  const DEFAULT_REQUEST_TIMEOUT_MS = 60000;

  function ProtocolChannel(options) {
    const opts = options || {};
    this.port = opts.port;
    this.host = opts.host || '127.0.0.1';
    this.token = opts.token || '';
    this.name = opts.name || 'web-console';
    this.socketFactory = opts.socketFactory || null; // 测试注入;缺省用全局 WebSocket
    this.onDisconnect = opts.onDisconnect || null;

    this.nextId = 1;
    this.pending = new Map(); // id -> {resolve, reject, timer}
    this.events = [];         // 收到的全部事件(对账/补账用,帽 4000)
    this.handlers = new Map(); // method -> [fn]
    this.socket = null;
    this.closed = false;
    this.initializeResult = null;

    // 出站纪律的镜子:协议里不该出现 base64 图片正文,收到的每条消息都
    // 验一遍,冒烟/页面上明账(不是防线,是警报)。
    this.sawBase64 = false;
    this.messagesReceived = 0;
  }

  ProtocolChannel.prototype.connect = function () {
    const self = this;
    const factory = this.socketFactory ||
      (typeof WebSocket !== 'undefined' ? WebSocket : null);
    if (!factory) {
      return Promise.reject(new Error('没有可用的 WebSocket 实现'));
    }
    return new Promise((resolve, reject) => {
      let settled = false;
      const socket = new factory(buildWsUrl(self.port, self.host));
      self.socket = socket;
      socket.onopen = function () {
        // 首帧 token 门(承载面):配了 token 的服务,第一条文本帧必须是
        // app_server/auth——服务端不回应,错了直接断线(由 initialize 超时
        // 或 onclose 兜出)。
        if (self.token) {
          socket.send(JSON.stringify({ method: 'app_server/auth', params: { token: self.token } }));
        }
        self.request('initialize', { clientName: self.name }).then((reply) => {
          if (settled) {
            return;
          }
          settled = true;
          self.initializeResult = reply.result || null;
          self.notify('initialized');
          resolve(self.initializeResult);
        }, (error) => {
          if (!settled) {
            settled = true;
            reject(error);
          }
        });
      };
      socket.onmessage = function (frame) {
        self.handleMessage(typeof frame.data === 'string' ? frame.data : String(frame.data));
      };
      socket.onclose = function () {
        if (!settled) {
          settled = true;
          reject(new Error('连接被服务端关了(token 不对或服务已收线)'));
        }
        self.teardown();
      };
      socket.onerror = function () {
        if (!settled) {
          settled = true;
          reject(new Error('WebSocket 连接失败(端口没开?)'));
        }
      };
    });
  };

  ProtocolChannel.prototype.handleMessage = function (text) {
    ++this.messagesReceived;
    if (text.indexOf('dataBase64') >= 0 || text.indexOf('iVBORw0KGgo') >= 0 || text.indexOf('/9j/4AA') >= 0) {
      this.sawBase64 = true;
    }
    let message = null;
    try {
      message = JSON.parse(text);
    } catch (_) {
      return;
    }
    if (message && typeof message.method === 'string') {
      // 事件(含服务端反向请求 permission/request / user/ask)。
      this.events.push(message);
      if (this.events.length > 4000) {
        this.events.splice(0, this.events.length - 4000);
      }
      const list = this.handlers.get(message.method);
      if (list) {
        for (const fn of list) {
          try {
            fn(message.params || {}, message);
          } catch (_) {
            /* 处理器炸了不许掀通道 */ }
        }
      }
      const catchAll = this.handlers.get('*');
      if (catchAll) {
        for (const fn of catchAll) {
          try {
            fn(message.params || {}, message);
          } catch (_) {
            /* 同上 */ }
        }
      }
      return;
    }
    if (message && Number.isFinite(message.id)) {
      const entry = this.pending.get(message.id);
      if (entry) {
        this.pending.delete(message.id);
        clearTimeout(entry.timer);
        entry.resolve(message);
      }
    }
  };

  ProtocolChannel.prototype.request = function (method, params, timeoutMs) {
    const self = this;
    if (this.closed || !this.socket) {
      return Promise.reject(new Error('通道已关: ' + method));
    }
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        self.pending.delete(id);
        reject(new Error('app-server 请求超时: ' + method));
      }, timeoutMs || DEFAULT_REQUEST_TIMEOUT_MS);
      self.pending.set(id, { resolve, reject, timer });
      self.socket.send(JSON.stringify({ id, method, params: params || {} }));
    });
  };

  ProtocolChannel.prototype.notify = function (method, params) {
    if (this.closed || !this.socket) {
      return;
    }
    this.socket.send(JSON.stringify({ method, params: params || {} }));
  };

  // 服务端反向请求(permission/request / user/ask)的响应信封:id 恒 0,
  // 配对靠 requestId。
  ProtocolChannel.prototype.answerServerRequest = function (requestId, payload) {
    this.sendRaw(JSON.stringify({ id: 0, result: Object.assign({ requestId: requestId }, payload || {}) }));
  };

  ProtocolChannel.prototype.sendRaw = function (text) {
    if (!this.closed && this.socket) {
      this.socket.send(text);
    }
  };

  ProtocolChannel.prototype.onEvent = function (method, fn) {
    if (!this.handlers.has(method)) {
      this.handlers.set(method, []);
    }
    this.handlers.get(method).push(fn);
  };

  ProtocolChannel.prototype.waitForEvent = function (method, predicate, timeoutMs) {
    const self = this;
    const deadline = Date.now() + (timeoutMs || 20000);
    return new Promise((resolve) => {
      const poll = function () {
        const hit = self.events.find(function (e) {
          return e.method === method && (!predicate || predicate(e.params || {}, e));
        });
        if (hit) {
          return resolve(hit);
        }
        if (self.closed || Date.now() > deadline) {
          return resolve(null);
        }
        setTimeout(poll, 25);
      };
      poll();
    });
  };

  ProtocolChannel.prototype.teardown = function () {
    if (this.closed) {
      return;
    }
    this.closed = true;
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(new Error('连接收线'));
    }
    this.pending.clear();
    if (this.onDisconnect) {
      this.onDisconnect();
    }
  };

  ProtocolChannel.prototype.close = function () {
    this.teardown();
    if (this.socket) {
      try {
        this.socket.close();
      } catch (_) {
        /* 已断 */ }
    }
  };

  // -------------------------------------------------------------------------
  // 事件账 reducer:协议事件进一只纯状态机,渲染层(app.js)只读不写。
  // 行数有帽(console/network 各留 400 行/页)——参考前端看"现在",补账
  // 走 query,不靠页面上攒全量。
  // -------------------------------------------------------------------------

  const ROW_CAP = 400;

  function ConsoleState() {
    this.threads = [];
    this.currentThreadId = '';
    this.transcriptByThread = {}; // threadId -> [item]
    this.turnStatusByThread = {}; // threadId -> 最近 turn/completed params
    this.usageTotal = { inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, cacheCreationTokens: 0, outputReasoningTokens: 0 };
    this.lastContext = null;
    this.approvals = [];          // 悬着的 permission/request(params 原文)
    this.questions = [];          // 悬着的 user/ask
    this.overflow = { dropped: 0, coalesced: 0 };
    this.browser = {
      session: null,
      launched: false,
      crashedReason: '',
      paused: false,
      pages: {},                  // pageId -> {pageId, title, url, active, generation}
      console: {},                // pageId -> {rows: [], dropped: 0, lastSeq: 0}
      network: {},                // pageId -> {rows: [], dropped: 0, lastSeq: 0}
      userEpoch: {},              // pageId -> number
      mirror: {},                 // pageId -> {artifact, frameSeq, width, height, dropped, at}
      screenshot: {},             // pageId -> image(最近一张)
      actions: {},                // actionId -> {method, owner, ok, error, cancelled, durationMs}
      downloads: [],
    };
    this.listeners = [];
  }

  ConsoleState.prototype.onChange = function (fn) {
    this.listeners.push(fn);
  };

  ConsoleState.prototype.emitChange = function () {
    for (const fn of this.listeners) {
      try {
        fn(this);
      } catch (_) {
        /* 渲染炸了不许掀账 */ }
    }
  };

  ConsoleState.prototype.transcript = function () {
    return this.transcriptByThread[this.currentThreadId] || [];
  };

  ConsoleState.prototype.pageJournal = function (book, pageId) {
    if (!book[pageId]) {
      book[pageId] = { rows: [], dropped: 0, lastSeq: 0 };
    }
    return book[pageId];
  };

  ConsoleState.prototype.appendRows = function (journal, entries) {
    for (const entry of entries || []) {
      journal.rows.push(entry);
      if (entry.seq > journal.lastSeq) {
        journal.lastSeq = entry.seq;
      }
    }
    if (journal.rows.length > ROW_CAP) {
      journal.rows.splice(0, journal.rows.length - ROW_CAP);
    }
  };

  ConsoleState.prototype.upsertItem = function (threadId, itemId, patch, createType) {
    const items = this.transcriptByThread[threadId] || (this.transcriptByThread[threadId] = []);
    let item = null;
    for (const candidate of items) {
      if (candidate.id === itemId) {
        item = candidate;
        break;
      }
    }
    if (!item) {
      item = { id: itemId, type: createType || 'text', text: '', status: 'open' };
      items.push(item);
    }
    Object.assign(item, patch);
    return item;
  };

  ConsoleState.prototype.apply = function (params, message) {
    const method = message && message.method;
    const browser = this.browser;
    switch (method) {
      case 'thread/started':
        this.threads.push(params.threadId);
        break;
      case 'thread/stopped':
      case 'thread/deleted': {
        const at = this.threads.indexOf(params.threadId);
        if (at >= 0) {
          this.threads.splice(at, 1);
        }
        break;
      }
      case 'turn/started':
        this.turnStatusByThread[params.threadId] = { status: 'running', turnId: params.turnId };
        break;
      case 'item/started':
        this.upsertItem(params.threadId, params.item && params.item.id, Object.assign({ status: 'open' }, params.item), params.item && params.item.type);
        break;
      case 'item/delta': {
        const item = this.upsertItem(params.threadId, params.itemId, {});
        item.text = (item.text || '') + (params.delta || '');
        break;
      }
      case 'item/completed':
        this.upsertItem(params.threadId, params.item && params.item.id, Object.assign({ status: 'done' }, params.item));
        break;
      case 'turn/usage':
        this.usageTotal.inputTokens += (params.usage && params.usage.inputTokens) || 0;
        this.usageTotal.outputTokens += (params.usage && params.usage.outputTokens) || 0;
        this.usageTotal.cacheReadTokens += (params.usage && params.usage.cacheReadTokens) || 0;
        this.usageTotal.cacheCreationTokens += (params.usage && params.usage.cacheCreationTokens) || 0;
        this.usageTotal.outputReasoningTokens += (params.usage && params.usage.outputReasoningTokens) || 0;
        break;
      case 'turn/context':
        this.lastContext = params;
        break;
      case 'turn/completed':
        this.turnStatusByThread[params.threadId] = params;
        break;
      case 'queue/overflow':
        this.overflow.dropped += params.dropped || 0;
        this.overflow.coalesced += params.coalesced || 0;
        break;
      case 'permission/request':
        this.approvals.push(params);
        break;
      case 'user/ask':
        this.questions.push(params);
        break;
      case 'browser/started':
        browser.session = params;
        browser.launched = true;
        browser.crashedReason = '';
        break;
      case 'browser/stopped':
        browser.session = null;
        browser.launched = false;
        browser.pages = {};
        browser.mirror = {};
        break;
      case 'browser/crashed':
        browser.crashedReason = params.reason || 'crashed';
        browser.launched = false;
        browser.pages = {};
        browser.mirror = {};
        break;
      case 'browser/paused':
        browser.paused = true;
        break;
      case 'browser/resumed':
        browser.paused = false;
        break;
      case 'browser/page/created':
        browser.pages[params.pageId] = {
          pageId: params.pageId, title: params.title || '', url: params.url || '', active: false, generation: 0,
        };
        break;
      case 'browser/page/updated':
        if (browser.pages[params.pageId]) {
          if (params.url !== undefined) {
            browser.pages[params.pageId].url = params.url;
          }
          if (params.generation !== undefined) {
            browser.pages[params.pageId].generation = params.generation;
          }
          if (params.title !== undefined) {
            browser.pages[params.pageId].title = params.title;
          }
        }
        break;
      case 'browser/navigation':
        if (browser.pages[params.pageId]) {
          browser.pages[params.pageId].url = params.url;
          browser.pages[params.pageId].generation = params.generation;
        }
        break;
      case 'browser/page/closed':
        delete browser.pages[params.pageId];
        delete browser.mirror[params.pageId];
        break;
      case 'browser/console/event': {
        const journal = this.pageJournal(browser.console, params.pageId);
        this.appendRows(journal, params.entries);
        journal.dropped += params.dropped || 0;
        if (Number.isFinite(params.lastSeq) && params.lastSeq > journal.lastSeq) {
          journal.lastSeq = params.lastSeq;
        }
        break;
      }
      case 'browser/network/event': {
        const journal = this.pageJournal(browser.network, params.pageId);
        this.appendRows(journal, params.entries);
        journal.dropped += params.dropped || 0;
        if (Number.isFinite(params.lastSeq) && params.lastSeq > journal.lastSeq) {
          journal.lastSeq = params.lastSeq;
        }
        break;
      }
      case 'browser/download/event':
        browser.downloads.push(params);
        if (browser.downloads.length > 50) {
          browser.downloads.shift();
        }
        break;
      case 'browser/screenshot/ready':
        browser.screenshot[params.pageId] = params.image || null;
        break;
      case 'browser/action/started':
        browser.actions[params.actionId] = {
          method: params.method, owner: params.owner, ok: null, cancelled: false, durationMs: 0,
        };
        break;
      case 'browser/action/completed':
        browser.actions[params.actionId] = {
          method: params.method,
          owner: params.owner,
          ok: params.ok === true,
          error: params.error || null,
          cancelled: params.cancelled === true,
          durationMs: params.durationMs || 0,
        };
        break;
      case 'browser/user_epoch':
        browser.userEpoch[params.pageId] = params.userEpoch;
        break;
      case 'browser/screencast/frame':
        browser.mirror[params.pageId] = {
          artifact: params.artifact || null,
          frameSeq: params.frameSeq,
          width: params.width,
          height: params.height,
          dropped: params.dropped || 0,
          at: Date.now(),
        };
        break;
      default:
        break;
    }
    this.emitChange();
  };

  // 把通道与账接起来:通道收到的每条事件都进 reducer。
  ConsoleState.prototype.attach = function (channel) {
    const self = this;
    channel.onEvent('*', function (params, message) {
      self.apply(params || {}, message);
    });
  };

  // 断线补账(cursor 口径):重连后凭已见 lastSeq 把 console/network 拉平。
  // 调用方(页/冒烟)在重新握手后来一遍。
  ConsoleState.prototype.backfillJournals = function (channel, pageId) {
    const self = this;
    const consoleSeen = this.pageJournal(this.browser.console, pageId).lastSeq;
    const networkSeen = this.pageJournal(this.browser.network, pageId).lastSeq;
    return channel.request('browser/console/query', { pageId: pageId, sinceSeq: consoleSeen, limit: 200 }).then(function (reply) {
      self.appendRows(self.pageJournal(self.browser.console, pageId), reply.result && reply.result.rows);
      if (reply.result && Number.isFinite(reply.result.lastSeq)) {
        self.pageJournal(self.browser.console, pageId).lastSeq = reply.result.lastSeq;
      }
      return channel.request('browser/network/query', { pageId: pageId, sinceSeq: networkSeen, limit: 200 });
    }).then(function (reply) {
      self.appendRows(self.pageJournal(self.browser.network, pageId), reply.result && reply.result.rows);
      if (reply.result && Number.isFinite(reply.result.lastSeq)) {
        self.pageJournal(self.browser.network, pageId).lastSeq = reply.result.lastSeq;
      }
      self.emitChange();
    });
  };

  const api = {
    buildWsUrl: buildWsUrl,
    buildHttpBaseUrl: buildHttpBaseUrl,
    buildArtifactUrl: buildArtifactUrl,
    parseSnapshotRefs: parseSnapshotRefs,
    ProtocolChannel: ProtocolChannel,
    ConsoleState: ConsoleState,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api; // Node 冒烟走这条路(与页上同一份代码)
  }
  root.LubanWebConsole = api;
})(typeof window !== 'undefined' ? window : globalThis);
