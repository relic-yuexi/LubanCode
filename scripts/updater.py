#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LubanCode 一键整包更新助手(GitHubRelease自动更新单 P1,§六布局+§七状态流)。

安装布局(§六):

    <install-root>/
      lubancode(.exe)            固定启动器(自举式:同一个二进制,见下)
      versions/<ver>-<sha8>/     不可变整包:真实 EXE 与全部官方资源
      current.json               当前与上次可用版本指向(同文件系统原子替换)
      install-state.json         安装来源、清单、事务索引(schema 2)
      staging/<transaction>/     未激活下载与解包
      backups/<transaction>/     旧安装与用户修改备份
      updates/<transaction>.json 事务日志
      updater/updater.py         本助手(受管树,与 install_plan.py 同目录)

状态流(§七):checking -> downloading -> verified -> staged ->
waiting-for-idle -> activating -> healthy -> committed;异常 failed /
needs-review / rolled-back,状态与原因持久保存在 updates/<txn>.json,重启可续。

要点(单子底线):
  - 版本比较不在这里:宿主(C++ CLI)用 src/config/update_checker.cpp 的
    语义版本比较选好目标,把 Release ID/asset ID/摘要钉给本助手;下载期间
    发布更新拼不成一套。
  - 技能保护复用 P0 决策表(install_plan.py 同一套判断),不另写。
  - 激活只是换 current.json 指针(写前持久化事务与回退指针,单次
    os.replace 不把多次移动称原子);旧版本目录原地不动,旧进程继续用旧资源。
  - Windows 不替换运行中 EXE:平铺交接把旧根 EXE 改名挪进备份(对运行中
    映像允许改名,运行中的进程不受影响),新版 EXE 落根位当固定启动器;
    改名失败就停 needs-review 等用户退出,绝不强杀。
  - 健康检查在隔离数据根(临时目录)跑无副作用探针(--version),不碰真实
    用户数据,不发渠道消息。
  - 清理只动"有安装记录的旧版本",跳过当前/上次可用/运行中版本、用户
    备份与未知旁文件;删版本目录前先把清单外文件抢救进 backups/。

退出码:0 成功;1 failed(旧版继续可用);2 needs-review(冲突/交接未完);
3 rolled-back(已恢复旧版)。

用法(C++ 侧调用形态;也可手工):
    updater.py status  --install-root R [--json]
    updater.py plan    --install-root R --version V --tag T --digest sha256:HEX \
                       [--asset-name A] [--asset-size N] [--archive PATH]
    updater.py update  --install-root R --repo O/R --version V --tag T \
                       --exe-version EV --release-id N --asset-id M \
                       --asset-name A --digest sha256:HEX [--asset-size N] \
                       [--archive PATH]
    updater.py rollback --install-root R
    updater.py gc      --install-root R [--dry-run]
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

# 同目录的 install_plan.py(包内 updater/ 树、仓库 scripts/ 都成立)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_plan  # noqa: E402  P0 所有权决策表,同一套判断不另写

IS_WINDOWS = os.name == "nt"
EXE_NAME = "lubancode.exe" if IS_WINDOWS else "lubancode"
RG_NAME = "rg.exe" if IS_WINDOWS else "rg"

SCHEMA_STATE = 2          # 版本化布局的 install-state.json
SCHEMA_CURRENT = 1        # current.json
STATE_NAME = "install-state.json"
CURRENT_NAME = "current.json"

# §七 状态流
TERMINAL_BAD = {"failed", "needs-review", "rolled-back"}
# 决策表里"未决不许动"的动作(§四/§七:冲突未决停 needs-review)
BLOCKING_ACTIONS = {"conflict-modified", "conflict-collision",
                    "conflict-reparse", "conflict-kind", "keep-modified-retired"}

# 下载与解包护栏(§七:超时、退避、大小上限)
DOWNLOAD_RETRIES = 3
DOWNLOAD_BACKOFF_BASE = 2.0
DOWNLOAD_TIMEOUT_SECS = 60
MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024      # 硬顶 4 GiB
MAX_UNPACK_FACTOR = 6                            # 解压膨胀上限 = 包大小 ×6
DISK_HEADROOM_BYTES = 256 * 1024 * 1024          # 预检余量
PROBE_TIMEOUT_SECS = 30


class UpdaterError(Exception):
    """failed:旧版继续可用,人话原因。"""
    exit_code = 1


class NeedsReviewError(UpdaterError):
    """needs-review:冲突/交接未完,等用户处理。"""
    exit_code = 2


class RolledBackError(UpdaterError):
    """rolled-back:已恢复旧版。"""
    exit_code = 3


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def dump_json(obj):
    # P0 惯例:纯 ASCII 落盘,PowerShell 5.1 读 BOM-less UTF-8 不花
    return json.dumps(obj, ensure_ascii=True, sort_keys=True, indent=2) + "\n"


def say(msg):
    print(msg)
    try:
        sys.stdout.flush()
    except Exception:
        pass


def fsync_dir(path):
    if IS_WINDOWS:
        return  # Windows 目录不可 fsync,原子性靠 os.replace
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write_json(path, obj):
    """同文件系统原子替换:同目录临时文件 + fsync + 单次 os.replace。"""
    path = os.path.abspath(path)
    tmp = os.path.join(os.path.dirname(path), ".%s.tmp-%d" %
                       (os.path.basename(path), os.getpid()))
    with open(tmp, "w", encoding="ascii", newline="\n") as f:
        f.write(dump_json(obj))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fsync_dir(os.path.dirname(path))


# ---------------------------------------------------------------------------
# 布局探测与指针
# ---------------------------------------------------------------------------

def layout_paths(root):
    root = os.path.abspath(root)
    return {
        "root": root,
        "versions": os.path.join(root, "versions"),
        "current": os.path.join(root, CURRENT_NAME),
        "state": os.path.join(root, STATE_NAME),
        "staging": os.path.join(root, "staging"),
        "backups": os.path.join(root, "backups"),
        "updates": os.path.join(root, "updates"),
        "exe": os.path.join(root, EXE_NAME),
        "updater": os.path.join(root, "updater"),
    }


def read_json_file(path):
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_current(root):
    """current.json -> {"current": <目录名>, "previous": <目录名|None>} 或 None。"""
    data = read_json_file(layout_paths(root)["current"])
    if not isinstance(data, dict) or data.get("schema") != SCHEMA_CURRENT:
        return None
    current = data.get("current")
    if not isinstance(current, str) or not current:
        return None
    previous = data.get("previous")
    return {"current": current,
            "previous": previous if isinstance(previous, str) and previous else None}


def detect_layout(root):
    """versioned(current.json 在)| flat(旧平铺安装)| empty(什么都没有)。"""
    paths = layout_paths(root)
    if os.path.isfile(paths["current"]):
        return "versioned"
    if os.path.isfile(paths["exe"]) or os.path.isfile(paths["state"]):
        return "flat"
    return "empty"


