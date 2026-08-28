// 示例 MCP server(stdio,按行 JSON-RPC)。stdout 只写协议帧,日志全走 stderr。
// navigate 记一笔账,screenshot 回一张占位说明——夹具,不真开浏览器。
"use strict";

const readline = require("readline");

const PROTOCOL_VERSION = "2024-11-05";
const TOOLS = [
  {
    name: "navigate",
    description:
      "记一次导航:收 url,回 generation 与记账行。夹具不真访问网络。",
    inputSchema: {
      type: "object",
      properties: { url: { type: "string", description: "目标地址" } },
      required: ["url"],
      additionalProperties: false,
    },
  },
  {
    name: "screenshot",
    description:
      "回一份截图占位说明与 generation。夹具不产真实图片。",
    inputSchema: {
      type: "object",
      properties: {
        generation: { type: "integer", description: "导航后拿到的 generation" },
      },
      additionalProperties: false,
    },
  },
];

let generation = 0;
const visits = [];

function textResult(text) {
  return { content: [{ type: "text", text }] };
}

function handle(request) {
  const { id, method, params } = request;
  const ok = (result) => ({ jsonrpc: "2.0", id, result });
  const err = (code, message) => ({ jsonrpc: "2.0", id, error: { code, message } });

  switch (method) {
    case "initialize":
      return ok({
        protocolVersion: PROTOCOL_VERSION,
        capabilities: { tools: {} },
        serverInfo: { name: "browser-fixture", version: "1.0.0" },
      });
    case "notifications/initialized":
      return null; // 通知不回帧
    case "tools/list":
      return ok({ tools: TOOLS });
    case "tools/call": {
      const name = params && params.name;
      const args = (params && params.arguments) || {};
      if (name === "navigate") {
        generation += 1;
        visits.push({ url: args.url, generation });
        return ok(textResult("navigated (fixture): " + args.url +
          " | generation=" + generation +
          " | visits=" + visits.length));
      }
      if (name === "screenshot") {
        if (typeof args.generation !== "number" || args.generation !== generation) {
          return err(-32000, "stale_ref: generation 不合,当前是 " + generation);
        }
        return ok(textResult(
          "screenshot (fixture): 800x600 占位图,generation=" + generation));
      }
      return err(-32602, "unknown tool: " + name);
    }
    default:
      return err(-32601, "method not found: " + method);
  }
}

const rl = readline.createInterface({ input: process.stdin });
rl.on("line", (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  let request;
  try {
    request = JSON.parse(trimmed);
  } catch (exc) {
    process.stderr.write("bad json line: " + exc + "\n");
    return;
  }
  let response;
  try {
    response = handle(request);
  } catch (exc) {
    response = { jsonrpc: "2.0", id: request.id, error: { code: -32603, message: String(exc) } };
  }
  if (response) process.stdout.write(JSON.stringify(response) + "\n");
});
rl.on("close", () => process.exit(0));
process.stderr.write("browser fixture server on stdio\n");
