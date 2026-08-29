#!/usr/bin/env node
// 官方 Browser Package 的可选 MCP 启动器(code-bearing 组件)。
//
// 边界(docs/reference/browser-runtime.md):浏览器 Runtime 住 LubanCode
// 核心分发的 browser/server.js,不进包。本脚本只做两件事:找到核心
// server.js;把 stdio 原样接管过去。自己不开浏览器、不存状态、不另起
// page registry——那都是 Runtime 的账。
//
// 找法(先后的规矩;都找不到就 stderr 说明白、退出码 1——不下载、
// 不内置副本,宁死不越界):
//   1. 环境变量 LUBANCODE_BROWSER_MCP 指到核心 browser/server.js
//      (给文件或它所在目录都行;装在任意位置都吃);
//   2. 从本脚本位置逐级往上找 <祖先>/browser/server.js——开发 checkout
//      里包住 <repo>/examples/packages/ 之下,几级上去就是仓库根。
//
// 透传:命令行参数原样转交(--engine/--headed/--viewport...),stdio 三路
// 直通,子进程退出码原样带出。stdout 只该有 MCP JSON-RPC(核心 server
// 自己的纪律),本脚本的报错全走 stderr。

'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const SERVER_FILE = 'server.js';

function isCoreServer(candidate) {
  try {
    return fs.statSync(candidate).isFile();
  } catch {
    return false;
  }
}

function fromEnvOverride() {
  const raw = process.env.LUBANCODE_BROWSER_MCP;
  if (!raw || !raw.trim()) return '';
  let target = raw.trim();
  try {
    if (fs.statSync(target).isDirectory()) target = path.join(target, SERVER_FILE);
  } catch {
    // 指到的不是目录:当它就是 server.js 的路径,交给 isCoreServer 判。
  }
  return isCoreServer(target) ? target : '';
}

function byWalkingUp() {
  // 从 mcp/browser-launcher/ 起往上:browser-launcher -> mcp -> <包根>
  // -> <层目录> -> ... -> <repo 根>,每级试 <级>/browser/server.js。
  let dir = __dirname;
  for (let i = 0; i < 12; ++i) {
    const candidate = path.join(dir, 'browser', SERVER_FILE);
    if (isCoreServer(candidate)) return candidate;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return '';
}

const core = fromEnvOverride() || byWalkingUp();
if (!core) {
  process.stderr.write(
    '[browser-launcher] 找不到 LubanCode 核心分发的 browser/' + SERVER_FILE + '。\n' +
    '  两条路任选其一:\n' +
    '  1. 设环境变量 LUBANCODE_BROWSER_MCP,指到核心 browser/server.js(或其目录);\n' +
    '  2. 把本包放在核心 checkout 的子目录里(如 <repo>/examples/packages/),\n' +
    '     启动器会逐级往上找。\n' +
    '  浏览器 Runtime 不随包分发,这是设计,不是缺件。\n'
  );
  process.exit(1);
}

const child = spawn(process.execPath, [core, ...process.argv.slice(2)], {
  stdio: ['inherit', 'inherit', 'inherit'],
});
child.on('error', (err) => {
  process.stderr.write('[browser-launcher] 起不来 ' + core + ': ' + (err && err.message) + '\n');
  process.exit(1);
});
child.on('exit', (code, signal) => {
  if (signal) process.kill(process.pid, signal);
  else process.exit(code === null ? 1 : code);
});