def version_dirname(version, digest_hex):
    return "%s-%s" % (version, digest_hex[:8])


def normalize_digest(digest):
    """'sha256:<hex>' 或裸 hex -> 小写 hex;别的算法/格式拒绝。"""
    if not isinstance(digest, str) or not digest:
        raise UpdaterError("缺少资产摘要(sha256);没有可信摘要不做自动安装")
    value = digest.strip().lower()
    if value.startswith("sha256:"):
        value = value[len("sha256:"):]
    elif ":" in value:
        raise UpdaterError("不认的摘要格式: %s(只认 sha256)" % digest)
    if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise UpdaterError("摘要不是 64 位十六进制: %s" % digest)
    return value


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# 安装根独占锁(§六:更新器对安装根持锁,防两个更新器串账)
# ---------------------------------------------------------------------------

def pid_alive(pid):
    if pid <= 0:
        return False
    try:
        if IS_WINDOWS:
            import ctypes
            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
            if not handle:
                return False
            try:
                exit_code = ctypes.c_ulong()
                if kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                    return exit_code.value == 259  # STILL_ACTIVE
                return True
            finally:
                kernel32.CloseHandle(handle)
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True   # 探不到权限按活着算,宁可拒一次不串账
    except OSError:
        return False


class InstallLock:
    """updates/.lock:O_EXCL 创建;持有者死了先挪走再抢(断线恢复)。"""

    def __init__(self, root):
        self.paths = layout_paths(root)
        self.path = os.path.join(self.paths["updates"], ".lock")

    def acquire(self):
        os.makedirs(self.paths["updates"], exist_ok=True)
        for _ in range(3):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
                with os.fdopen(fd, "w", encoding="ascii") as f:
                    f.write(dump_json({"pid": os.getpid(), "acquired_at_utc": utcnow()}))
                return
            except FileExistsError:
                holder = read_json_file(self.path)
                pid = holder.get("pid") if isinstance(holder, dict) else None
                if isinstance(pid, int) and pid == os.getpid():
                    raise UpdaterError("本进程已持有安装锁(不可重入)。")
                if isinstance(pid, int) and pid_alive(pid):
                    raise UpdaterError(
                        "另一个更新器正在本安装根上运行(pid %d);等它收尾或处理完再试。" % pid)
                stale = self.path + ".stale-%d" % int(time.time())
                try:
                    os.replace(self.path, stale)
                    say("[lock] 清掉陈锁(持有者 pid %s 已退出)" % pid)
                except OSError:
                    pass
        raise UpdaterError("安装锁竞争失败,稍后重试")

    def release(self):
        try:
            if os.path.isfile(self.path):
                holder = read_json_file(self.path)
                pid = holder.get("pid") if isinstance(holder, dict) else None
                if pid == os.getpid():
                    os.remove(self.path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 事务日志(§七:每步可重入,重启后能继续或退回)
# ---------------------------------------------------------------------------

def new_txn_id():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + \
        "-" + os.urandom(4).hex()


class Transaction:
    def __init__(self, root, txn_id):
        self.root = os.path.abspath(root)
        self.paths = layout_paths(root)
        self.id = txn_id
        self.path = os.path.join(self.paths["updates"], txn_id + ".json")
        self.data = {}

    @staticmethod
    def open_existing(path):
        data = read_json_file(path)
        if not isinstance(data, dict) or "id" not in data:
            return None
        txn = Transaction(os.path.dirname(os.path.dirname(path)), data["id"])
        txn.path = path
        txn.data = data
        return txn

    def create(self, target):
        os.makedirs(self.paths["updates"], exist_ok=True)
        self.data = {
            "schema": 1,
            "id": self.id,
            "kind": "update",
            "created_at_utc": utcnow(),
            "state": "checking",
            "target_version": target["version"],
            "target_dirname": target["dirname"],
            "target_digest": target["digest_hex"],
            "target": target,
        }
        self.flush()

    def flush(self):
        atomic_write_json(self.path, self.data)

    @property
    def state(self):
        return self.data.get("state", "")

    def transition(self, state, **fields):
        self.data["state"] = state
        self.data["state_at_utc"] = utcnow()
        self.data.update(fields)
        self.flush()

    def note(self, text):
        self.data.setdefault("notes", []).append("%s %s" % (utcnow(), text))
        self.flush()

    def stage_dir(self):
        return os.path.join(self.paths["staging"], self.id)

    def archive_path(self):
        return os.path.join(self.stage_dir(), "archive.bin")


def list_transactions(root):
    paths = layout_paths(root)
    txns = []
    if os.path.isdir(paths["updates"]):
        for name in sorted(os.listdir(paths["updates"])):
            if not name.endswith(".json"):
                continue
            txn = Transaction.open_existing(os.path.join(paths["updates"], name))
            if txn is not None:
                txns.append(txn)
    return txns


def cleanup_staging(root, txn_id):
    paths = layout_paths(root)
    stage = os.path.join(paths["staging"], txn_id)
    try:
        if os.path.isdir(stage):
            shutil.rmtree(stage)
    except OSError as exc:
        say("[cleanup] staging 清不掉(%s): %s" % (stage, exc))


def find_resumable(root, target):
    """同一目标(同摘要)、未完结或停在 needs-review 的事务可续(§七:冲突
    处理完重跑不重下);异目标的在途事务作废清场。failed/rolled-back/
    committed 是终态,重跑另开新账。"""
    for txn in list_transactions(root):
        if txn.data.get("kind") != "update":
            continue
        if txn.state in ("failed", "rolled-back", "committed"):
            continue
        if txn.data.get("target_digest") != target["digest_hex"]:
            txn.transition("failed", reason="superseded",
                           detail="目标版本已换,旧事务作废")
            cleanup_staging(root, txn.id)
            continue
        return txn
    return None


# ---------------------------------------------------------------------------
# 下载(§七:固定 Release/asset/摘要;超时退避;大小上限;断点续传)
# ---------------------------------------------------------------------------

def github_asset_url(repo, asset_id):
    return "https://api.github.com/repos/%s/releases/assets/%d" % (repo, int(asset_id))


def download_with_resume(url, dest, digest_hex, declared_size, headers=None):
    """流式下载 + 全文件摘要核对。断点续传:partial 文件住在 staging/<txn>/
    里,事务钉死了 asset id 与摘要(续的必然是同一资产);206 续传、200 重下,
    收尾一律整文件对摘要,对不上即失败删件。"""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    partial = dest + ".part"
    cap = declared_size if declared_size else MAX_ARCHIVE_BYTES

    last_error = None
    for attempt in range(1, DOWNLOAD_RETRIES + 1):
        try:
            offset = 0
            mode = "wb"
            if os.path.isfile(partial):
                offset = os.path.getsize(partial)
                if offset > cap:
                    offset = 0
                else:
                    mode = "ab"
            req_headers = {"User-Agent": "lubancode-updater"}
            if headers:
                req_headers.update(headers)
            if offset > 0:
                req_headers["Range"] = "bytes=%d-" % offset
            req = urllib.request.Request(url, headers=req_headers)
            with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT_SECS) as resp:
                if resp.status == 200:
                    offset = 0
                    mode = "wb"  # 服务端不认 Range,从头来
                elif resp.status != 206:
                    raise UpdaterError("下载返回 HTTP %d" % resp.status)
                written = offset
                next_mark = (offset // (16 << 20) + 1) * (16 << 20)
                with open(partial, mode) as out:
                    while True:
                        chunk = resp.read(1 << 18)
                        if not chunk:
                            break
                        written += len(chunk)
                        if written > cap:
                            raise UpdaterError(
                                "下载超过大小上限(%d 字节,声明的 %s)" %
                                (cap, declared_size or "硬顶"))
                        out.write(chunk)
                        if written >= next_mark:
                            say("[download] %d MiB" % (written >> 20))
                            next_mark += 16 << 20
            if declared_size and written != declared_size:
                raise UpdaterError("下载不完整:得了 %d 字节,声明 %d" %
                                   (written, declared_size))
            actual = sha256_file(partial)
            if actual != digest_hex:
                os.remove(partial)
                raise UpdaterError("摘要不符:期望 sha256:%s,实得 sha256:%s" %
                                   (digest_hex, actual))
            os.replace(partial, dest)
            return
        except UpdaterError:
            raise
        except (urllib.error.URLError, socket.timeout, OSError) as exc:
            last_error = exc
            if attempt < DOWNLOAD_RETRIES:
                wait = DOWNLOAD_BACKOFF_BASE ** attempt
                say("[download] 第 %d 次失败(%s),%.0f 秒后重试" % (attempt, exc, wait))
                time.sleep(wait)
    raise UpdaterError("下载失败(重试 %d 次): %s" % (DOWNLOAD_RETRIES, last_error))


# ---------------------------------------------------------------------------
# 解包(§七:路径与类型检查)
# ---------------------------------------------------------------------------

def screen_member_name(name):
    """归档成员名过 P0 同一套路径契约:拒绝绝对路径/反斜杠/../保留名等。
    '.' 与空段折叠,其余逐段拼回相对路径整体验。"""
    name = name.replace("\\", "/")
    if name.startswith("/"):
        return None
    if len(name) > 512:
        return None
    parts = []
    for seg in name.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            return None
        parts.append(seg)
    if not parts:
        return None
    rebuilt = "/".join(parts)
    if not install_plan.valid_relpath(rebuilt):
        return None
    return rebuilt


def make_executable(path):
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def unpack_archive(archive_path, dest_dir, declared_size):
    """zip / tar.gz 解包,成员逐一筛:路径穿越、符号链接、硬链接、设备、
    非常规文件一律拒绝整个包;解压总量设上限(防炸弹)。"""
    os.makedirs(dest_dir, exist_ok=True)
    cap = (declared_size or (64 << 20)) * MAX_UNPACK_FACTOR
    total = 0

    if zipfile.is_zipfile(archive_path):
        with zipfile.ZipFile(archive_path) as zf:
            for info in zf.infolist():
                rel = screen_member_name(info.filename)
                if rel is None:
                    raise UpdaterError("归档成员路径不合法: %s" % info.filename)
                mode = (info.external_attr >> 16) & 0xFFFF
                if stat.S_ISLNK(mode):
                    raise UpdaterError("归档含符号链接: %s" % info.filename)
                dest = os.path.join(dest_dir, *rel.split("/"))
                if info.is_dir() or stat.S_ISDIR(mode):
                    os.makedirs(dest, exist_ok=True)
                    continue
                total += info.file_size
                if total > cap:
                    raise UpdaterError("解压总量超过上限(%d 字节)" % cap)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with zf.open(info) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                if mode & 0o111:
                    make_executable(dest)
    elif tarfile.is_tarfile(archive_path):
        with tarfile.open(archive_path, "r:*") as tf:
            for member in tf:
                rel = screen_member_name(member.name)
                if rel is None:
                    raise UpdaterError("归档成员路径不合法: %s" % member.name)
                if member.issym() or member.islnk():
                    raise UpdaterError("归档含链接: %s" % member.name)
                if member.isdev() or member.isfifo():
                    raise UpdaterError("归档含设备/管道: %s" % member.name)
                dest = os.path.join(dest_dir, *rel.split("/"))
                if member.isdir():
                    os.makedirs(dest, exist_ok=True)
                    continue
                if not member.isfile():
                    raise UpdaterError("归档成员不是普通文件: %s" % member.name)
                total += member.size
                if total > cap:
                    raise UpdaterError("解压总量超过上限(%d 字节)" % cap)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                src = tf.extractfile(member)
                if src is None:
                    raise UpdaterError("归档成员读不出: %s" % member.name)
                with src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                if member.mode & 0o111:
                    make_executable(dest)
    else:
        raise UpdaterError("不是 zip 也不是 tar.gz: %s" % archive_path)
    return total


def probe_exe(exe_path, expected_version=None, timeout_secs=PROBE_TIMEOUT_SECS):
    """无副作用探针:隔离数据根(临时目录,不是版本目录、不是用户根)跑
    --version。expected_version 给了就精确对 'lubancode <版本>';没给只验
    跑得起来、印的是 lubancode 头。不碰真实用户数据(§七)。"""
    exe_path = os.path.abspath(exe_path)
    probe_home = tempfile.mkdtemp(prefix="lubancode-health-")
    env = dict(os.environ)
    env["LUBANCODE_HOME"] = probe_home
    env["LUBANCODE_DATA_HOME"] = os.path.join(probe_home, "data")
    env["LUBANCODE_LANG"] = "en"
    try:
        out = subprocess.run([exe_path, "--version"], capture_output=True, text=True,
                             timeout=timeout_secs, check=False, env=env,
                             encoding="utf-8", errors="replace")
    except (OSError, subprocess.SubprocessError) as exc:
        return False, "探针跑不起来(%s)" % exc
    finally:
        shutil.rmtree(probe_home, ignore_errors=True)
    stdout = (out.stdout or "").strip()
    first = stdout.splitlines()[0] if stdout else ""
    if out.returncode != 0 or not first.startswith("lubancode "):
        return False, "探针输出不合: rc=%d stdout=%r" % (out.returncode, first)
    if expected_version is not None and first != "lubancode %s" % expected_version:
        return False, "探针版本不合:期望 'lubancode %s',得 %r" % (expected_version, first)
    return True, first


def verify_package(pkg_dir, expected):
    """逐文件核对 manifest(路径/大小/SHA-256)、包内无清单外文件、EXE 在、
    平台资源(rg)在、EXE 版本对。任何一项不合即失败——坏包不激活。"""
    manifest_path = os.path.join(pkg_dir, install_plan.MANIFEST_NAME)
    if not os.path.isfile(manifest_path):
        raise UpdaterError("包里缺 manifest.json")
    manifest = install_plan.read_manifest_file(manifest_path, "包内清单")
    if manifest.get("version") != expected["version"]:
        raise UpdaterError("清单版本(%r)与目标版本(%s)不合" %
                           (manifest.get("version"), expected["version"]))
    problems = install_plan.validate_manifest(manifest, "包内清单")
    if problems:
        raise UpdaterError("包内清单不干净: %s" % "; ".join(problems))

    listed = install_plan.manifest_to_map(manifest)
    for key, entry in sorted(listed.items()):
        full = os.path.join(pkg_dir, *entry["path"].split("/"))
        if not os.path.isfile(full):
            raise UpdaterError("包内缺清单文件: %s" % entry["path"])
        size = os.path.getsize(full)
        if entry.get("size") is not None and size != entry["size"]:
            raise UpdaterError("大小不合: %s(清单 %s,实得 %d)" %
                               (entry["path"], entry["size"], size))
        actual = sha256_file(full)
        if entry.get("sha256") and actual != entry["sha256"]:
            raise UpdaterError("摘要不合: %s(期望 %s,实得 %s)" %
                               (entry["path"], entry["sha256"], actual))

    # 清单外文件(拼套检测):manifest.json 自己除外
    allowed = set(listed.keys()) | {install_plan.fold(install_plan.MANIFEST_NAME)}
    for dirpath, _dirnames, filenames in os.walk(pkg_dir):
        for name in filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), pkg_dir).replace(os.sep, "/")
            if install_plan.fold(rel) not in allowed:
                raise UpdaterError("包内有清单外文件: %s" % rel)

    exe = os.path.join(pkg_dir, EXE_NAME)
    if not os.path.isfile(exe):
        raise UpdaterError("包里缺 %s" % EXE_NAME)
    if not IS_WINDOWS:
        make_executable(exe)
        rg = os.path.join(pkg_dir, "libexec", RG_NAME)
        if os.path.isfile(rg):
            make_executable(rg)
    ok, detail = probe_exe(exe, expected["exe_version"])
    if not ok:
        raise UpdaterError("EXE 探针不过: %s" % detail)
    if not os.path.isfile(os.path.join(pkg_dir, "libexec", RG_NAME)):
        raise UpdaterError("包里缺 libexec/%s" % RG_NAME)
    return manifest


