#!/usr/bin/env python3
"""按 datasets.lock.json 取外部基准数据(现为 Microsoft Lost in Conversation)。

Q2 实验 B2 compact 备料装置之一。数据本体不进仓(§一合同):本脚本把归档
拉到 cache/<name>/ 下,进仓的只有锁文件、本脚本、README、smoke 清单。

用法(在仓库任意位置):
    python tests/eval/compaction_benefit/fetch_datasets.py            # 取数/校验/解包
    python tests/eval/compaction_benefit/fetch_datasets.py --check    # 只校验已有缓存
    python tests/eval/compaction_benefit/fetch_datasets.py --force    # 删标记重解包

行为:
    1. 读同目录 datasets.lock.json;
    2. 目标目录已有正确标记(revision+sha256 对上)且关键文件在 → 幂等跳过;
    3. cache/downloads/ 已有归档 → 校验 SHA-256,对不上即删重下;
    4. 下载(primary codeload 直连,失败走 fallback + 代理 LIC_FETCH_PROXY,
       默认 http://127.0.0.1:10808,空串禁用);
    5. 校验 SHA-256 与字节数,不符报错退出(不吞);
    6. 安全解包(拒绝对路径/..,剥掉顶层目录)到 cache/<name>/,写标记文件。

只依赖标准库。缺网时给出明确报错与建议。
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import urllib.error
import urllib.request
from pathlib import Path

# Windows GBK 控制台下中文报错会变乱码,统一按 UTF-8 出
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
LOCK_FILE = HERE / "datasets.lock.json"
CACHE_ROOT = HERE / "cache"
DOWNLOAD_DIR = CACHE_ROOT / "downloads"
MARKER_NAME = ".lubancode_fetch_ok"
DEFAULT_PROXY = "http://127.0.0.1:10808"
TIMEOUT_SECONDS = 60


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_lock() -> dict:
    if not LOCK_FILE.exists():
        sys.exit(f"[fetch] 找不到锁文件:{LOCK_FILE}")
    with open(LOCK_FILE, encoding="utf-8") as f:
        return json.load(f)


def marker_ok(dest: Path, entry: dict) -> bool:
    marker = dest / MARKER_NAME
    if not marker.exists():
        return False
    try:
        record = json.loads(marker.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return False
    if record.get("revision") != entry["revision"] or record.get("archive_sha256") != entry["archive_sha256"]:
        return False
    return all((dest / rel).exists() for rel in entry["verify_files_after_extract"])


def download(url: str, out: Path, proxy: str | None) -> None:
    if proxy:
        handler = urllib.request.ProxyHandler({"http": proxy, "https": proxy})
        opener = urllib.request.build_opener(handler)
    else:
        opener = urllib.request.build_opener()
    req = urllib.request.Request(url, headers={"User-Agent": "lubancode-eval-fetch/1.0"})
    with opener.open(req, timeout=TIMEOUT_SECONDS) as resp, open(out, "wb") as f:
        shutil.copyfileobj(resp, f)


def safe_extract(tar: tarfile.TarFile, dest: Path, strip: int = 1) -> None:
    dest_resolved = dest.resolve()
    members = []
    for member in tar.getmembers():
        parts = member.name.split("/")
        if strip:
            if len(parts) <= strip:
                continue  # 顶层目录条目本身
            member.name = "/".join(parts[strip:])
        # 拒绝逃逸:解出的绝对路径必须落在 dest 内
        target = (dest / member.name).resolve()
        if not str(target).startswith(str(dest_resolved)):
            raise RuntimeError(f"归档内路径逃逸,拒绝解包:{member.name}")
        members.append(member)
    tar.extractall(dest, members=members, filter="data")


def fetch_entry(entry: dict, check_only: bool, force: bool) -> None:
    name = entry["name"]
    dest = HERE / entry["dest"]
    expected_sha = entry["archive_sha256"]
    expected_bytes = entry["archive_bytes"]

    if force and dest.exists():
        shutil.rmtree(dest)
        print(f"[fetch] {name}:--force,已删旧目录 {dest}")

    if marker_ok(dest, entry):
        print(f"[fetch] {name}:缓存已是锁定 revision {entry['revision'][:12]},幂等跳过(--force 可强制重来)")
        return

    if check_only:
        print(f"[fetch] {name}:缓存缺失或不匹配,--check 模式不下载。")
        return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOAD_DIR / f"{name}.tar.gz"

    need_download = True
    if archive.exists():
        actual = sha256_of(archive)
        if actual == expected_sha:
            print(f"[fetch] {name}:本地归档哈希对上,免下载 {archive}")
            need_download = False
        else:
            print(f"[fetch] {name}:本地归档哈希不符({actual[:12]}…),删掉重下")
            archive.unlink()

    if need_download:
        proxy = DEFAULT_PROXY
        attempts = [(entry["urls"]["primary"], None), (entry["urls"]["fallback"], proxy)]
        last_err: Exception | None = None
        for url, use_proxy in attempts:
            try:
                print(f"[fetch] {name}:下载 {url}" + (f"(代理 {use_proxy})" if use_proxy else ""))
                download(url, archive, use_proxy)
                actual = sha256_of(archive)
                if actual != expected_sha:
                    raise RuntimeError(f"下载后哈希不符:期望 {expected_sha},实得 {actual}")
                need_download = False
                break
            except (urllib.error.URLError, TimeoutError, OSError, RuntimeError) as e:
                last_err = e
                print(f"[fetch] {name}:该路失败({e});换下一条")
                if archive.exists():
                    archive.unlink()
        if need_download:
            sys.exit(
                f"[fetch] {name}:下载失败。最后错误:{last_err}\n"
                f"  排查建议:1) 检查网络;2) 设 LIC_FETCH_PROXY 换代理(空串禁用代理);\n"
                f"  3) 手动下载任一锁内 URL 存为 {archive} 后重跑本脚本;\n"
                f"  4) 手册镜像:{entry['urls'].get('mirror', '(无)')}"
            )

    actual_sha = sha256_of(archive)
    actual_bytes = archive.stat().st_size
    if actual_sha != expected_sha or actual_bytes != expected_bytes:
        sys.exit(
            f"[fetch] {name}:归档校验不过。sha256 {'对' if actual_sha == expected_sha else '不对'},"
            f"字节数 {'对' if actual_bytes == expected_bytes else '不对'}"
            f"({actual_bytes} vs {expected_bytes})。删档重跑。"
        )
    print(f"[fetch] {name}:SHA-256 对上({actual_sha[:12]}…),{actual_bytes} 字节")

    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as tar:
        safe_extract(tar, dest)
    missing = [rel for rel in entry["verify_files_after_extract"] if not (dest / rel).exists()]
    if missing:
        sys.exit(f"[fetch] {name}:解包后关键文件缺失:{missing}")
    (dest / MARKER_NAME).write_text(
        json.dumps({"revision": entry["revision"], "archive_sha256": expected_sha, "fetched_at": entry.get("fetched_at", "")}, indent=2),
        encoding="utf-8",
    )
    print(f"[fetch] {name}:解包完成 → {dest}(标记 {MARKER_NAME} 已写)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="只校验已有缓存,缺了不下载")
    parser.add_argument("--force", action="store_true", help="删除目标目录后重新解包")
    args = parser.parse_args()

    global DEFAULT_PROXY
    if "LIC_FETCH_PROXY" in os.environ:
        DEFAULT_PROXY = os.environ["LIC_FETCH_PROXY"] or None

    lock = load_lock()
    for entry in lock["datasets"]:
        fetch_entry(entry, args.check, args.force)
    print("[fetch] 全部完成。")


if __name__ == "__main__":
    main()
