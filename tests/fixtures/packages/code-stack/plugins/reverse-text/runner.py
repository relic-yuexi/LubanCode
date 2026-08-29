# reverse-text 的 process 插件脚本,协议 v1:stdin 恰好一份 JSON,stdout
# 恰好一份 JSON,日志只写 stderr。纯本地,不出网,不落盘。
import json
import sys

for stream in (sys.stdin, sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8")
    except Exception:
        pass

request = json.load(sys.stdin)
try:
    text = request["arguments"]["text"]
    response = {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": text[::-1]}],
    }
except Exception as exc:  # noqa: BLE001 - 夹具:失败也按协议回 execution_failed
    response = {
        "protocol": 1,
        "call_id": request.get("call_id", ""),
        "ok": False,
        "error": {"code": "execution_failed", "message": str(exc)},
    }
json.dump(response, sys.stdout, ensure_ascii=False)
