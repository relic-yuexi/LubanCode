#!/usr/bin/env python3
"""按 committed manifest 拉取随包 ripgrep 二进制(SearchTool 内置 Ripgrep 后端迁移单 P0-2)。

只读 third_party/ripgrep/manifest.json:下载固定版本的 release 资产(不问
GitHub API 的 latest)、重算 SHA-256 与 manifest 对账、只抽取 manifest 指定
的那一枚成员(不"顺手解全包",无路径穿越面)、落进 <target>/libexec/、拷贝
MIT license、跑 rg --version 精确校验首行版本。任何一步不合即非零退出,
不留半套产物(先写 staging,全套过门后原子换入)。

用法:
  python scripts/fetch_ripgrep.py --target dist/lubancode [--platform windows-x64]
      [--proxy http://127.0.0.1:10808] [--cache build/rg_cache]

  --platform  缺省按当前机器挑(Windows->windows-x64,Darwin arm64->macos-arm64,
              其余->linux-x64);Release 流水线三平台各自明传。
  --cache     archive 下载缓存目录(按文件名复用,已存在且哈希对得上就不重下)。
  --proxy     下载代理;不传则先试直连,再退 HTTPS_PROXY/HTTP_PROXY 环境变量。
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST_PATH = os.path.join(REPO_ROOT, "third_party", "ripgrep", "manifest.json")


def detect_platform_key() -> str:
    if platform.system() == "Windows":
        return "windows-x64"
    if platform.system() == "Darwin":
        return "macos-arm64" if platform.machine() == "arm64" else "linux-x64"
    return "linux-x64"


def load_manifest() -> dict:
    import json

    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    for field in ("name", "version", "license", "upstream", "assets"):
        if field not in manifest:
            raise SystemExit(f"manifest.json 缺字段: {field}")
    for key, asset in manifest["assets"].items():
        for field in ("archive", "archive_format", "sha256", "member", "output"):
            if field not in asset or not asset[field]:
                raise SystemExit(f"manifest.json assets.{key} 缺字段: {field}")
        sha = asset["sha256"]
        if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            raise SystemExit(f"manifest.json assets.{key}.sha256 不是 64 位十六进制: {sha!r}")
    return manifest


def download(url: str, dest: str, proxy: str | None) -> None:
    handlers: list[urllib.request.ProxyHandler] = []
    if proxy:
        handlers.append(urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
    opener = urllib.request.build_opener(*handlers)
    print(f"下载 {url}")
    with opener.open(url, timeout=120) as resp, open(dest, "wb") as out:
        shutil.copyfileobj(resp, out)


def fetch_archive(url: str, cache_dir: str | None, archive: str, sha256: str, proxy: str | None) -> str:
    """下载(或复用缓存)archive 并重算哈希。返回 archive 本地路径。"""
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        cached = os.path.join(cache_dir, archive)
        if os.path.exists(cached) and sha256_file(cached) == sha256:
            print(f"缓存命中: {cached}")
            return cached
    tmp = tempfile.mktemp(suffix=".download")
    try:
        download(url, tmp, proxy)
        actual = sha256_file(tmp)
        if actual != sha256:
            raise SystemExit(
                f"SHA-256 不合,停:\n  manifest: {sha256}\n  实测:    {actual}\n上游资产与 manifest 不一致,不许入库/入包。"
            )
        dest = os.path.join(cache_dir, archive) if cache_dir else os.path.join(tempfile.gettempdir(), archive)
        os.replace(tmp, dest)
        return dest
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def deb_member_bytes(archive_path: str, member: str) -> bytes:
    """.deb 是 ar 归档:取 data.tar.*,再从 tar 里抽 member(剥 ./ 前缀对账)。"""
    with open(archive_path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit("不是合法 .deb(ar 魔数不合)")
    pos = 8
    data_name = None
    while pos + 60 <= len(data):
        header = data[pos : pos + 60]
        name = header[0:16].decode("ascii", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii", "replace").strip())
        except ValueError:
            raise SystemExit(f".deb 成员头坏: {name!r}")
        body = data[pos + 60 : pos + 60 + size]
        if name.startswith("data.tar"):
            data_name = name
            data_bytes = body
            break
        pos += 60 + size + (size % 2)
    if data_name is None:
        raise SystemExit(".deb 里找不到 data.tar.*")
    # tarfile 按 magic 自动解 xz/gz/zst(zst 需 Python 3.14;上游 .deb 目前是 xz)
    with tarfile.open(fileobj=io.BytesIO(data_bytes), mode="r:*") as tar:
        return extract_tar_member(tar, member, archive_path)


def extract_tar_member(tar: tarfile.TarFile, member: str, archive_path: str) -> bytes:
    for info in tar.getmembers():
        # 路径统一剥 ./ 前缀再比对,只抽 manifest 点名的那一枚。
        normalized = info.name[2:] if info.name.startswith("./") else info.name
        if normalized == member and info.isfile():
            fileobj = tar.extractfile(info)
            if fileobj is None:
                raise SystemExit(f"tar 成员读不出来: {info.name}")
            return fileobj.read()
    raise SystemExit(f"{archive_path} 里找不到 manifest 指定成员: {member}")


def extract_member(archive_path: str, asset: dict) -> bytes:
    fmt = asset["archive_format"]
    member = asset["member"]
    if fmt == "zip":
        with zipfile.ZipFile(archive_path) as zf:
            # 只按 manifest 精确路径取成员,不遍历解包,无穿越面。
            try:
                return zf.read(member)
            except KeyError:
                raise SystemExit(f"{archive_path} 里找不到 manifest 指定成员: {member}")
    if fmt in ("tar.gz", "tar.xz", "tar"):
        with tarfile.open(archive_path, mode="r:*") as tar:
            return extract_tar_member(tar, member, archive_path)
    if fmt == "deb":
        return deb_member_bytes(archive_path, member)
    raise SystemExit(f"不认得 archive_format: {fmt}")


def check_version(exe: str, manifest: dict) -> str:
    """跑 rg --version,首行第二枚记号必须精确等于 manifest 版本。"""
    check = manifest.get("version_check", {})
    argv = [exe] + check.get("argv", ["--version"])
    try:
        proc = subprocess.run(argv, capture_output=True, timeout=30, text=True, encoding="utf-8", errors="replace")
    except OSError as e:
        raise SystemExit(f"rg --version 起不来({exe}): {e}")
    if proc.returncode != 0:
        raise SystemExit(f"rg --version 退出码 {proc.returncode}: {proc.stderr.strip()}")
    first_line = proc.stdout.splitlines()[0].strip() if proc.stdout.splitlines() else ""
    tokens = first_line.split()
    want_program = check.get("first_line_program", "ripgrep")
    want_version = check.get("first_line_version", manifest["version"])
    if len(tokens) < 2 or tokens[0] != want_program or tokens[1] != want_version:
        raise SystemExit(
            f"版本不合:首行 {first_line!r},要求 {want_program} {want_version}(版本记号精确相等;"
            f"上游实际首形如 'ripgrep {want_version} (rev ...)',rev 段不钉)"
        )
    print(f"版本校验过: {first_line}")
    return first_line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", required=True, help="发行包根(lubancode/),rg 落 <target>/libexec/")
    parser.add_argument("--platform", default=None, help="manifest 资产键:windows-x64/macos-arm64/linux-x64")
    parser.add_argument("--proxy", default=None, help="下载代理,如 http://127.0.0.1:10808")
    parser.add_argument("--cache", default=None, help="archive 缓存目录(哈希对上就复用)")
    args = parser.parse_args()

    manifest = load_manifest()
    key = args.platform or detect_platform_key()
    if key not in manifest["assets"]:
        raise SystemExit(f"manifest 没有 {key} 资产;可用: {', '.join(manifest['assets'])}")
    asset = manifest["assets"][key]

    url = manifest["download_url_template"].format(release_tag=manifest["release_tag"], archive=asset["archive"])
    archive_path = fetch_archive(url, args.cache, asset["archive"], asset["sha256"], args.proxy)
    print(f"SHA-256 对上: {asset['sha256']}")

    payload = extract_member(archive_path, asset)
    print(f"抽出成员 {asset['member']}: {len(payload)} 字节")

    # 先落 staging,版本 smoke 过门后整体换入 <target>/libexec——不半套。
    # staging 放 target 同盘(os.replace 不跨卷),target 可能尚不存在,先建。
    os.makedirs(args.target, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=".rg_stage_", dir=args.target)
    try:
        libexec = os.path.join(staging, "libexec")
        os.makedirs(libexec)
        exe = os.path.join(libexec, asset["output"])
        with open(exe, "wb") as f:
            f.write(payload)
        os.chmod(exe, 0o755)  # Windows 无执行位,chmod 是空操作;POSIX/macOS 必须守

        check_version(exe, manifest)

        dest_libexec = os.path.join(args.target, "libexec")
        os.makedirs(dest_libexec, exist_ok=True)
        dest_exe = os.path.join(dest_libexec, asset["output"])
        os.replace(exe, dest_exe)
        os.chmod(dest_exe, 0o755)

        # MIT license 随包:licenses/ripgrep-MIT.txt + THIRD_PARTY_NOTICES.md。
        licenses_dir = os.path.join(args.target, "licenses")
        os.makedirs(licenses_dir, exist_ok=True)
        shutil.copyfile(
            os.path.join(REPO_ROOT, "third_party", "ripgrep", manifest.get("license_file", "LICENSE-MIT")),
            os.path.join(licenses_dir, "ripgrep-MIT.txt"),
        )
        notices_dest = os.path.join(args.target, "THIRD_PARTY_NOTICES.md")
        if os.path.exists(os.path.join(REPO_ROOT, "THIRD_PARTY_NOTICES.md")):
            shutil.copyfile(os.path.join(REPO_ROOT, "THIRD_PARTY_NOTICES.md"), notices_dest)

        print(f"就位: {dest_exe}")
        print(f"就位: {os.path.join(licenses_dir, 'ripgrep-MIT.txt')}")
        return 0
    finally:
        shutil.rmtree(staging, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