# ---------------------------------------------------------------------------
# 技能保护预检(§五/§七:激活前,复用 P0 决策表)
# ---------------------------------------------------------------------------

def current_version_dir(root):
    pointer = read_current(root)
    if pointer is None:
        return None
    candidate = os.path.join(layout_paths(root)["versions"], pointer["current"])
    return candidate if os.path.isdir(candidate) else None


def load_old_manifest(scan_dir, state):
    """所有权基线 = 上次可信官方清单。版本化布局:版本目录自带 manifest.json;
    平铺:install-state.json 里存的 manifest,再退到平铺 manifest.json。"""
    side = os.path.join(scan_dir, install_plan.MANIFEST_NAME)
    if os.path.isfile(side):
        return install_plan.read_manifest_file(side, "当前版本清单")
    if isinstance(state, dict):
        manifest = state.get("manifest")
        if manifest and not install_plan.validate_manifest(manifest, "install-state 清单"):
            return manifest
    return None


def ownership_precheck(root, new_manifest, report_prefix="[precheck]"):
    """激活前的技能保护预检(只读):拿 P0 决策表扫当前整包目录。
    返回 (阻断项, 报告行, 布局)。阻断项(conflict-* / keep-modified-retired
    /无基线相撞)-> needs-review;报告项(keep-unknown / missing-kept)只说不动。"""
    layout = detect_layout(root)
    state = read_json_file(layout_paths(root)["state"])
    if layout == "versioned":
        scan_dir = current_version_dir(root)
        if scan_dir is None:
            return [], ["当前指针指向的版本目录不存在,按全新激活处理"], layout
    elif layout == "flat":
        scan_dir = os.path.abspath(root)
    else:
        return [], ["安装根是空的,按全新安装处理"], layout

    maps = {tree: os.path.join(scan_dir, tree)
            for tree in install_plan.ROLE_TOP_DIRS}
    disk = install_plan.scan_disk(maps, scan_dir)
    old_manifest = load_old_manifest(scan_dir, state)
    new_map = install_plan.manifest_to_map(new_manifest)

    if old_manifest is None:
        # §五.4:无基线。纯新增(不相撞)可以走;相撞交用户处理。
        collisions = sorted(disk[k]["path"] for k in new_map if k in disk)
        if collisions:
            return [{"path": p, "action": "no-baseline-collision",
                     "reason": "旧安装没有清单且撞上新版路径"} for p in collisions[:50]], \
                ["旧安装没有所有权基线;相撞 %d 项(明细见事务日志)" % len(collisions)], \
                layout
        return [], ["旧安装没有基线,但与新包路径不相撞:按纯新增走,未知文件保留"], layout

    plan = install_plan.build_plan(new_map, install_plan.manifest_to_map(old_manifest), disk)
    blocking = []
    lines = []
    for e in plan:
        if e["action"] == "keep-unknown":
            lines.append("%s 保留(用户/未知文件): %s" % (report_prefix, e["path"]))
        elif e["action"] == "missing-kept":
            lines.append("%s 官方件曾被本地删除,新版随包恢复: %s" % (report_prefix, e["path"]))
        elif e["action"] in BLOCKING_ACTIONS:
            blocking.append(e)
            lines.append("%s 冲突(%s): %s —— %s" %
                         (report_prefix, e["action"], e["path"], e["reason"]))
    return blocking, lines, layout


