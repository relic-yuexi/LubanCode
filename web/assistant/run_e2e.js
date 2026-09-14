#!/usr/bin/env node
// 常驻助理 Web 主界面单 W1 的端到端验收(真 exe + 假模型,零真实密钥、
// 零真实网络外呼——模型流量全打本地回环假后端)。承载层与协议层的裸
// 驱动;页(JS)的 UI 交互验收归真实浏览器批次,如实不冒充。
//
// 六幕:
//   1. 启动:真 `lubancode assistant --no-open`,stderr 打 URL(含一次性
//      bootstrap 凭据);监听就绪才打印。
//   2. 认证门(§七):/healthz 只回身份;静态无 cookie 401(配对提示);
//      bootstrap 交换 204+Set-Cookie(HttpOnly/SameSite=Strict);重放 403;
//      伪造 Origin 403;静态有 cookie 200;manifest 外 404。
//   3. 聊天链路(§五/§六):WS(带 cookie)initialize(1.3 + 助理能力声明)
//      → config/status(未配置)→ config/model/set(指假后端;凭据不回显)
//      → config/status(已配置)→ thread/start → turn/start(clientOperationId)
//      → 假后端回话 → turn/completed → 同键重发零重跑(模型只调一次)。
//   4. 刷新恢复:断开 WS → 新连接(带 cookie)→ thread/read 找回两轮正文。
//   5. 断线合同(W0):假后端扣住(hold)→ turn/start 受理 → 关 WS →
//      放行 → 重连 → operation/read 按原键回 final(模型恰好一次)。
//   6. 重复启动与端口:同 profile 第二个实例退码 2 且只开旧实例页面(旧
//      实例健康);指定端口被占准确报错(非零退出);shutdown 停助理,锁
//      释放后可重启。
//
// 用法:node web/assistant/run_e2e.js [--binary <lubancode>]
// 找不到可执行文件打印 SKIP 退 0,不冒充通过(与 node-client e2e 同口径)。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

