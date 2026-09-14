// LubanCode 助理主界面——内核(W1)。
//
// 纯逻辑,零 DOM:协议通道(WebSocket 上的 AppServer 1.3 协议)+ 事件账
// reducer + bootstrap 交换。页面(assistant_app.js)与 Node e2e
// (run_e2e.js)吃同一份内核——页上怎么走协议,e2e 就怎么验,不许测试
// 另开一条路。
//
// 复用参考前端(examples/web-console)的协议客户端与事件账经验,按
// 助理模式裁剪:
//   - 认证不是首帧 token 门,是本地会话 cookie(bootstrap 交换发放,
//     HttpOnly,JS 读不到——所以交换走 fetch,不走 WS);
//   - 断线合同是 detached:WS 断了只影响订阅,重连后凭领域快照
//     (thread/list、thread/read、operation/read)补账,不把页面当账本;
//   - 占用/接管:第二条控制连接收到 assistant/connection/occupied 通报,
//     用户显式接管时带 ?takeover=1 重连。
'use strict';

(function (root) {
  // 协议兼容声明(壳不捆绑内核,版本对齐靠声明+连上核对):本前端按
  // 助理模式(capabilities.mode === 'assistant')与 1.x 协议面验过。
  const PROTOCOL_COMPAT = { min: '1.3', max: '1.3' };

  function compareVersions(a, b) {
    const pa = String(a || '').split('.');
    const pb = String(b || '').split('.');
    for (let i = 0; i < 2; ++i) {
      const na = parseInt(pa[i], 10) || 0;
      const nb = parseInt(pb[i], 10) || 0;
      if (na !== nb) return na < nb ? -1 : 1;
    }
    return 0;
  }

  function checkProtocolCompat(initializeResult) {
    const version = initializeResult && initializeResult.protocolVersion;
    const caps = (initializeResult && initializeResult.capabilities) || {};
    if (!version) {
      return { ok: false, reason: '内核没报协议版本' };
    }
    if (compareVersions(version, PROTOCOL_COMPAT.min) < 0 ||
        compareVersions(version, PROTOCOL_COMPAT.max) > 0) {
      return { ok: false, reason: '内核协议 ' + version + ',本前端声明兼容 ' +
        PROTOCOL_COMPAT.min + ' ~ ' + PROTOCOL_COMPAT.max };
    }
    if (caps.mode !== 'assistant' || caps.workLifetime !== 'detached') {
      return { ok: false, reason: '对端不是助理模式服务(capabilities.mode != assistant)' };
    }
    return { ok: true };
  }

  // -------------------------------------------------------------------------
  // bootstrap 交换:URL fragment 里的一次性凭据(#b=...)换会话 cookie。
  // fragment 本就不发往服务端;交换成功立即清 fragment(history.replaceState,
  // 不留历史记录)。同源 fetch 自带/续 cookie;HttpOnly 由服务端 Set-Cookie。
  // 返回 {ok, reason?};浏览器拒 cookie 时如实报,不降级免认证。
  // -------------------------------------------------------------------------
  function exchangeBootstrap(secret) {
    return fetch('/auth/exchange', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: secret,
    }).then(function (reply) {
      if (reply.status === 204 || reply.status === 200) {
        return { ok: true };
      }
      return { ok: false, reason: reply.status === 403 ? '凭据无效或已用过(重启 lubancode assistant 拿新地址)' : ('交换失败 HTTP ' + reply.status) };
    }).catch(function (error) {
      return { ok: false, reason: '交换请求发不出: ' + error };
    });
  }

  function readBootstrapFragment() {
    const hash = window.location.hash || '';
    const hit = /^#b=([0-9a-f]{16,256})$/.exec(hash);
    return hit ? hit[1] : '';
  }

  function clearBootstrapFragment() {
    if (window.location.hash && window.location.hash.indexOf('#b=') === 0) {
      history.replaceState(null, '', window.location.pathname);
    }
  }

  // -------------------------------------------------------------------------
  // 协议通道(cookie 已就绪后连 /ws;请求/事件与 web-console 同款)
  // -------------------------------------------------------------------------
  const DEFAULT_REQUEST_TIMEOUT_MS = 60000;

  function ProtocolChannel(options) {
    const opts = options || {};
    this.port = opts.port;
    this.host = opts.host || window.location.hostname || '127.0.0.1';
    this.path = opts.path || '/ws';
    this.socketFactory = opts.socketFactory || null; // e2e 注入;缺省用全局 WebSocket
    this.onDisconnect = opts.onDisconnect || null;
    this.onOccupied = opts.onOccupied || null;

    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    this.handlers = new Map();
    this.socket = null;
    this.closed = false;
    this.initializeResult = null;
    this.messagesReceived = 0;
  }

  ProtocolChannel.prototype.connect = function () {
    const self = this;
    const factory = this.socketFactory || (typeof WebSocket !== 'undefined' ? WebSocket : null);
    if (!factory) {
      return Promise.reject(new Error('没有可用的 WebSocket 实现'));
    }
    return new Promise(function (resolve, reject) {
      let settled = false;
      const url = 'ws://' + self.host + ':' + self.port + self.path;
      const socket = new factory(url);
      self.socket = socket;
      socket.onopen = function () {
        // 认证在升级前的 HTTP 层(cookie);这里直接握手。
        self.request('initialize', { clientName: 'assistant-web' }).then(function (reply) {
          if (settled) return;
          settled = true;
          self.initializeResult = reply.result || null;
          self.notify('initialized');
          resolve(self.initializeResult);
        }, function (error) {
          if (!settled) { settled = true; reject(error); }
        });
      };
      socket.onmessage = function (frame) {
        self.handleMessage(typeof frame.data === 'string' ? frame.data : String(frame.data));
      };
      socket.onclose = function () {
        if (!settled) {
          settled = true;
          reject(new Error('连接被服务端关了(没有会话权限或服务已收线)'));
        }
        self.teardown();
      };
      socket.onerror = function () {
        if (!settled) {
          settled = true;
          reject(new Error('WebSocket 连接失败'));
        }
      };
    });
  };

  ProtocolChannel.prototype.handleMessage = function (text) {
    ++this.messagesReceived;
    let message = null;
    try { message = JSON.parse(text); } catch (_) { return; }
    if (message && typeof message.method === 'string') {
      this.events.push(message);
      if (this.events.length > 4000) {
        this.events.splice(0, this.events.length - 4000);
      }
      if (message.method === 'assistant/connection/occupied' && this.onOccupied) {
        this.onOccupied(message.params || {});
      }
      const list = this.handlers.get(message.method);
      if (list) {
        for (const fn of list) {
          try { fn(message.params || {}, message); } catch (_) { /* 不掀通道 */ }
        }
      }
      const catchAll = this.handlers.get('*');
      if (catchAll) {
        for (const fn of catchAll) {
          try { fn(message.params || {}, message); } catch (_) { /* 同上 */ }
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
    return new Promise(function (resolve, reject) {
      const timer = setTimeout(function () {
        self.pending.delete(id);
        reject(new Error('请求超时: ' + method));
      }, timeoutMs || DEFAULT_REQUEST_TIMEOUT_MS);
      self.pending.set(id, { resolve: resolve, reject: reject, timer: timer });
      self.socket.send(JSON.stringify({ id: id, method: method, params: params || {} }));
    });
  };

  ProtocolChannel.prototype.notify = function (method, params) {
    if (this.closed || !this.socket) return;
    this.socket.send(JSON.stringify({ method: method, params: params || {} }));
  };

  // 服务端反向请求(permission/request / user/ask)的响应信封:id 恒 0,
  // 配对靠 requestId。
  ProtocolChannel.prototype.answerServerRequest = function (requestId, payload) {
    this.sendRaw(JSON.stringify({ id: 0, result: Object.assign({ requestId: requestId }, payload || {}) }));
  };

  ProtocolChannel.prototype.sendRaw = function (text) {
    if (!this.closed && this.socket) this.socket.send(text);
  };

  ProtocolChannel.prototype.onEvent = function (method, fn) {
    if (!this.handlers.has(method)) this.handlers.set(method, []);
    this.handlers.get(method).push(fn);
  };

  ProtocolChannel.prototype.close = function () {
    this.teardown();
    if (this.socket) {
      try { this.socket.close(); } catch (_) { /* 已断 */ }
    }
  };

  ProtocolChannel.prototype.teardown = function () {
    if (this.closed) return;
    this.closed = true;
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(new Error('连接收线'));
    }
    this.pending.clear();
    if (this.onDisconnect) this.onDisconnect();
  };

  // -------------------------------------------------------------------------
  // 聊天事件账 reducer(页面只读不写;真账在服务端,刷新走 thread/read)
  // -------------------------------------------------------------------------
  function ChatState() {
    this.threads = [];               // thread/list 的投影(展示用)
    this.currentThreadId = '';
    this.itemsByThread = {};         // threadId -> [item](仅本轮在场的增量的画)
    this.turnStatusByThread = {};    // threadId -> running | 最近 completed params
    this.approvals = [];             // 悬着的 permission/request
    this.questions = [];             // 悬着的 user/ask
    this.overflow = { dropped: 0, coalesced: 0 };
    this.listeners = [];
  }

  ChatState.prototype.onChange = function (fn) { this.listeners.push(fn); };
  ChatState.prototype.emitChange = function () {
    for (const fn of this.listeners) {
      try { fn(this); } catch (_) { /* 渲染炸了不掀账 */ }
    }
  };

  ChatState.prototype.items = function () {
    return this.itemsByThread[this.currentThreadId] || [];
  };

  ChatState.prototype.upsertItem = function (threadId, itemId, patch, createType) {
    const items = this.itemsByThread[threadId] || (this.itemsByThread[threadId] = []);
    let item = null;
    for (const candidate of items) {
      if (candidate.id === itemId) { item = candidate; break; }
    }
    if (!item) {
      item = { id: itemId, type: createType || 'text', text: '', status: 'open' };
      items.push(item);
    }
    Object.assign(item, patch);
    return item;
  };

  ChatState.prototype.apply = function (params, message) {
    const method = message && message.method;
    switch (method) {
      case 'turn/started':
        this.turnStatusByThread[params.threadId] = { status: 'running', turnId: params.turnId };
        break;
      case 'item/started':
        this.upsertItem(params.threadId, params.item && params.item.id,
          Object.assign({ status: 'open' }, params.item), params.item && params.item.type);
        break;
      case 'item/delta': {
        const item = this.upsertItem(params.threadId, params.itemId, {});
        item.text = (item.text || '') + (params.delta || '');
        break;
      }
      case 'item/completed':
        this.upsertItem(params.threadId, params.item && params.item.id,
          Object.assign({ status: 'done' }, params.item));
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
      default:
        break;
    }
    this.emitChange();
  };

  ChatState.prototype.attach = function (channel) {
    const self = this;
    channel.onEvent('*', function (params, message) { self.apply(params || {}, message); });
  };

  const api = {
    PROTOCOL_COMPAT: PROTOCOL_COMPAT,
    checkProtocolCompat: checkProtocolCompat,
    exchangeBootstrap: exchangeBootstrap,
    readBootstrapFragment: readBootstrapFragment,
    clearBootstrapFragment: clearBootstrapFragment,
    ProtocolChannel: ProtocolChannel,
    ChatState: ChatState,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api; // Node e2e 走这条(与页上同一份代码)
  }
  root.LubanAssistant = api;
})(typeof window !== 'undefined' ? window : globalThis);