# ---------------------------------------------------------------------------
# 进程协调(§六:Windows 不替换运行中 EXE;不强杀)
# ---------------------------------------------------------------------------

def exe_in_use(path):
    """Windows:对运行中的 EXE 以写打开会吃 sharing violation——当占用。
    探测不了也当占用(保守)。POSIX:unlink/改名不影响运行进程,恒不占。"""
    if not IS_WINDOWS:
        return False
    try:
        import ctypes
        GENERIC_WRITE = 0x40000000
        OPEN_EXISTING = 3
        handle = ctypes.windll.kernel32.CreateFileW(
            str(path), GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if handle in (0, -1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF):
            return True
        ctypes.windll.kernel32.CloseHandle(handle)
        return False
    except Exception:
        return True


def version_in_use_posix(version_path):
    """Linux:/proc/*/exe 指进这棵版本目录 = 运行中。别的平台认不出
    (macOS 无 /proc):返回 False,清理侧只靠 keep 集合兜底。"""
    if IS_WINDOWS or not os.path.isdir("/proc"):
        return False
    version_path = os.path.abspath(version_path)
    try:
        entries = os.listdir("/proc")
    except OSError:
        return False
    for name in entries:
        if not name.isdigit():
            continue
        try:
            target = os.readlink(os.path.join("/proc", name, "exe"))
        except OSError:
            continue
        if target.startswith(version_path + os.sep) or target == version_path:
            return True
    return False


def version_in_use(version_path):
    exe = os.path.join(version_path, EXE_NAME)
    if os.path.isfile(exe) and exe_in_use(exe):
        return True
    if not IS_WINDOWS and version_in_use_posix(version_path):
        return True
    return False


# ---------------------------------------------------------------------------
# 磁盘预检与清理(§七)
# ---------------------------------------------------------------------------

def disk_preflight(root, need_bytes):
    free = shutil.disk_usage(os.path.abspath(root)).free
    if free < need_bytes + DISK_HEADROOM_BYTES:
        raise UpdaterError(
            "磁盘空间不够:需要约 %d MiB(含余量),%s 只剩 %d MiB" %
            ((need_bytes + DISK_HEADROOM_BYTES) >> 20, root, free >> 20))


def estimate_need(asset_size):
    base = asset_size or (512 << 20)
    return base * 3  # 包 + 解包 + 备份余量


def gc_versions(root, dry_run=False):
    """至少保留一份已知可用整包;跳过当前/上次可用/运行中版本与用户备份。
    删版本目录前,清单外文件(用户塞进去的)先抢救进 backups/stranded-<名>/。"""
    paths = layout_paths(root)
    pointer = read_current(root)
    keep = set()
    if pointer:
        keep.add(pointer["current"])
        if pointer["previous"]:
            keep.add(pointer["previous"])
    pending = set()
    for t in list_transactions(root):
        if t.state not in TERMINAL_BAD and t.state != "committed":
            pending.add(t.data.get("target_dirname"))
    keep.discard(None)
    removed = []
    if not os.path.isdir(paths["versions"]):
        return removed
    for name in sorted(os.listdir(paths["versions"])):
        full = os.path.join(paths["versions"], name)
        if not os.path.isdir(full):
            continue
        if name in keep or name in pending:
            continue
        if version_in_use(full):
            say("[gc] %s 有进程在用,跳过" % name)
            continue
        manifest = load_old_manifest(full, None)
        listed = set(install_plan.fold(e["path"])
                     for e in manifest.get("files", [])) if manifest else set()
        stranded = 0
        for dirpath, _dirs, files in os.walk(full):
            for fname in files:
                rel = os.path.relpath(os.path.join(dirpath, fname), full).replace(os.sep, "/")
                if install_plan.fold(rel) not in listed:
                    stranded += 1
                    if dry_run:
                        continue
                    rescue = os.path.join(paths["backups"], "stranded-" + name, *rel.split("/"))
                    os.makedirs(os.path.dirname(rescue), exist_ok=True)
                    shutil.copy2(os.path.join(dirpath, fname), rescue)
        if dry_run:
            say("[gc] 预演:将移除旧版本 %s(清单外文件 %d 个会先进备份)" % (name, stranded))
            removed.append(name)
            continue
        shutil.rmtree(full)
        if stranded:
            say("[gc] 已移除旧版本 %s;清单外文件 %d 个保存在 backups/stranded-%s/" %
                (name, stranded, name))
        else:
            say("[gc] 已移除旧版本 %s" % name)
        removed.append(name)
    return removed


# ---------------------------------------------------------------------------
# 激活、健康检查、提交、回滚(§七 状态流主链)
# ---------------------------------------------------------------------------

def write_install_state(root, target, manifest, txn_id, channel):
    paths = layout_paths(root)
    state = {
        "schema": SCHEMA_STATE,
        "layout": "versioned",
        "installed_at_utc": utcnow(),
        "version": target["version"],
        "platform": target.get("platform"),
        "channel": channel,
        "source": {
            "repo": target.get("repo"),
            "release_id": target.get("release_id"),
            "release_tag": target.get("tag"),
            "asset_id": target.get("asset_id"),
            "asset_name": target.get("asset_name"),
            "asset_digest": "sha256:" + target["digest_hex"],
            "download_url": target.get("download_url"),
        },
        "installer": "updater.py",
        "transaction": txn_id,
        "manifest_provenance": "official-package",
        "manifest": manifest,
        "pending_conflicts": [],
    }
    atomic_write_json(paths["state"], state)


def sync_updater_tree(root, version_path):
    """把版本目录里的 updater 树按清单文件覆盖同步到根(启动器进程按 exe
    同目录找 updater/)。不清不删——本脚本自己就住在那棵树里。"""
    src_tree = os.path.join(version_path, "updater")
    if not os.path.isdir(src_tree):
        return
    dst_tree = layout_paths(root)["updater"]
    for dirpath, _dirs, files in os.walk(src_tree):
        for fname in files:
            rel = os.path.relpath(os.path.join(dirpath, fname), src_tree)
            dest = os.path.join(dst_tree, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, fname), dest)


