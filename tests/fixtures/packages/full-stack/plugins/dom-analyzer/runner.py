# dom-analyzer 的 process 插件脚本,协议 v1:stdin 恰好一份 JSON,stdout 恰好一份 JSON。
# 日志只写 stderr。纯本地统计,不出网,不落盘。
import json
import re
import sys

for stream in (sys.stdin, sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8")  # Windows 管道默认本地代码页
    except Exception:
        pass

TAG_RE = re.compile(r"<([a-zA-Z][a-zA-Z0-9-]*)")
LINK_RE = re.compile(r"<a\b[^>]*href=[\"']([^\"']*)[\"']", re.IGNORECASE)
FORM_RE = re.compile(r"<form\b[^>]*>", re.IGNORECASE)
IMG_RE = re.compile(r"<img\b[^>]*>", re.IGNORECASE)
HEADING_RE = re.compile(r"<(h[1-6])\b[^>]*>(.*?)</h[1-6]>", re.IGNORECASE | re.DOTALL)
TAG_ONLY_RE = re.compile(r"<[^>]+>")


def inspect(html, url=""):
    tags = TAG_RE.findall(html)
    links = LINK_RE.findall(html)
    forms = FORM_RE.findall(html)
    images = IMG_RE.findall(html)
    images_missing_alt = [t for t in images if not re.search(r"\balt=", t, re.IGNORECASE)]
    headings = ["%s %s" % (level.lower(), TAG_ONLY_RE.sub("", text).strip())
                for level, text in HEADING_RE.findall(html)]

    suspicious = [h for h in links if h in ("", "#") or h.startswith("javascript:")]
    suspicious += [h for h in links if "localhost" in h or re.match(r"^https?://\d+\.", h)]

    lines = []
    if url:
        lines.append("url: %s" % url)
    lines.append("elements: %d" % len(tags))
    lines.append("links: %d (suspicious %d)" % (len(links), len(suspicious)))
    for entry in suspicious:
        lines.append("  suspicious: %s" % entry)
    lines.append("forms: %d" % len(forms))
    lines.append("images: %d (missing alt %d)" % (len(images), len(images_missing_alt)))
    lines.append("headings: %d" % len(headings))
    for entry in headings:
        lines.append("  %s" % entry)
    return "\n".join(lines)


request = json.load(sys.stdin)
try:
    arguments = request["arguments"]
    summary = inspect(arguments["html"], arguments.get("url", ""))
    response = {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": summary}],
    }
except Exception as exc:  # noqa: BLE001 - 夹具:一切失败都按协议回 execution_failed
    response = {
        "protocol": 1,
        "call_id": request.get("call_id", ""),
        "ok": False,
        "error": {"code": "execution_failed", "message": str(exc)},
    }
json.dump(response, sys.stdout, ensure_ascii=False)