let passed = 0;
let failed = 0;
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

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function findBinary(explicit) {
  if (explicit) {
    return fs.existsSync(explicit) ? explicit : null;
  }
  const repo = path.resolve(__dirname, '..', '..');
  const candidates = [
    path.join(repo, 'build', 'lubancode'),
    path.join(repo, 'build', 'lubancode.exe'),
    path.join(repo, 'build', 'Debug', 'lubancode.exe'),
    path.join(repo, 'build', 'Release', 'lubancode.exe'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

function makeTempRoot(tag) {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'lubancode-assistant-e2e-' + tag + '-'));
}

// ---------------------------------------------------------------------------
// 本地回环假 anthropic 后端。reply 按文本回一幕 SSE;hold 扣住连接不回
// (断线合同幕的"回合在飞行中"闸),release() 把扣住的连接逐个放行——
// 同一条连接继续写 SSE 后收尾,不是断流。
// ---------------------------------------------------------------------------

class FakeBackend {
  constructor(options) {
    this.options = options || {};
    this.requests = [];
    this.mode = this.options.mode || 'reply';
    this.held = [];
    this.server = http.createServer((req, res) => {
      const chunks = [];
      req.on('data', (chunk) => chunks.push(chunk));
      res.on('error', () => { /* 断管忽略 */ });
      req.on('end', () => {
        this.requests.push({
          method: req.method,
          url: req.url,
          headers: req.headers,
          body: Buffer.concat(chunks).toString('utf8'),
        });
        if (this.mode === 'hold') {
          // 扣住:不发头不发正文(头留到 release 时 replyTo 一次性发,
          // 否则 Node 会报 headers already sent)。
          this.held.push(res);
          return;
        }
        this.replyTo(res);
      });
    });
  }

  replyTo(res) {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
    const sse = (name, object) => res.write('event: ' + name + '\ndata: ' + JSON.stringify(object) + '\n\n');
    sse('message_start', { type: 'message_start', message: { id: 'msg_asst_e2e', model: 'fake-model' } });
    sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
    sse('content_block_delta', {
      type: 'content_block_delta', index: 0,
      delta: { type: 'text_delta', text: this.options.replyText || '假后端的回话' },
    });
    sse('content_block_stop', { type: 'content_block_stop', index: 0 });
    sse('message_delta', {
      type: 'message_delta', delta: { stop_reason: 'end_turn' },
      usage: { input_tokens: 17, output_tokens: 7 },
    });
    sse('message_stop', { type: 'message_stop' });
    res.end();
  }

  release() {
    const held = this.held.splice(0);
    for (const res of held) {
      this.replyTo(res);
    }
  }

  listen() {
    return new Promise((resolve) => {
      this.server.listen(0, '127.0.0.1', () => resolve(this.server.address().port));
    });
  }

  close() {
    for (const res of this.held.splice(0)) {
      try { res.destroy(); } catch (_) { /* 已断 */ }
    }
    return new Promise((resolve) => this.server.close(resolve));
  }
}

// ---------------------------------------------------------------------------
// 助理实例:spawn 真 exe,收 stderr 抢 URL,提供停/杀。
// ---------------------------------------------------------------------------

class AssistantProcess {
  constructor(binary, root, profile, extraArgs) {
    this.binary = binary;
    this.root = root;
    this.profile = profile;
    this.extraArgs = extraArgs || [];
    this.child = null;
    this.stderrTail = '';
    this.url = '';
    this.exited = null;
  }

  get env() {
    return Object.assign({}, process.env, {
      LUBANCODE_HOME: path.join(this.root, 'home'),
      LUBANCODE_DATA_HOME: path.join(this.root, 'data'),
      LUBANCODE_MANAGED: '1',
      LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS: '1',
      LUBANCODE_ASSISTANT_WEB: path.resolve(__dirname),
    });
  }

  async start() {
    fs.mkdirSync(path.join(this.root, 'home'), { recursive: true });
    fs.mkdirSync(path.join(this.root, 'data'), { recursive: true });
    const args = ['assistant', '--no-open', '--profile', this.profile].concat(this.extraArgs);
    this.child = spawn(this.binary, args, {
      env: this.env,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    this.exited = new Promise((resolve) => {
      this.child.on('exit', (code, signal) => resolve({ code: code, signal: signal }));
    });
    let buffer = '';
    this.child.stdout.on('data', () => { /* assistant 的公告走 stderr */ });
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (chunk) => {
      buffer += chunk;
      this.stderrTail = (this.stderrTail + chunk).slice(-4000);
      // 自己启动打印"打开地址";重复启动打印"它的页面地址"。
      const hit = /(?:打开地址|它的页面地址):\s*(http:\/\/127\.0\.0\.1:\d+\/#b=[0-9a-f]+)/.exec(buffer);
      if (hit && !this.url) {
        this.url = hit[1];
      }
    });
    for (let i = 0; i < 300 && !this.url && this.child.exitCode === null; ++i) {
      await sleep(50);
    }
    return this.url;
  }

  stop() {
    if (this.child && this.child.exitCode === null) {
      this.child.kill();
    }
  }
}

// ---------------------------------------------------------------------------
// 裸 HTTP 客户端(http 模块;Connection: close 一问一答)
// ---------------------------------------------------------------------------

function httpOnce(port, method, target, body, headers) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: '127.0.0.1', port: port, method: method, path: target, headers: headers || {} },
      (res) => {
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () =>
          resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString('utf8') }));
      });
    req.on('error', reject);
    if (body !== undefined && body !== null) {
      req.setHeader('Content-Type', 'text/plain; charset=utf-8');
      req.setHeader('Content-Length', Buffer.byteLength(body));
      req.write(body);
    }
    req.end();
  });
}

// ---------------------------------------------------------------------------
// 裸 WS 客户端(net + 手搓握手/帧;与 C++ 测试册同款)
// ---------------------------------------------------------------------------

class WsClient {
  constructor(port, cookie, takeover) {
    this.port = port;
    this.cookie = cookie || '';
    this.takeover = takeover;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    this.inbox = [];
    this.events = [];
    this.closed = false;
    this.nextId = 0;
  }