def handover_flat_launcher(root, txn, target):
    """平铺 -> 版本化交接(§六:不替换运行中的 EXE,不强杀):
    旧根 EXE 改名挪进 backups/<txn>/legacy/(Windows 对运行中映像允许改名,
    运行中的进程不受影响);新版 EXE 落根位当固定启动器;updater 树同步到根。
    改名失败(被锁)停在 needs-review,等用户退出后重跑续上。"""
    paths = layout_paths(root)
    version_exe = os.path.join(paths["versions"], target["dirname"], EXE_NAME)
    if os.path.abspath(paths["exe"]) == os.path.abspath(version_exe):
        raise UpdaterError("安装根与版本目录重叠,拒绝交接")
    legacy_dir = os.path.join(paths["backups"], txn.id, "legacy")
    os.makedirs(legacy_dir, exist_ok=True)
    legacy_target = os.path.join(legacy_dir, EXE_NAME)
    if os.path.isfile(paths["exe"]):
        if os.path.exists(legacy_target):
            os.remove(legacy_target)
        try:
            os.rename(paths["exe"], legacy_target)
        except OSError as exc:
            raise NeedsReviewError(
                "旧根 EXE 挪不进备份(%s)——多半仍被运行中的进程/杀软锁着。"
                "退出所有 lubancode 进程后重跑 lubancode update 续上"
                "(已下载核对的包不重下)。绝不强杀。" % exc)
    shutil.copy2(version_exe, paths["exe"])
    if not IS_WINDOWS:
        make_executable(paths["exe"])
    sync_updater_tree(root, os.path.join(paths["versions"], target["dirname"]))


