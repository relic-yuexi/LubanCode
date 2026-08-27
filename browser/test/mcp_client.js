// 测试驱动的 MCP 客户端:起 browser/server.js 子进程,走 stdio JSON-RPC。
'use strict';

const { spawn } = require('child_process');
const path = require('path');

const SERVER = path.resolve(__dirname, '..', 'server.js');

class BrowserMcpClient {
  constructor(extraArgs) {
    this.extraArgs = extraArgs || [];
    this.nextId = 1;
    this.pending = new Map();
    this.buffer = '';
    this.child = null;
  }

  async start() {
    this.child = spawn(process.execPath, [SERVER, ...this.extraArgs], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.child.stdout.setEncoding('utf8');
    this.child.stdout.on('data', (chunk) => this.onData(chunk));
    this.stderr = '';
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (chunk) => {
      this.stderr += chunk;
    });
    this.exitPromise = new Promise((resolve) => this.child.on('exit', (code) => resolve(code)));
    await this.request('initialize', {
      protocolVersion: '2025-06-18',
      capabilities: {},
      clientInfo: { name: 'browser-mcp-tests', version: '0.1.0' },
    });
    this.notify('notifications/initialized', {});
  }

  onData(chunk) {
    this.buffer += chunk;
    let index;
    while ((index = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      if (!line) continue;
      let message;
      try {
        message = JSON.parse(line);
      } catch (_) {
        continue;
      }
      if (message.id !== undefined && this.pending.has(message.id)) {
        const { resolve } = this.pending.get(message.id);
        this.pending.delete(message.id);
        resolve(message);
      }
    }
  }

  request(method, params, timeoutMs = 90000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('MCP 请求超时: ' + method + '(' + timeoutMs + 'ms)'));
      }, timeoutMs);
      this.pending.set(id, {
        resolve: (message) => {
          clearTimeout(timer);
          resolve(message);
        },
      });
      this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params: params || {} }) + '\n');
    });
  }

  notify(method, params) {
    this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params: params || {} }) + '\n');
  }

  // 便捷口:tools/call,断言 isError=false 后回 result。
  async call(name, args) {
    const response = await this.request('tools/call', { name, arguments: args || {} });
    if (response.error) throw new Error('tools/call 撞 JSON-RPC 错: ' + JSON.stringify(response.error));
    return response.result;
  }

  // 发请求不等答:测试取消链路用(拿 id 发 notifications/cancelled)。
  requestAsync(method, params) {
    const id = this.nextId++;
    const promise = new Promise((resolve, reject) => {
      this.pending.set(id, { resolve });
      this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params: params || {} }) + '\n');
    });
    return { id, promise };
  }

  // 期待失败:回 { text, code }。
  async callExpectError(name, args) {
    const response = await this.request('tools/call', { name, arguments: args || {} });
    if (response.error) return { text: JSON.stringify(response.error), code: 'jsonrpc' };
    const result = response.result;
    if (!result.isError) throw new Error('期待失败,却成功了: ' + name);
    return { text: result.content.map((c) => c.text || '').join('\n'), code: (result.structuredContent || {}).code || '' };
  }

  stop() {
    if (this.child && this.child.exitCode === null) {
      this.child.stdin.end();
      try {
        this.child.kill();
      } catch (_) { /* 已退 */ }
    }
    return this.exitPromise;
  }
}

module.exports = { BrowserMcpClient };
