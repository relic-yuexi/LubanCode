"""A real Stop hook that requests exactly one continuation and records its input."""

import json
from pathlib import Path
import sys

payload = json.load(sys.stdin)
with Path(sys.argv[1]).open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(payload, ensure_ascii=False) + "\n")
if payload.get("stop_hook_active", False):
    print("{}")
else:
    print(json.dumps({"continue": False, "stopReason": "SCOPED_STOP_CONTINUE"}))