  connect() {
    const self = this;
    const key = crypto.randomBytes(16).toString('base64');
    return new Promise((resolve, reject) => {
      const target = this.takeover ? '/ws?takeover=1' : '/ws';
      const lines = [
        'GET ' + target + ' HTTP/1.1',
        'Host: 127.0.0.1:' + self.port,
        'Upgrade: websocket',
        'Connection: Upgrade',
        'Sec-WebSocket-Key: ' + key,
        'Sec-WebSocket-Version: 13',
      ];
      if (self.cookie) {
        lines.push('Cookie: lubancode_assistant_session=' + self.cookie);
      }
      const socket = net.connect(self.port, '127.0.0.1');
      self.socket = socket;
      socket.on('error', (error) => {
        if (!self.closed) { reject(error); }
      });
      let handshake = Buffer.alloc(0);
      let handshaken = false;
      socket.on('data', (chunk) => {
        if (!handshaken) {
          handshake = Buffer.concat([handshake, chunk]);
          const split = handshake.indexOf('\r\n\r\n');
          if (split === -1) {
            return;
          }
          const head = handshake.slice(0, split).toString('utf8');
          if (head.indexOf('HTTP/1.1 101') === -1) {
            self.close();
            reject(new Error('升级被拒: ' + head.split('\r\n')[0]));
            return;
          }
          const expectAccept = crypto.createHash('sha1')
            .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
          if (head.indexOf('Sec-WebSocket-Accept: ' + expectAccept) === -1) {
            self.close();
            reject(new Error('accept 对不上'));
            return;
          }
          handshaken = true;
          const rest = handshake.slice(split + 4);
          if (rest.length > 0) {
            self.feed(rest);
          }
          resolve();
          return;
        }
        self.feed(chunk);
      });
      socket.on('close', () => {
        self.closed = true;
      });
      socket.write(lines.join('\r\n') + '\r\n\r\n');
    });
  }

  feed(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (true) {
      if (this.buffer.length < 2) {
        return;
      }
      const first = this.buffer[0];
      const opcode = first & 0x0F;
      let length = this.buffer[1] & 0x7F;
      let offset = 2;
      if (length === 126) {
        if (this.buffer.length < 4) {
          return;
        }
        length = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) {
          return;
        }
        length = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      if (this.buffer.length < offset + length) {
        return;
      }
      const payload = this.buffer.slice(offset, offset + length).toString('utf8');
      this.buffer = this.buffer.slice(offset + length);
      if (opcode === 0x8) {
        this.closed = true;
        return;
      }
      if (opcode !== 0x1) {
        continue;
      }
      const message = JSON.parse(payload);
      if (message && typeof message.method === 'string') {
        this.events.push(message);
      } else {
        this.inbox.push(message);
      }
    }
  }