def restore_flat_legacy(root, txn):
    """从平铺交接的备份恢复旧根 EXE(回滚到平铺旧版用)。恢复不成也要摘掉
    current 指针——平铺旧 EXE 不认指针,留着会把后续启动当启动器空转。"""
    paths = layout_paths(root)
    legacy = os.path.join(paths["backups"], txn.id, "legacy", EXE_NAME)
    restored = False
    if os.path.isfile(legacy):
        if os.path.isfile(paths["exe"]):
            os.rename(paths["exe"],
                      os.path.join(paths["backups"], txn.id, "launcher-parked-" + EXE_NAME))
        shutil.copy2(legacy, paths["exe"])
        if not IS_WINDOWS:
            make_executable(paths["exe"])
        restored = True
    try:
        os.remove(paths["current"])
    except OSError:
        pass
    return restored


def activate(root, txn, target, manifest):
    paths = layout_paths(root)
    layout = detect_layout(root)
    version_path = os.path.join(paths["versions"], target["dirname"])
    old_pointer = read_current(root)

    txn.transition("waiting-for-idle",
                   rollback_current=old_pointer["current"] if old_pointer else None,
                   layout_before=layout)
    # 版本化布局:激活不碰旧版本目录、不碰运行中的 EXE,无需等待。
    # 平铺交接的等待逻辑在 handover_flat_launcher(改名失败停 needs-review)。

    txn.transition("activating")

    if not os.path.isdir(version_path):
        staged_pkg = os.path.join(txn.stage_dir(), "pkg")
        os.makedirs(paths["versions"], exist_ok=True)
        os.rename(staged_pkg, version_path)
        fsync_dir(paths["versions"])

    new_pointer = {
        "schema": SCHEMA_CURRENT,
        "current": target["dirname"],
        "previous": old_pointer["current"] if old_pointer else None,
        "updated_at_utc": utcnow(),
        "transaction": txn.id,
    }
    atomic_write_json(paths["current"], new_pointer)

    if layout == "flat":
        handover_flat_launcher(root, txn, target)
        say("[activate] 平铺安装已交接为固定入口布局;旧 EXE 保存在 %s" %
            os.path.join(paths["backups"], txn.id, "legacy"))
    say("[activate] current -> %s" % target["dirname"])


def rollback_after_activation_failure(root, txn, reason):
    """激活后失败:恢复旧指针,保留诊断(§七:失败恢复旧指针并保留诊断)。
    先试指回旧版本目录;平铺来的就恢复旧根 EXE 并摘掉指针。"""
    paths = layout_paths(root)
    detail = {"reason": reason, "rolled_back_at_utc": utcnow()}
    previous = txn.data.get("rollback_current")
    restored = False
    if previous:
        prev_dir = os.path.join(paths["versions"], previous)
        ok, probe_detail = probe_exe(os.path.join(prev_dir, EXE_NAME))
        if os.path.isdir(prev_dir) and ok:
            pointer = {
                "schema": SCHEMA_CURRENT,
                "current": previous,
                "previous": txn.data.get("target_dirname"),
                "updated_at_utc": utcnow(),
                "transaction": txn.id,
            }
            atomic_write_json(paths["current"], pointer)
            detail["restored_to"] = previous
            restored = True
        else:
            detail["previous_probe"] = probe_detail
    if not restored:
        detail["restored_flat"] = restore_flat_legacy(root, txn)
    txn.transition("rolled-back", **detail)
    cleanup_staging(root, txn.id)


def health_and_commit(root, txn, target, manifest, channel):
    paths = layout_paths(root)
    version_path = os.path.join(paths["versions"], target["dirname"])
    ok, detail = probe_exe(os.path.join(version_path, EXE_NAME), target["exe_version"])
    if not ok:
        rollback_after_activation_failure(root, txn, "健康检查不过: %s" % detail)
        raise RolledBackError("新版健康检查失败(%s),已恢复旧版;诊断保留在 %s" %
                              (detail, txn.path))
    txn.transition("healthy", health_probe=detail)

    write_install_state(root, target, manifest, txn.id, channel)
    txn.transition("committed", committed_at_utc=utcnow())
    cleanup_staging(root, txn.id)
    gc_versions(root)
    say("[commit] %s 已上线;事务 %s 提交。" % (target["version"], txn.id))


# ---------------------------------------------------------------------------
# 子命令:status / plan / update / rollback / gc
# ---------------------------------------------------------------------------

def cmd_status(args):
    root = os.path.abspath(args.install_root)
    paths = layout_paths(root)
    layout = detect_layout(root)
    pointer = read_current(root)
    state = read_json_file(paths["state"])
    txns = list_transactions(root)
    pending = [t for t in txns if t.state not in TERMINAL_BAD and t.state != "committed"]

    if args.json:
        out = {
            "layout": layout,
            "current": pointer["current"] if pointer else None,
            "previous": pointer["previous"] if pointer else None,
            "installed": {
                "version": state.get("version"),
                "channel": state.get("channel"),
                "source": state.get("source"),
            } if isinstance(state, dict) else None,
            "pending_transactions": [
                {"id": t.id, "state": t.state,
                 "target": t.data.get("target_version")} for t in pending],
        }
        print(dump_json(out))
        return 0
    say("安装根: %s" % root)
    say("布局: %s" % layout)
    if pointer:
        say("当前版本: %s" % pointer["current"])
        say("上次可用: %s" % (pointer["previous"] or "(无)"))
    else:
        say("尚无 current 指针(未做过一键更新)")
    if isinstance(state, dict):
        source = state.get("source") or {}
        say("安装来源: %s tag=%s asset=%s" %
            (source.get("repo"), source.get("release_tag"), source.get("asset_name")))
    for t in pending:
        say("在途事务: %s 状态=%s 目标=%s" % (t.id, t.state, t.data.get("target_version")))
    if not pending:
        say("没有在途事务。")
    return 0


def detect_platform():
    if IS_WINDOWS:
        return "windows-x64"
    if sys.platform.startswith("linux"):
        return "linux-x64"
    return "macos-arm64"


def collect_target(args):
    digest_hex = None
    if args.digest:
        digest_hex = normalize_digest(args.digest)
    elif args.archive:
        if not os.path.isfile(args.archive):
            raise UpdaterError("本地包不存在: %s" % args.archive)
        digest_hex = sha256_file(args.archive)
        say("[target] 本地包摘要 sha256:%s" % digest_hex)
    else:
        raise UpdaterError(
            "缺少资产摘要(sha256)。GitHub 资产未带 digest 时须从可信更新元数据取,"
            "否则不做自动安装;本地包用 --archive 会自算摘要。")
    version = args.version
    if not version:
        raise UpdaterError("缺少 --version")
    tag = args.tag or ("v" + version)
    return {
        "version": version,
        "tag": tag,
        "exe_version": args.exe_version or version,
        "platform": args.platform or detect_platform(),
        "dirname": version_dirname(version, digest_hex),
        "digest_hex": digest_hex,
        "repo": args.repo,
        "release_id": args.release_id,
        "asset_id": args.asset_id,
        "asset_name": args.asset_name,
        "asset_size": args.asset_size,
        "download_url": None,
    }


