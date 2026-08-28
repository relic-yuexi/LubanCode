// Transport 层:stdio 上的 JSON-RPC 分帧。只管读行、解 JSON、交宿主
// 分发、写回——不认识浏览器,不保存会话状态,也不知道工具是什么。
//
// 规矩:stdout 只写 MCP JSON-RPC(一行一条);别的输出全走 stderr。
'use strict';

const PROTOCOL_VERSION = '2025-06-18';
const SUPPORTED_VERSIONS = new Set(['2025-06-18', '2025-03-26', '2024-11-05']);

// onMessage(message) 收到每条合法 JSON 消息(通知与请求都算)。
// 返回 { send, sendResult, sendError }。
function startStdioTransport(onMessage) {
  let buffer = '';

  function send(message) {
    process.stdout.write(JSON.stringify(message) + '\n');
  }

  function sendResult(id, result) {
    send({ jsonrpc: '2.0', id, result });
  }

  function sendError(id, code, message) {
    send({ jsonrpc: '2.0', id, error: { code, message } });
  }

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
      onMessage(message);
    }
  });

  return { send, sendResult, sendError };
}

module.exports = { startStdioTransport, PROTOCOL_VERSION, SUPPORTED_VERSIONS };