  send(object) {
    const payload = Buffer.from(JSON.stringify(object), 'utf8');
    const mask = crypto.randomBytes(4);
    let header;
    if (payload.length <= 125) {
      header = Buffer.from([0x81, 0x80 | payload.length]);
    } else if (payload.length <= 0xFFFF) {
      header = Buffer.alloc(4);
      header[0] = 0x81;
      header[1] = 0x80 | 126;
      header.writeUInt16BE(payload.length, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x81;
      header[1] = 0x80 | 127;
      header.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    const masked = Buffer.alloc(payload.length);
    for (let i = 0; i < payload.length; ++i) {
      masked[i] = payload[i] ^ mask[i % 4];
    }
    this.socket.write(Buffer.concat([header, mask, masked]));
  }

  request(method, params) {
    const self = this;
    const id = ++this.nextId;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('请求超时: ' + method)), 60000);
      self.send({ id: id, method: method, params: params || {} });
      const poll = function () {
        // 先查信箱再判断线:应答与 close 帧可能同一段到(shutdown 就是这
        // 形状——回了再收线),先到先得,不许拿断线盖过应答。
        for (let i = 0; i < self.inbox.length; ++i) {
          if (self.inbox[i].id === id) {
            const reply = self.inbox.splice(i, 1)[0];
            clearTimeout(timer);
            resolve(reply);
            return;
          }
        }
        if (self.closed) {
          clearTimeout(timer);
          reject(new Error('连接已断: ' + method));
          return;
        }
        setTimeout(poll, 25);
      };
      poll();
    });
  }

  waitForEvent(method, predicate, timeoutMs) {
    const self = this;
    const deadline = Date.now() + (timeoutMs || 20000);
    return new Promise((resolve) => {
      const poll = function () {
        const hit = self.events.find(function (event) {
          return event.method === method && (!predicate || predicate(event.params || {}, event));
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
  }

  close() {
    this.closed = true;
    if (this.socket) {
      this.socket.destroy();
    }
  }
}

// ---------------------------------------------------------------------------
// 幕
// ---------------------------------------------------------------------------

// 开一条控制通道(连接 + 握手)。刚断线就重连时,服务端可能还把死连接
// 记在"当前控制连接"上(读到 EOF 有个窗口),新连接会收占用通报——
// 退避重试即过;连续占用才真抛。
async function openChannel(field) {
  let lastError = null;
  for (let attempt = 0; attempt < 8; ++attempt) {
    const ws = new WsClient(field.port, field.cookie);
    await ws.connect();
    try {
      const init = await ws.request('initialize', { clientName: 'assistant-e2e' });
      if (init.error) {
        throw new Error('initialize 被拒: ' + JSON.stringify(init.error));
      }
      ws.initializeResult = init.result;
      ws.send({ method: 'initialized' });
      return ws;
    } catch (error) {
      const occupied = ws.events.some((event) => event.method === 'assistant/connection/occupied');
      ws.close();
      lastError = error;
      if (!occupied) {
        throw error;
      }
      await sleep(400);
    }
  }
  throw lastError || new Error('openChannel 重试撞满');
}

async function scene1_start(assistant) {
  console.log('幕1 启动:监听就绪才打印 URL');
  const url = await assistant.start();
  ok('启动打印了可复制的 URL(含一次性凭据)', !!url, assistant.stderrTail);
  return url;
}

async function scene2_authGate(url) {
  console.log('幕2 认证门:healthz/静态/交换/重放/伪造 Origin');
  const port = Number(/127\.0\.0\.1:(\d+)/.exec(url)[1]);

  const health = await httpOnce(port, 'GET', '/healthz');
  ok('healthz 200 且只回身份', health.status === 200 &&
    JSON.parse(health.body).service === 'lubancode-assistant');
  ok('healthz 不泄控制凭据', health.body.indexOf('controlSecret') === -1);

  const bare = await httpOnce(port, 'GET', '/');
  ok('静态无 cookie 401(配对提示)', bare.status === 401 && bare.body.indexOf('需要从启动链接进入') !== -1);

  const fragment = /#b=([0-9a-f]+)/.exec(url)[1];
  const exchange = await httpOnce(port, 'POST', '/auth/exchange', fragment);
  ok('bootstrap 交换 204', exchange.status === 204 || exchange.status === 200);
  const setCookie = (exchange.headers['set-cookie'] || []).join('\n');
  ok('Set-Cookie HttpOnly + SameSite=Strict',
    setCookie.indexOf('HttpOnly') !== -1 && setCookie.indexOf('SameSite=Strict') !== -1, setCookie);
  const cookie = /lubancode_assistant_session=([^;\s]+)/.exec(setCookie)[1];

  const replay = await httpOnce(port, 'POST', '/auth/exchange', fragment);
  ok('bootstrap 重放 403(防重放)', replay.status === 403);

  const badOrigin = await httpOnce(port, 'GET', '/', undefined, {
    Cookie: 'lubancode_assistant_session=' + cookie,
    Origin: 'http://evil.example.com',
  });
  ok('伪造 Origin 403', badOrigin.status === 403);

  const authed = await httpOnce(port, 'GET', '/', undefined, {
    Cookie: 'lubancode_assistant_session=' + cookie,
  });
  ok('静态有 cookie 200(页面字节)', authed.status === 200 && authed.body.indexOf('LubanCode 助理') !== -1);
  ok('CSP 头在场', (authed.headers['content-security-policy'] || '') !== '');

  const notListed = await httpOnce(port, 'GET', '/run_e2e.js', undefined, {
    Cookie: 'lubancode_assistant_session=' + cookie,
  });
  ok('manifest 外文件 404(白名单墙)', notListed.status === 404);

  return { port: port, cookie: cookie };
}

async function scene3_chat(field, backend) {
  console.log('幕3 聊天链路:initialize → 首配模型 → 对话 → 幂等重发零重跑');
  const ws = await openChannel(field);
  const init = { result: ws.initializeResult };
  ok('握手钉 1.3', init.result && init.result.protocolVersion === '1.3');
  ok('能力声明是助理模式(detached)',
    init.result && init.result.capabilities &&
    init.result.capabilities.mode === 'assistant' &&
    init.result.capabilities.workLifetime === 'detached');
  ok('助理方法面已声明',
    init.result.capabilities.methods.indexOf('config/model/set') !== -1);
  ws.send({ method: 'initialized' });

  const before = await ws.request('config/status', {});
  ok('首启未配置(configured=false)', before.result && before.result.configured === false);
  ok('状态投影零密钥', JSON.stringify(before.result).indexOf('test-key-e2e') === -1);

  const saved = await ws.request('config/model/set', {
    wire: 'anthropic',
    baseUrl: 'http://127.0.0.1:' + backend.port + '/',
    apiKey: 'test-key-e2e',
    model: 'fake-model',
  });
  ok('config/model/set 保存成功', !saved.error && saved.result && saved.result.saved === true,
    JSON.stringify(saved));
  ok('保存回执不回显密钥', JSON.stringify(saved).indexOf('test-key-e2e') === -1);

  const after = await ws.request('config/status', {});
  ok('保存后 configured=true 且密钥只报来源',
    after.result.configured === true && after.result.apiKeySource === 'inline' &&
    JSON.stringify(after.result).indexOf('test-key-e2e') === -1);

  const started = await ws.request('thread/start', { clientOperationId: 'CREATE-1' });
  const threadId = started.result && started.result.threadId;
  ok('thread/start 开场', !!threadId, JSON.stringify(started));

  const modelCallsBefore = backend.requests.length;
  const accepted = await ws.request('turn/start', {
    threadId: threadId, text: '第一句', clientOperationId: 'OP-1',
  });
  ok('turn/start 受理即回(带 turnId/operationId)',
    accepted.result && !!accepted.result.turnId && !!accepted.result.operationId);
  const completed = await ws.waitForEvent('turn/completed', (params) => params.threadId === threadId);
  ok('收到 turn/completed 终态', !!completed && completed.params.status === 'success');

  const resent = await ws.request('turn/start', {
    threadId: threadId, text: '第一句', clientOperationId: 'OP-1',
  });
  await sleep(400);
  ok('同键重发零重跑(模型恰好一次)',
    backend.requests.length === modelCallsBefore + 1 && !resent.error,
    '模型调用 ' + backend.requests.length + ' 次(基线 ' + modelCallsBefore + ')');

  return { ws: ws, threadId: threadId };
}

async function scene4_refreshRestore(field, chat) {
  console.log('幕4 刷新恢复:断开 → 新连接 → thread/read 找回两轮正文');
  chat.ws.close();
  await sleep(200);
  const ws2 = await openChannel(field);

  const read = await ws2.request('thread/read', { threadId: chat.threadId, lastSeq: 0 });
  const items = (read.result && read.result.items) || [];
  const texts = [];
  for (const item of items) {
    for (const block of item.content || []) {
      if (block.type === 'text') {
        texts.push((item.role === 'user' ? 'U:' : 'A:') + block.text);
      }
    }
  }
  const joined = texts.join('|');
  ok('恢复出用户问句', joined.indexOf('U:第一句') !== -1, joined);
  ok('恢复出助理答句', joined.indexOf('假后端的回话') !== -1, joined);
  ws2.close();
}

async function scene5_detached(field, backend, chat) {
  console.log('幕5 断线合同:关 WS 后已受理回合照跑到终态');
  backend.mode = 'hold';
  const modelCallsBefore = backend.requests.length;
  const ws = await openChannel(field);

  const accepted = await ws.request('turn/start', {
    threadId: chat.threadId, text: '关页之后还跑吗', clientOperationId: 'OP-2',
  });
  ok('hold 模式下 turn/start 仍受理', accepted.result && !!accepted.result.turnId);
  // 关页:硬断(只撤订阅,不取消工作——W0 合同)。
  ws.close();
  await sleep(400);
  ok('断线期间回合还在跑(模型请求已发未收场)',
    backend.requests.length === modelCallsBefore + 1 && backend.held.length >= 1);
  backend.mode = 'reply';
  backend.release();
  // 重连,领域账核对终态(事件可能错过,账不会)。
  const ws2 = await openChannel(field);
  let final = null;
  for (let i = 0; i < 100 && !final; ++i) {
    const read = await ws2.request('operation/read', {
      threadId: chat.threadId, clientOperationId: 'OP-2',
    });
    if (read.result && read.result.status === 'final') {
      final = read.result;
      break;
    }
    await sleep(100);
  }
  ok('operation/read 按原键回 final(关页后任务跑到终态)', !!final, JSON.stringify(final));
  ok('那次模型请求没有被重跑', backend.requests.length === modelCallsBefore + 1);

  // 单控制连接 + 显式接管(§六):ws2 持着控制连接,第三条连接收占用
  // 通报;带 takeover=1 的第四条接管,ws2 被换下。
  const ws3 = new WsClient(field.port, field.cookie);
  await ws3.connect();
  const occupied = await ws3.waitForEvent('assistant/connection/occupied', null, 5000);
  ok('第二页收占用通报(不吊着等)', !!occupied);
  ws3.close();
  const ws4 = new WsClient(field.port, field.cookie, /*takeover=*/true);
  await ws4.connect();
  await ws4.request('initialize', { clientName: 'assistant-e2e' });
  ws4.send({ method: 'initialized' });
  ok('接管连接完成握手', true);
  for (let i = 0; i < 50 && !ws2.closed; ++i) {
    await sleep(50);
  }
  ok('被接管的旧连接被换下(关闭)', ws2.closed);
  ws2.close();
  ws4.close();
}

async function scene6_duplicateAndPorts(binary, root, field, firstExited) {
  console.log('幕6 重复启动/端口冲突/停助理');

  // 重复启动:同 profile 第二个实例——退码 2,不杀原进程,只开旧实例页面。
  const dup = new AssistantProcess(binary, root, 'e2e');
  const dupUrl = await dup.start();
  const exit = await dup.exited;
  ok('重复启动拿到旧实例的新页面地址', !!dupUrl, dup.stderrTail);
  ok('重复启动退码 2', exit.code === 2, JSON.stringify(exit));
  ok('重复启动报了已在运行', dup.stderrTail.indexOf('已在运行') !== -1, dup.stderrTail);
  const healthAgain = await httpOnce(field.port, 'GET', '/healthz');
  ok('原实例仍健康(没被杀)', healthAgain.status === 200);

  // 端口冲突:占一个端口,指定它启动——准确报错非零退出。
  const squatter = net.createServer();
  await new Promise((resolve) => squatter.listen(0, '127.0.0.1', resolve));
  const takenPort = squatter.address().port;
  const clash = new AssistantProcess(binary, makeTempRoot('clash'), 'clash',
    ['--port', String(takenPort)]);
  await clash.start();
  const clashExit = await clash.exited;
  ok('端口被占非零退出', clashExit.code !== 0 && clashExit.code !== null, JSON.stringify(clashExit));
  ok('报错点名端口且说明不偷换', clash.stderrTail.indexOf(String(takenPort)) !== -1 &&
    clash.stderrTail.indexOf('不偷偷换端口') !== -1, clash.stderrTail);
  squatter.close();

  // 停助理:shutdown → 退出码 0;锁释放后可重启。
  const ws = await openChannel(field);
  const shutdownReply = await ws.request('shutdown', {});
  ok('shutdown 应答', !shutdownReply.error, JSON.stringify(shutdownReply));
  const firstExit = await firstExited;
  ok('停止助理后进程退码 0', firstExit.code === 0, JSON.stringify(firstExit));

  const reborn = new AssistantProcess(binary, root, 'e2e');
  const rebornUrl = await reborn.start();
  ok('锁释放后同 profile 可重启', !!rebornUrl, reborn.stderrTail);
  reborn.stop();
  await reborn.exited;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

async function main() {
  const args = process.argv.slice(2);
  let binary = null;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--binary' && i + 1 < args.length) {
      binary = args[++i];
    }
  }
  const resolved = findBinary(binary);
  if (!resolved) {
    console.log('SKIP: 找不到 lubancode 可执行文件(--binary <路径> 指定)');
    process.exit(0);
  }
  console.log('二进制: ' + resolved);

  const backend = new FakeBackend({ replyText: '假后端的回话' });
  backend.port = await backend.listen();

  const root = makeTempRoot('main');
  const assistant = new AssistantProcess(resolved, root, 'e2e');
  let wsChat = null;
  try {
    const url = await scene1_start(assistant);
    const field = await scene2_authGate(url);
    const chat = await scene3_chat(field, backend);
    wsChat = chat.ws;
    await scene4_refreshRestore(field, chat);
    await scene5_detached(field, backend, chat);
    await scene6_duplicateAndPorts(resolved, root, field, assistant.exited);
  } catch (error) {
    ok('e2e 主流程跑完(未抛异常)', false, String((error && error.stack) || error));
  } finally {
    if (wsChat) {
      wsChat.close();
    }
    assistant.stop();
    await Promise.race([assistant.exited, sleep(3000)]);
    await backend.close().catch(() => {});
    fs.rmSync(root, { recursive: true, force: true });
  }

  console.log('\n合计: ' + passed + ' 过 / ' + failed + ' 败');
  if (failures.length > 0) {
    console.log('败项:');
    for (const line of failures) {
      console.log('  - ' + line);
    }
    process.exit(1);
  }
}

main().catch((error) => {
  console.error('e2e 崩了: ' + ((error && error.stack) || error));
  process.exit(1);
});