def cmd_plan(args):
    root = os.path.abspath(args.install_root)
    target = collect_target(args)
    layout = detect_layout(root)

    say("== 更新预演(不改安装、不动用户数据)==")
    say("目标版本: %s(tag %s,平台 %s)" % (target["version"], target["tag"], target["platform"]))
    say("资产: %s(%s 字节)摘要 sha256:%s" %
        (target["asset_name"] or "(未知)", target["asset_size"] or "未知", target["digest_hex"]))
    say("当前布局: %s" % layout)

    need = estimate_need(target["asset_size"])
    if os.path.isdir(root):
        free = shutil.disk_usage(root).free
        say("磁盘预检: 需约 %d MiB,余 %d MiB%s" %
            (need >> 20, free >> 20,
             "(偏紧)" if free < need + DISK_HEADROOM_BYTES else "(够)"))

    say("将创建版本目录: versions/%s(整包,不可变)" % target["dirname"])
    if layout == "flat":
        say("将执行平铺 -> 版本化迁移:受管树先完整备份;旧根 EXE 改名挪进"
            " backups/<txn>/legacy/,新版 EXE 落根位当固定启动器(不强杀、"
            "不替换运行中的映像)。")
    say("用户技能根 / 项目 .lubancode/.agents / 配置会话记忆凭据:一概不动。")

    new_manifest = {"schema": install_plan.SCHEMA, "files": [], "file_count": 0}
    if args.archive:
        # 本地包:解到系统临时目录读清单,给出逐文件预演(不动安装根)
        tmp_pkg = tempfile.mkdtemp(prefix="lubancode-plan-")
        try:
            unpack_archive(args.archive, tmp_pkg, target["asset_size"])
            new_manifest = install_plan.read_manifest_file(
                os.path.join(tmp_pkg, install_plan.MANIFEST_NAME), "本地包清单")
        finally:
            shutil.rmtree(tmp_pkg, ignore_errors=True)
        say("本地包核对: %d 个官方文件(解临时目录核对,不动安装)" %
            (new_manifest.get("file_count") or 0))
    else:
        say("下载: GitHub Release %s 的 asset %s(固定 asset id 与摘要;断点续传)"
            % (target["tag"], target["asset_id"] or target["asset_name"]))

    blocking, lines, _layout = ownership_precheck(root, new_manifest)
    for line in lines:
        say(line)
    if blocking:
        say("注意: 冲突/未决 %d 项(见上)。执行更新会停在 needs-review,"
            "官方新版不落这些路径,原件保留。" % len(blocking))
    else:
        say("技能保护预检: 没有阻断项。")
    say("激活: current.json 同文件系统原子换指针(写前持久化事务与回退指针),"
        "旧版本目录不动。")
    say("健康检查: 隔离数据根跑 %s --version;过了才提交。" % EXE_NAME)
    say("回滚: lubancode update --rollback 切回上次可用整包。")
    return 0


def copy_local_archive(src, txn):
    """本地官方包(GitHub 不通时的手动路径,§八):同一校验、同一事务流程。"""
    dest = txn.archive_path()
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    digest = sha256_file(src)
    target = txn.data.get("target", {})
    if target.get("digest_hex") and digest != target["digest_hex"]:
        raise UpdaterError("本地包摘要不符:期望 sha256:%s,实得 sha256:%s" %
                           (target["digest_hex"], digest))
    shutil.copy2(src, dest)
    txn.transition("verified", archive_sha256="sha256:" + digest,
                   source_archive=os.path.abspath(src))


def cmd_update(args):
    root = os.path.abspath(args.install_root)
    target = collect_target(args)
    paths = layout_paths(root)
    lock = InstallLock(root)
    lock.acquire()
    txn = None
    try:
        # 在途事务:同目标续跑,异目标作废(§七:每一步可重入)
        txn = find_resumable(root, target)
        if txn is None:
            txn = Transaction(root, new_txn_id())
            txn.create(target)
        else:
            say("[resume] 续上事务 %s(状态 %s)" % (txn.id, txn.state))
            txn.data["target"] = target
            txn.flush()

        version_path = os.path.join(paths["versions"], target["dirname"])
        if os.path.isdir(version_path):
            # 已有同摘要版本目录:整包重核对后直接进激活(重跑幂等,防目录被动手脚)
            manifest = verify_package(version_path, target)
            if txn.state not in ("staged", "waiting-for-idle", "activating",
                                 "healthy", "needs-review"):
                txn.transition("staged", note="复用已核对的版本目录,免下载")
            say("[stage] 版本目录 %s 已在且核对通过,跳过下载" % target["dirname"])
        else:
            if txn.state == "checking":
                disk_preflight(root, estimate_need(target["asset_size"]))
                txn.transition("downloading")

            archive = txn.archive_path()
            if not os.path.isfile(archive):
                if args.archive:
                    copy_local_archive(args.archive, txn)
                    archive = txn.archive_path()
                else:
                    if not target["asset_id"] or not target["repo"]:
                        raise UpdaterError(
                            "缺 --asset-id/--repo(联网下载必需);本地包走 --archive")
                    url = github_asset_url(target["repo"], target["asset_id"])
                    target["download_url"] = url
                    txn.data["target"] = target
                    txn.flush()
                    download_with_resume(
                        url, archive, target["digest_hex"], target["asset_size"],
                        headers={"Accept": "application/octet-stream"})
            if txn.state != "verified":
                txn.transition("verified", archive_sha256="sha256:" + target["digest_hex"])

            pkg_dir = os.path.join(txn.stage_dir(), "pkg")
            shutil.rmtree(pkg_dir, ignore_errors=True)
            unpack_archive(archive, pkg_dir, target["asset_size"])
            manifest = verify_package(pkg_dir, target)
            txn.transition("staged")

        # 激活前技能保护预检(§七):冲突未决停 needs-review,staging 保留待续
        blocking, lines, layout = ownership_precheck(root, manifest)
        for line in lines:
            say(line)
        if blocking:
            txn.transition("needs-review", blocking=[
                {"path": e["path"], "action": e["action"]} for e in blocking])
            raise NeedsReviewError(
                "技能保护预检有 %d 项冲突未决,已停在 needs-review(安装未变,旧版继续可用)。\n"
                "逐项处理(移走自改文件或确认采用官方新版)后重跑 lubancode update 续上。\n"
                "明细: %s" % (len(blocking), txn.path))

        if layout == "flat":
            # 平铺迁移先完整备份(§五.4:宁可多备份),挪进本事务名下
            maps = {tree: os.path.join(root, tree)
                    for tree in install_plan.ROLE_TOP_DIRS}
            if any(os.path.isdir(d) for d in maps.values()):
                backup_root = install_plan.full_backup(maps, root)
                txn_backup = os.path.join(paths["backups"], txn.id, "flat")
                os.makedirs(os.path.dirname(txn_backup), exist_ok=True)
                try:
                    os.rename(backup_root, txn_backup)
                    backup_root = txn_backup
                except OSError:
                    pass  # 挪不动就留原地,账上照记路径
                txn.note("平铺迁移完整备份: %s" % backup_root)
                say("[backup] 平铺安装完整备份: %s" % backup_root)

        activate(root, txn, target, manifest)
        health_and_commit(root, txn, target, manifest, channel=args.channel or "stable")
        return 0
    except (NeedsReviewError, RolledBackError):
        raise
    except UpdaterError:
        if txn is not None and txn.state not in TERMINAL_BAD and txn.state != "committed":
            swapped = read_current(root)
            if swapped and swapped.get("current") == target["dirname"] \
                    and txn.state in ("activating", "healthy"):
                rollback_after_activation_failure(
                    root, txn, reason="激活中途失败: %s" % sys.exc_info()[1])
                raise RolledBackError(
                    "激活中途失败,已恢复旧版;诊断: %s" % txn.path)
            txn.transition("failed", reason=str(sys.exc_info()[1]))
            cleanup_staging(root, txn.id)
        raise
    finally:
        lock.release()


