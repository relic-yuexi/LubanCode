"""Real stdio MCP fixture: report the child's cwd and process identity."""

import json
import os
import sys


def reply(request_id, result):
    print(json.dumps({"jsonrpc": "2.0", "id": request_id, "result": result}), flush=True)


for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request.get("method")
    if method == "initialize":
        reply(request["id"], {
            "protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
            "serverInfo": {"name": "session-resources-fixture", "version": "1"},
        })
    elif method == "tools/list":
        reply(request["id"], {"tools": [
            {"name": name, "description": name,
             "inputSchema": {"type": "object", "properties": {}}}
            for name in ("where", "not_admitted")
        ]})
    elif method == "tools/call":
        name = request.get("params", {}).get("name")
        reply(request["id"], {"content": [{"type": "text", "text": json.dumps({
            "cwd": os.getcwd(), "pid": os.getpid(), "tool": name,
        })}], "isError": name != "where"})
    else:
        reply(request["id"], {})
