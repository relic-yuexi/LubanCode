"""Preserve referenced CMake File API bytes; copying is not graph acceptance."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


def preserve(index_path: Path, destination: Path, github_sha: str) -> dict:
    index_path = Path(index_path)
    destination = Path(destination)
    if index_path.is_symlink() or destination.exists() or destination.is_symlink():
        raise ValueError("File API originals must be new and cannot be linked")
    reply = index_path.parent.resolve(strict=True)
    pending = [index_path.name]
    originals: dict[str, bytes] = {}
    while pending:
        name = pending.pop()
        if not isinstance(name, str) or not name or name in (".", "..") or \
                "/" in name or "\\" in name or ":" in name or Path(name).is_absolute():
            raise ValueError("File API original reference leaves the reply directory")
        if name in originals:
            continue
        path = reply / name
        if path.is_symlink() or not path.is_file() or path.resolve(strict=True).parent != reply:
            raise ValueError("File API original is missing, linked or outside the reply")
        raw = path.read_bytes()
        document = json.loads(raw)
        originals[name] = raw
        # Traverse query objects and every configuration, including target and
        # directory replies. A future jsonFile reference is retained as well.
        objects = [document]
        while objects:
            current = objects.pop()
            if isinstance(current, dict):
                if "jsonFile" in current:
                    pending.append(current["jsonFile"])
                objects.extend(current.values())
            elif isinstance(current, list):
                objects.extend(current)
    if "manifest.json" in originals:
        raise ValueError("File API original collides with the evidence manifest")
    destination.mkdir(parents=True, exist_ok=False)
    files = []
    for name, raw in sorted(originals.items()):
        (destination / name).write_bytes(raw)
        files.append({"name": name, "bytes": len(raw),
                      "sha256": hashlib.sha256(raw).hexdigest()})
    receipt = {"schemaVersion": 1, "githubSha": github_sha, "index": index_path.name,
               "directory": str(destination), "status": "copied", "acceptance": "not_evaluated",
               "files": files}
    (destination / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                                encoding="utf-8")
    return receipt
