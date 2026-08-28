#!/usr/bin/env node
// LubanCode 专属 Browser MCP(常驻 stdio server,MCP 富结果单 P1)。
//
// 分层(单子:内嵌浏览器调试工作台 阶段 1,边界冻结见
// docs/reference/browser-runtime.md):
//
//   lib/config.js     配置层:参数、目录、视口、安全闸,唯一一份默认值
//   lib/session.js    session 层:BrowserSession 本体——Playwright 生命
//                     周期、页签账、ref、下载、崩溃、串行队列
//   lib/tools.js      tool adapter 层:12 件 MCP 工具的皮,不持状态
//   lib/transport.js  transport 层:stdio JSON-RPC 分帧
//   server.js         本文件:装配 + MCP 方法 + 取消账 + 进程生命周期
//
// 规矩(单子定案):
//   - stdout 只写 MCP JSON-RPC(一行一条);Playwright 与自家的日志只进 stderr。
//   - 启动时不开浏览器,第一次 browser 工具调用才 lazy launch。
//   - 一份浏览器状态一位主人:页签账只有 session 一份,工具调用排进
//     同一事件队列串行跑,两只 tools/call 不得同时抢一只 page。
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

const { parseArgs, buildConfig, rejectDailyProfileDir, log } = require('./lib/config');
const { BrowserSession, BrowserError, errorShape, describeError } = require('./lib/session');
const { createBrowserTools } = require('./lib/tools');
const { startStdioTransport, PROTOCOL_VERSION, SUPPORTED_VERSIONS } = require('./lib/transport');

// ---------------------------------------------------------------------------
// 装配:config -> session -> tools。测试宿主直调时也走同样的
// buildConfig + BrowserSession,两条路一个账。
// ---------------------------------------------------------------------------

const args = parseArgs(process.argv.slice(2));
const config = buildConfig(args);

const dailyClash = rejectDailyProfileDir(config.userDataDir);
if (dailyClash) {
  log('拒绝启动:user-data-dir 指向用户日常浏览器 profile(' + dailyClash + '),LubanCode 浏览器须用专属 profile。');
  process.stderr.flush ? process.stderr.flush() : null;
  process.exit(2);
}

const session = new BrowserSession(config);

// persistent 档提前拿锁(首个工具调用前就拒掉抢同目录的第二只进程)。
if (config.profileMode === 'persistent') {
  try {
    session.lockProfile();
  } catch (error) {
    const message = error instanceof BrowserError ? error.message : String(error.message || error);
    log(message);
    // 锁被占:以错误码退出,宿主按 MCP 起服失败处理。
    process.stderr.write(message + '\n');
    process.exit(3);
  }
}

const tools = createBrowserTools({ session });

// ---------------------------------------------------------------------------
// MCP 方法与取消账
// ---------------------------------------------------------------------------

const pendingCancellations = new Set();
// 在飞调用账(P1.6):id -> { cancelled }。取消通知命中在飞请求就置旗;
// 单发 Playwright 动作靠自身超时收口,轮询型动作(wait 的 url/固定等待)
// 见旗即停,不硬等满。
const activeCalls = new Map();

const transport = startStdioTransport(handleMessage);

async function handleInitialize(id, params) {
  const requested = params && typeof params.protocolVersion === 'string' ? params.protocolVersion : PROTOCOL_VERSION;
  const negotiated = SUPPORTED_VERSIONS.has(requested) ? requested : PROTOCOL_VERSION;
  transport.sendResult(id, {
    protocolVersion: negotiated,
    capabilities: { tools: {} },
    serverInfo: { name: 'lubancode-browser-mcp', version: '0.1.0' },
  });
}

async function handleToolsList(id) {
  transport.sendResult(id, { tools: tools.listTools() });
}

async function handleToolsCall(id, params) {
  const name = params && params.name;
  const input = (params && params.arguments) || {};
  const state = { cancelled: false };
  activeCalls.set(id, state);
  const token = { get cancelled() { return state.cancelled; } };
  try {
    const result = await tools.callTool(name, input, token);
    transport.sendResult(id, result);
  } catch (error) {
    if (pendingCancellations.delete(id)) {
      transport.sendResult(id, { content: [{ type: 'text', text: '已取消: ' + name + '(页面未判死,可继续操作)' }], isError: true, structuredContent: { code: 'browser.cancelled' } });
      return;
    }
    const { code, message } = errorShape(error);
    log('tool', name, 'failed:', code, message);
    transport.sendResult(id, { content: [{ type: 'text', text: message }], isError: true, structuredContent: { code } });
  } finally {
    activeCalls.delete(id);
  }
}

const methods = {
  initialize: handleInitialize,
  'tools/list': handleToolsList,
  'tools/call': handleToolsCall,
};

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
    transport.sendError(id, -32601, 'unknown method: ' + method);
    return;
  }
  Promise.resolve(handler(id, message.params || {})).catch((error) => {
    transport.sendError(id, -32603, describeError(error));
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
    session.releaseLockNow(); // shutdown 已释放过则幂等空转
    process.exit(code || 0);
  }
}

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('exit', () => {
  // 同步兜底:异步关不掉就留锁文件说明,下场会话靠 pid 探活接管。
  session.releaseLockNow();
});
process.stdin.on('end', () => shutdown(0));
process.stdin.on('close', () => shutdown(0));

log('browser mcp ready:', { engine: config.engine, headless: config.headless, profile: config.profileMode, user_data_dir: config.userDataDir, downloads_dir: config.downloadsDir, pid: process.pid });