def cmd_rollback(args):
    root = os.path.abspath(args.install_root)
    paths = layout_paths(root)
    lock = InstallLock(root)
    lock.acquire()
    try:
        pointer = read_current(root)
        if pointer is None:
            say("没有 current 指针,无可回滚(平铺安装请用包内安装脚本的备份)。")
            return 0
        previous = pointer.get("previous")
        if not previous:
            say("没有记录上次可用版本。最近一次事务的备份在: %s" % paths["backups"])
            return 1

        prev_dir = os.path.join(paths["versions"], previous)
        if not os.path.isdir(prev_dir):
            say("上次可用版本目录不在了: %s" % prev_dir)
            return 1
        # 数据兼容门禁(§七):先探针能跑,再换指针;不硬切
        ok, detail = probe_exe(os.path.join(prev_dir, EXE_NAME))
        if not ok:
            say("上次可用版本探针不过(%s),不换指针。新版继续生效。" % detail)
            return 1

        txn = Transaction(root, new_txn_id())
        txn.data = {
            "schema": 1, "id": txn.id, "kind": "rollback",
            "created_at_utc": utcnow(), "state": "activating",
            "from_version": pointer["current"], "to_version": previous,
        }
        txn.flush()
        new_pointer = {
            "schema": SCHEMA_CURRENT,
            "current": previous,
            "previous": pointer["current"],
            "updated_at_utc": utcnow(),
            "transaction": txn.id,
        }
        atomic_write_json(paths["current"], new_pointer)
        state = read_json_file(paths["state"])
        if isinstance(state, dict):
            state["rolled_back_at_utc"] = utcnow()
            state["version"] = previous.split("-", 1)[0]
            atomic_write_json(paths["state"], state)
        txn.transition("committed", committed_at_utc=utcnow())
        say("[rollback] current -> %s(回滚完成; %s 保留,随时可再切回)" %
            (previous, pointer["current"]))
        return 0
    finally:
        lock.release()


def cmd_gc(args):
    root = os.path.abspath(args.install_root)
    lock = InstallLock(root)
    if not args.dry_run:
        lock.acquire()
    try:
        removed = gc_versions(root, dry_run=args.dry_run)
        say("清理完成: 移除 %d 个旧版本" % len(removed))
        return 0
    finally:
        if not args.dry_run:
            lock.release()


# ---------------------------------------------------------------------------
# 参数与入口
# ---------------------------------------------------------------------------

def add_target_args(ap):
    ap.add_argument("--install-root", required=True, help="安装根(启动器所在目录)")
    ap.add_argument("--repo", help="官方仓库 owner/name(联网下载用)")
    ap.add_argument("--version", required=True, help="目标版本(major.minor.patch[-预发布])")
    ap.add_argument("--tag", help="Release tag;缺省 v<version>")
    ap.add_argument("--exe-version", help="EXE 探针期望版本;缺省同 --version"
                    "(预发布 tag 的 EXE 印正式版本号,由宿主算好递进来)")
    ap.add_argument("--release-id", type=int, help="钉死的 Release ID")
    ap.add_argument("--asset-id", type=int, help="钉死的 asset ID")
    ap.add_argument("--asset-name", help="资产名(记账/展示)")
    ap.add_argument("--asset-size", type=int, help="资产字节数(上限/预检)")
    ap.add_argument("--digest", help="sha256:<hex>;缺省时 --archive 自算")
    ap.add_argument("--platform", help="目标平台;缺省按本机")
    ap.add_argument("--archive", help="本地官方包路径(GitHub 不通时手动指定,"
                        "仍走同一校验/保护/事务)")
    ap.add_argument("--channel", default="stable", help="stable / prerelease")


def main():
    # 子进程/管道场景统一 UTF-8 输出(Windows 管道默认 ANSI 会花)
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser(description="LubanCode 一键整包更新助手"
                                     "(GitHubRelease自动更新单 P1)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_status = sub.add_parser("status", help="安装布局/指针/在途事务")
    p_status.add_argument("--install-root", required=True)
    p_status.add_argument("--json", action="store_true")
    p_status.set_defaults(func=cmd_status)

    p_plan = sub.add_parser("plan", help="预演:列下载/替换/保留/备份/冲突,不动安装")
    add_target_args(p_plan)
    p_plan.set_defaults(func=cmd_plan)

    p_update = sub.add_parser("update", help="下载、校验、暂存、激活、健康检查、提交")
    add_target_args(p_update)
    p_update.set_defaults(func=cmd_update)

    p_rb = sub.add_parser("rollback", help="切回上次可用整包(先探针,不硬切)")
    p_rb.add_argument("--install-root", required=True)
    p_rb.set_defaults(func=cmd_rollback)

    p_gc = sub.add_parser("gc", help="清理旧版本(跳过当前/上次可用/运行中)")
    p_gc.add_argument("--install-root", required=True)
    p_gc.add_argument("--dry-run", action="store_true")
    p_gc.set_defaults(func=cmd_gc)

    args = ap.parse_args()
    try:
        return args.func(args)
    except NeedsReviewError as exc:
        print("needs-review: %s" % exc, file=sys.stderr)
        return 2
    except RolledBackError as exc:
        print("rolled-back: %s" % exc, file=sys.stderr)
        return 3
    except UpdaterError as exc:
        print("failed: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
