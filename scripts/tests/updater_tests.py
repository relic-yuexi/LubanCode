#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""updater.py 行为用例(GitHubRelease自动更新单 P1)。

手工跑:python3 scripts/tests/updater_tests.py(或 bash scripts/tests/updater.tests.sh)
CI 里由 ci.yml 的 install-scripts 腿跑(windows + ubuntu)。

覆盖(§十验收的 CI 可测部分;真机进程占用/断电类如实留未勾):
  A. 纯单元:归档成员路径筛、摘要规范化、版本目录名
  B. 布局与指针:empty/flat/versioned 探测、current.json 读写、原子写不留临时件
  C. 安装锁:活 pid 拒、死 pid 抢陈锁
  D. 坏包各失败路一律保旧版(两平台):摘要不符/路径穿越/符号链接/清单不合/
     缺 manifest/清单外文件/磁盘预检不过——退出 1,平铺安装原样
  E. 端到端(POSIX):平铺 -> 版本化迁移成功;用户旁文件保留;旧 EXE 进备份;
     根位换启动器;幂等重跑免下载
  F. 冲突 -> needs-review 停住;解决后续跑同一事务,不重下(POSIX)
  G. 回滚:切回上次可用;无 previous 时明拒(POSIX)
  H. 清理:留当前/上次可用,旧版本清单外文件先进备份(POSIX)
  I. 断点续传:本地 HTTP(Range)续对同资产;错前缀续完摘要必红
  J. 目标已换:在途事务作废清场

EXE 探针依赖假 exe(shell 脚本),只在 POSIX 跑——Windows 腿覆盖 A-D/I/J。
"""

import hashlib
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPTS = os.path.join(REPO, "scripts")
sys.path.insert(0, SCRIPTS)

import generate_manifest  # noqa: E402
import updater            # noqa: E402

IS_WINDOWS = os.name == "nt"
POSIX_OK = not IS_WINDOWS
EXE_NAME = "lubancode.exe" if IS_WINDOWS else "lubancode"

PASS = 0
FAIL = 0


def ok(name):
    global PASS
    PASS += 1
    print("[PASS] %s" % name)


def bad(name, detail):
    global FAIL
    FAIL += 1
    print("[FAIL] %s" % name)
    print("       %s" % detail)


def check(name, cond, detail=""):
    if cond:
        ok(name)
    else:
        bad(name, detail or "条件不成立")


def check_eq(name, expect, actual):
    check(name, expect == actual, "期望: %r\n       实际: %r" % (expect, actual))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_updater(args, timeout=180):
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    proc = subprocess.run(
        [sys.executable, os.path.join(SCRIPTS, "updater.py")] + args,
        capture_output=True, text=True, timeout=timeout, check=False,
        cwd=REPO, env=env, encoding="utf-8", errors="replace")
    return proc.returncode, proc.stdout + proc.stderr


# ---------------------------------------------------------------------------
# 夹具:造假发行包与假安装
# ---------------------------------------------------------------------------

def fake_exe(version):
    return "#!/bin/sh\necho 'lubancode %s'\n" % version


def build_pkg_dir(base, version, platform="test-x64"):
    """按发行包结构造假包目录(EXE 是回版本号的脚本桩)。"""
    pkg = os.path.join(base, "pkg-%s" % version)
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(pkg)
    dirs = ["skills/lubancode-config/references", "docs/features", "web/assistant",
            "libexec", "licenses", "updater"]
    for d in dirs:
        os.makedirs(os.path.join(pkg, d), exist_ok=True)
    with open(os.path.join(pkg, EXE_NAME), "w", newline="\n") as f:
        f.write(fake_exe(version))
    os.chmod(os.path.join(pkg, EXE_NAME), 0o755)
    with open(os.path.join(pkg, "skills/lubancode-config/SKILL.md"), "w") as f:
        f.write("---\nname: lubancode-config\n---\nrouter skill %s\n" % version)
    with open(os.path.join(pkg, "skills/lubancode-config/references/document-map.md"), "w") as f:
        f.write("map %s\n" % version)
    with open(os.path.join(pkg, "docs/README.md"), "w") as f:
        f.write("docs %s\n" % version)
    with open(os.path.join(pkg, "web/assistant/index.html"), "w") as f:
        f.write("<html>%s</html>\n" % version)
    with open(os.path.join(pkg, "libexec", "rg.exe" if IS_WINDOWS else "rg"), "w", newline="\n") as f:
        f.write("#!/bin/sh\necho 'ripgrep stub'\n")
    rg_path = os.path.join(pkg, "libexec", "rg.exe" if IS_WINDOWS else "rg")
    os.chmod(rg_path, 0o755)
    with open(os.path.join(pkg, "licenses/ripgrep-MIT.txt"), "w") as f:
        f.write("MIT\n")
    with open(os.path.join(pkg, "LICENSE"), "w") as f:
        f.write("MIT\n")
    with open(os.path.join(pkg, "THIRD_PARTY_NOTICES.md"), "w") as f:
        f.write("notices %s\n" % version)
    for name in ("updater.py", "install_plan.py"):
        shutil.copy2(os.path.join(SCRIPTS, name), os.path.join(pkg, "updater", name))
    manifest = generate_manifest.build_manifest(pkg, version, platform, "stable")
    with open(os.path.join(pkg, "manifest.json"), "w", encoding="ascii", newline="\n") as f:
        f.write(generate_manifest.dump(manifest))
    return pkg


def pack_tar(pkg_dir, out_path):
    """按 release.yml 的形态打 tar.gz(保留执行位)。"""
    with tarfile.open(out_path, "w:gz") as tf:
        for dirpath, _dirs, files in os.walk(pkg_dir):
            for fname in sorted(files):
                full = os.path.join(dirpath, fname)
                rel = os.path.relpath(full, pkg_dir)
                info = tf.gettarinfo(full, arcname=rel)
                if os.access(full, os.X_OK):
                    info.mode = 0o755
                with open(full, "rb") as src:
                    tf.addfile(info, src)


def make_release(base, version):
    """造一份假发行:返回 (archive, digest)。"""
    pkg = build_pkg_dir(base, version)
    archive = os.path.join(base, "lubancode-v%s-test.tar.gz" % version)
    pack_tar(pkg, archive)
    return archive, sha256(archive)


def make_flat_install(base, version):
    """造一份旧平铺安装(解包即装,带 manifest.json)。"""
    root = os.path.join(base, "flat-%s" % version)
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    pkg = build_pkg_dir(base, version)
    for dirpath, dirs, files in os.walk(pkg):
        rel_dir = os.path.relpath(dirpath, pkg)
        if rel_dir != ".":
            os.makedirs(os.path.join(root, rel_dir), exist_ok=True)
        for fname in files:
            shutil.copy2(os.path.join(dirpath, fname),
                         os.path.join(root, rel_dir, fname))
    return root


def current_of(root):
    return updater.read_current(root)


def txn_files(root):
    d = os.path.join(root, "updates")
    return sorted(f for f in os.listdir(d) if f.endswith(".json")) if os.path.isdir(d) else []


def read_txn(root, name):
    with open(os.path.join(root, "updates", name), "r", encoding="utf-8-sig") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# A. 纯单元
# ---------------------------------------------------------------------------

def section_units():
    print("== A. 纯单元")
    bad_names = ["../evil.txt", "/abs.txt", "C:/x", "a:b",
                 "skills/../../evil", "docs/CON", "skills/a.md."]
    for name in bad_names:
        check("成员路径拒绝: %s" % name,
              updater.screen_member_name(name) is None)
    check_eq("成员路径折叠反斜杠", "a/b.txt", updater.screen_member_name("a\\b.txt"))
    check_eq("成员路径折叠 ./", "skills/a.md", updater.screen_member_name("./skills/a.md"))
    check_eq("成员路径双斜杠", "skills/a.md", updater.screen_member_name("skills//a.md"))
    check_eq("成员路径正名", "skills/a/b.md", updater.screen_member_name("skills/a/b.md"))

    check_eq("摘要规范化", "a" * 64, updater.normalize_digest("sha256:" + "A" * 64))
    try:
        updater.normalize_digest("md5:" + "a" * 32)
        bad("非 sha256 拒绝", "竟然过了")
    except updater.UpdaterError:
        ok("非 sha256 拒绝")
    try:
        updater.normalize_digest("short")
        bad("坏摘要拒绝", "竟然过了")
    except updater.UpdaterError:
        ok("坏摘要拒绝")

    check_eq("版本目录名", "1.2.3-deadbeef",
             updater.version_dirname("1.2.3", "deadbeef" * 8))
    check("dump_json 纯 ASCII", all(ord(c) < 128 for c in updater.dump_json({"k": "中文"})))


# ---------------------------------------------------------------------------
# B. 布局与指针
# ---------------------------------------------------------------------------

def section_layout():
    print("== B. 布局与指针")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = os.path.join(base, "root")
        os.makedirs(root)
        check_eq("空根布局", "empty", updater.detect_layout(root))
        with open(os.path.join(root, EXE_NAME), "w") as f:
            f.write(fake_exe("1.0.0"))
        check_eq("平铺布局", "flat", updater.detect_layout(root))
        check("坏指针当无", updater.read_current(root) is None)
        with open(os.path.join(root, "current.json"), "w") as f:
            f.write("{ not json")
        check_eq("坏 JSON 当无指针", "versioned", updater.detect_layout(root))
        check("坏 JSON 读不出指针", updater.read_current(root) is None)

        os.remove(os.path.join(root, "current.json"))
        updater.atomic_write_json(os.path.join(root, "current.json"),
                                  {"schema": 1, "current": "v1-abcdef12", "previous": None})
        pointer = updater.read_current(root)
        check_eq("指针读写回路", "v1-abcdef12", pointer["current"] if pointer else None)
        leftovers = [f for f in os.listdir(root) if f.startswith(".current.json.tmp")]
        check("原子写不留临时件", not leftovers, str(leftovers))
        updater.atomic_write_json(os.path.join(root, "current.json"),
                                  {"schema": 1, "current": "v2-12345678", "previous": "v1-abcdef12"})
        pointer = updater.read_current(root)
        check_eq("指针原子换向", "v2-12345678", pointer["current"] if pointer else None)
        check_eq("指针记回退", "v1-abcdef12", pointer["previous"] if pointer else None)
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# C. 安装锁
# ---------------------------------------------------------------------------

def section_lock():
    print("== C. 安装锁")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = os.path.join(base, "root")
        os.makedirs(os.path.join(root, "updates"))
        lock = updater.InstallLock(root)
        lock.acquire()
        try:
            updater.InstallLock(root).acquire()
            bad("活锁持有时第二把拒", "竟然拿到了")
        except updater.UpdaterError:
            ok("活锁持有时第二把拒")
        lock.release()

        # 死 pid 的陈锁:会被挪走重抢
        with open(os.path.join(root, "updates", ".lock"), "w") as f:
            json.dump({"pid": 99999999}, f)
        lock2 = updater.InstallLock(root)
        try:
            lock2.acquire()
            ok("死 pid 陈锁可抢")
            lock2.release()
        except updater.UpdaterError as exc:
            bad("死 pid 陈锁可抢", str(exc))
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# D. 坏包各失败路(保旧版;两平台都跑,全部在 EXE 探针之前失败)
# ---------------------------------------------------------------------------

def section_failure_paths():
    print("== D. 坏包失败路保旧版")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        exe_before = open(os.path.join(root, EXE_NAME), "rb").read()
        os.makedirs(os.path.join(root, "skills", "user-extra"), exist_ok=True)
        with open(os.path.join(root, "skills", "user-extra", "NOTE.md"), "w") as f:
            f.write("用户旁文件\n")
        archive, digest = make_release(base, "2.0.0")

        def assert_old_intact(name):
            check("%s: 旧 EXE 字节不动" % name,
                  open(os.path.join(root, EXE_NAME), "rb").read() == exe_before)
            check("%s: 无 current.json" % name,
                  not os.path.exists(os.path.join(root, "current.json")))
            check("%s: 用户旁文件还在" % name,
                  os.path.isfile(os.path.join(root, "skills", "user-extra", "NOTE.md")))
            check("%s: 不留版本目录" % name,
                  not os.path.exists(os.path.join(root, "versions")))

        # d1 摘要不符:同包改一字节
        tampered = os.path.join(base, "tampered.tar.gz")
        with open(archive, "rb") as src, open(tampered, "wb") as out:
            data = bytearray(src.read())
            data[len(data) // 2] ^= 0xFF
            out.write(bytes(data))
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", tampered, "--digest", "sha256:" + digest])
        check_eq("摘要不符退 1", 1, rc)
        check("摘要不符报摘要", "摘要不符" in out or "digest" in out.lower(), out[-300:])
        assert_old_intact("摘要不符")

        # d2 路径穿越
        evil = os.path.join(base, "evil.tar.gz")
        with tarfile.open(evil, "w:gz") as tf:
            info = tarfile.TarInfo("../evil-escape.txt")
            payload = b"escaped\n"
            info.size = len(payload)
            tf.addfile(info, io.BytesIO(payload))
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", evil])
        check_eq("路径穿越退 1", 1, rc)
        check("路径穿越报成员", "成员路径不合法" in out, out[-300:])
        check("穿越没落盘", not os.path.exists(os.path.join(base, "evil-escape.txt")))
        assert_old_intact("路径穿越")

        # d3 符号链接成员
        linky = os.path.join(base, "linky.tar.gz")
        with tarfile.open(linky, "w:gz") as tf:
            info = tarfile.TarInfo("skills/link-to-etc")
            info.type = tarfile.SYMTYPE
            info.linkname = "/etc"
            tf.addfile(info)
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", linky])
        check_eq("符号链接退 1", 1, rc)
        check("符号链接报链接", "含链接" in out, out[-300:])
        assert_old_intact("符号链接")

        # d4 清单不合:包目录里偷改一个已记账文件再打包
        pkg2 = build_pkg_dir(base, "2.0.0")
        with open(os.path.join(pkg2, "docs/README.md"), "a") as f:
            f.write("偷偷改过\n")
        tampered2 = os.path.join(base, "tampered2.tar.gz")
        pack_tar(pkg2, tampered2)
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", tampered2])
        check_eq("清单不合退 1", 1, rc)
        check("清单不合报摘要/清单", ("摘要不合" in out) or ("清单" in out), out[-300:])
        assert_old_intact("清单不合")

        # d5 缺 manifest
        pkg3 = build_pkg_dir(base, "2.0.0")
        os.remove(os.path.join(pkg3, "manifest.json"))
        nomanifest = os.path.join(base, "nomanifest.tar.gz")
        pack_tar(pkg3, nomanifest)
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", nomanifest])
        check_eq("缺清单退 1", 1, rc)
        assert_old_intact("缺清单")

        # d6 清单外文件
        pkg4 = build_pkg_dir(base, "2.0.0")
        with open(os.path.join(pkg4, "smuggled.txt"), "w") as f:
            f.write("不在清单里\n")
        smuggle = os.path.join(base, "smuggle.tar.gz")
        pack_tar(pkg4, smuggle)
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", smuggle])
        check_eq("清单外文件退 1", 1, rc)
        check("清单外文件报清单外", "清单外" in out, out[-300:])
        assert_old_intact("清单外文件")

        # d7 磁盘预检不过(声明超大资产)
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", archive, "--asset-size", "999999999999"])
        check_eq("磁盘预检退 1", 1, rc)
        check("磁盘预检报空间", "磁盘" in out, out[-300:])
        assert_old_intact("磁盘预检")

        # 各失败路的 staging 收干净
        staging = os.path.join(root, "staging")
        if os.path.isdir(staging):
            leftovers = os.listdir(staging)
            check("失败路 staging 清空", not leftovers, str(leftovers))
        else:
            ok("失败路 staging 清空")
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# E. 端到端:平铺 -> 版本化迁移(POSIX;假 EXE 是 shell 脚本)
# ---------------------------------------------------------------------------

def section_e2e():
    if not POSIX_OK:
        print("== E. 端到端(POSIX only,跳过)")
        return
    print("== E. 端到端:平铺迁移 + 幂等重跑")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        exe_v1 = open(os.path.join(root, EXE_NAME), "rb").read()
        os.makedirs(os.path.join(root, "skills", "user-extra"), exist_ok=True)
        with open(os.path.join(root, "skills", "user-extra", "NOTE.md"), "w") as f:
            f.write("用户旁文件\n")
        archive2, digest2 = make_release(base, "2.0.0")

        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", archive2, "--digest", "sha256:" + digest2])
        check_eq("平铺迁移退 0", 0, rc)
        if rc != 0:
            print(out)
            return

        dirname2 = updater.version_dirname("2.0.0", digest2)
        pointer = current_of(root)
        check_eq("指针指向新版本目录", dirname2, pointer["current"] if pointer else None)
        check("无平铺 previous", (pointer or {}).get("previous") in (None, ""))
        vdir = os.path.join(root, "versions", dirname2)
        check("版本目录整包在", os.path.isfile(os.path.join(vdir, EXE_NAME)) and
              os.path.isfile(os.path.join(vdir, "manifest.json")))
        check_eq("版本目录 EXE 是新版",
                 fake_exe("2.0.0"), open(os.path.join(vdir, EXE_NAME)).read())
        check("用户旁文件保留在原地",
              os.path.isfile(os.path.join(root, "skills", "user-extra", "NOTE.md")))
        # 根位换启动器(= 新版 EXE 字节);旧 EXE 进备份
        check_eq("根位是启动器(新版字节)",
                 exe_v2_bytes(base, "2.0.0"), open(os.path.join(root, EXE_NAME), "rb").read())
        backups = os.listdir(os.path.join(root, "backups"))
        legacy = None
        for b in backups:
            cand = os.path.join(root, "backups", b, "legacy", EXE_NAME)
            if os.path.isfile(cand):
                legacy = cand
        check("旧 EXE 进备份", legacy is not None, str(backups))
        if legacy:
            check_eq("备份里是旧 EXE 字节", exe_v1, open(legacy, "rb").read())
        check("根 updater 树就位",
              os.path.isfile(os.path.join(root, "updater", "updater.py")))
        state = json.load(open(os.path.join(root, "install-state.json"), encoding="utf-8-sig"))
        check_eq("install-state schema 2", 2, state.get("schema"))
        check_eq("install-state 版本", "2.0.0", state.get("version"))
        check_eq("install-state 记摘要", "sha256:" + digest2,
                 (state.get("source") or {}).get("asset_digest"))
        txns = txn_files(root)
        check_eq("一笔事务", 1, len(txns))
        record = read_txn(root, txns[0])
        check_eq("事务已提交", "committed", record.get("state"))
        check_eq("事务状态流走到 committed",
                 ["waiting-for-idle", "activating", "healthy", "committed"][-1],
                 record.get("state"))
        check("staging 清空", not os.listdir(os.path.join(root, "staging")))

        # 幂等重跑:免下载,不添新目录
        rc, out = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                               "--archive", archive2, "--digest", "sha256:" + digest2])
        check_eq("幂等重跑退 0", 0, rc)
        check("幂等重跑免下载", "已在且核对通过" in out, out[-300:])
        check_eq("版本目录不重复", 1, len(os.listdir(os.path.join(root, "versions"))))
        check_eq("幂等重跑不添事务", 1, len(txn_files(root)))
    finally:
        shutil.rmtree(base, ignore_errors=True)


def exe_v2_bytes(base, version):
    return open(os.path.join(build_pkg_dir(base, version), EXE_NAME), "rb").read()


# ---------------------------------------------------------------------------
# F. 冲突 -> needs-review -> 解决后续跑(POSIX)
# ---------------------------------------------------------------------------

def section_needs_review():
    if not POSIX_OK:
        print("== F. needs-review(POSIX only,跳过)")
        return
    print("== F. 冲突停 needs-review,解决后续跑")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        archive2, digest2 = make_release(base, "2.0.0")
        rc, _ = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                             "--archive", archive2, "--digest", "sha256:" + digest2])
        check_eq("前置迁移成功", 0, rc)

        # 用户改了当前版本目录里的官方技能 -> 冲突
        dirname2 = updater.version_dirname("2.0.0", digest2)
        skill = os.path.join(root, "versions", dirname2, "skills/lubancode-config/SKILL.md")
        original = open(skill).read()
        with open(skill, "w") as f:
            f.write(original + "用户自己改的\n")

        archive3, digest3 = make_release(base, "3.0.0")
        rc, out = run_updater(["update", "--install-root", root, "--version", "3.0.0",
                               "--archive", archive3, "--digest", "sha256:" + digest3])
        check_eq("冲突退 2(needs-review)", 2, rc)
        check("冲突报预检", "冲突" in out, out[-400:])
        pointer = current_of(root)
        check_eq("指针不动", dirname2, pointer["current"] if pointer else None)
        txns = txn_files(root)
        record = read_txn(root, txns[-1])
        check_eq("事务停 needs-review", "needs-review", record.get("state"))
        check("冲突明细入账", record.get("blocking"), json.dumps(record)[:200])
        # staging 保留(续跑免重下)
        staged_txn = txns[-1][:-5]
        check("staging 留档待续", os.path.isdir(os.path.join(root, "staging", staged_txn)))

        # 解决:还原官方件,重跑 -> 同一笔事务续上
        with open(skill, "w") as f:
            f.write(original)
        rc, out = run_updater(["update", "--install-root", root, "--version", "3.0.0",
                               "--archive", archive3, "--digest", "sha256:" + digest3])
        check_eq("解决后续跑退 0", 0, rc)
        check("续上同一事务", "续上事务" in out, out[-300:])
        dirname3 = updater.version_dirname("3.0.0", digest3)
        pointer = current_of(root)
        check_eq("指针到 3.0.0", dirname3, pointer["current"] if pointer else None)
        check_eq("previous 记 2.0.0", dirname2, pointer["previous"] if pointer else None)
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# G. 回滚(POSIX)
# ---------------------------------------------------------------------------

def section_rollback():
    if not POSIX_OK:
        print("== G. 回滚(POSIX only,跳过)")
        return
    print("== G. 回滚")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        archive2, digest2 = make_release(base, "2.0.0")
        archive3, digest3 = make_release(base, "3.0.0")
        run_updater(["update", "--install-root", root, "--version", "2.0.0",
                     "--archive", archive2, "--digest", "sha256:" + digest2])
        rc, _ = run_updater(["update", "--install-root", root, "--version", "3.0.0",
                             "--archive", archive3, "--digest", "sha256:" + digest3])
        check_eq("前置 3.0.0 上线", 0, rc)
        dirname2 = updater.version_dirname("2.0.0", digest2)
        dirname3 = updater.version_dirname("3.0.0", digest3)

        rc, out = run_updater(["rollback", "--install-root", root])
        check_eq("回滚退 0", 0, rc)
        pointer = current_of(root)
        check_eq("回滚指回 2.0.0", dirname2, pointer["current"] if pointer else None)
        check_eq("回滚记 previous=3.0.0", dirname3, pointer["previous"] if pointer else None)
        state = json.load(open(os.path.join(root, "install-state.json"), encoding="utf-8-sig"))
        check_eq("install-state 版本随回滚", "2.0.0", state.get("version"))
        # 版本目录都在,随时切回
        check("3.0.0 目录保留",
              os.path.isdir(os.path.join(root, "versions", dirname3)))

        # 无 previous 的版本化布局:明拒
        base2 = tempfile.mkdtemp(prefix="luban-updater-test2-")
        root2 = make_flat_install(base2, "1.0.0")
        rc0, _ = run_updater(["update", "--install-root", root2, "--version", "2.0.0",
                              "--archive", archive2, "--digest", "sha256:" + digest2])
        check_eq("前置单版本", 0, rc0)
        # 手工抹掉 previous,模拟无可回滚
        pointer2 = current_of(root2)
        pointer2["previous"] = None
        updater.atomic_write_json(os.path.join(root2, "current.json"), pointer2)
        rc, out = run_updater(["rollback", "--install-root", root2])
        check_eq("无 previous 退 1", 1, rc)
        check("无 previous 明说", "上次可用" in out, out[-300:])
        shutil.rmtree(base2, ignore_errors=True)
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# H. 清理(POSIX)
# ---------------------------------------------------------------------------

def section_gc():
    if not POSIX_OK:
        print("== H. 清理(POSIX only,跳过)")
        return
    print("== H. 清理:留当前/上次可用,清单外先进备份")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        archive2, digest2 = make_release(base, "2.0.0")
        archive3, digest3 = make_release(base, "3.0.0")
        run_updater(["update", "--install-root", root, "--version", "2.0.0",
                     "--archive", archive2, "--digest", "sha256:" + digest2])
        run_updater(["update", "--install-root", root, "--version", "3.0.0",
                     "--archive", archive3, "--digest", "sha256:" + digest3])
        # 手工塞一个更老的版本目录,带清单外用户文件
        old_dir = os.path.join(root, "versions", "0.9.0-00000000")
        shutil.copytree(os.path.join(root, "versions",
                                     updater.version_dirname("2.0.0", digest2)), old_dir)
        with open(os.path.join(old_dir, "user-dropped.txt"), "w") as f:
            f.write("用户塞进旧版本的文件\n")

        rc, out = run_updater(["gc", "--install-root", root])
        check_eq("清理退 0", 0, rc)
        check("老版本移除", not os.path.isdir(old_dir))
        stranded = os.path.join(root, "backups", "stranded-0.9.0-00000000", "user-dropped.txt")
        check("清单外文件进备份", os.path.isfile(stranded))
        keep = {current_of(root)["current"], current_of(root)["previous"]}
        remaining = set(os.listdir(os.path.join(root, "versions")))
        check("当前/上次可用都在", keep <= remaining, "%s vs %s" % (keep, remaining))
        check_eq("只多不少(2 个)", 2, len(remaining))
    finally:
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------------------
# I. 断点续传(本地 HTTP 支持 Range;两平台)
# ---------------------------------------------------------------------------

class RangeHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = os.path.join(self.server.root_dir, self.path.lstrip("/"))
        data = open(path, "rb").read()
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[len("bytes="):].split("-")[0])
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
            self.send_header("Content-Length", str(len(data) - start))
            self.end_headers()
            self.wfile.write(data[start:])
        else:
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def log_message(self, *_args):
        pass


def section_resume_download():
    print("== I. 断点续传核对同资产")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    serve_dir = None
    server = None
    try:
        payload = os.urandom(300_000)
        src = os.path.join(base, "asset.bin")
        with open(src, "wb") as f:
            f.write(payload)
        serve_dir = tempfile.mkdtemp(prefix="luban-updater-serve-")
        shutil.copy2(src, os.path.join(serve_dir, "asset.bin"))
        handler = RangeHandler
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server.root_dir = serve_dir
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = "http://127.0.0.1:%d/asset.bin" % port
        digest = hashlib.sha256(payload).hexdigest()

        dest = os.path.join(base, "out.bin")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        # 正确前缀的 partial:206 续传成功
        with open(dest + ".part", "wb") as f:
            f.write(payload[:100_000])
        updater.download_with_resume(url, dest, digest, len(payload))
        check("续传(正确前缀)成功", sha256(dest) == digest)

        # 错误前缀的 partial:续完摘要必红
        os.remove(dest)
        with open(dest + ".part", "wb") as f:
            f.write(os.urandom(100_000))
        try:
            updater.download_with_resume(url, dest, digest, len(payload))
            bad("续传(错误前缀)必红", "摘要竟然过了")
        except updater.UpdaterError as exc:
            check("续传(错误前缀)必红", "摘要不符" in str(exc), str(exc))
        check("坏 partial 已删", not os.path.exists(dest + ".part"))

        # 声明大小不合:拒绝
        if os.path.exists(dest):
            os.remove(dest)
        try:
            updater.download_with_resume(url, dest, "0" * 64, len(payload) + 1)
            bad("下载不完整必红", "竟然过了")
        except updater.UpdaterError as exc:
            check("下载不完整必红", True, str(exc))
    finally:
        if server is not None:
            server.shutdown()
        shutil.rmtree(base, ignore_errors=True)
        if serve_dir is not None:
            shutil.rmtree(serve_dir, ignore_errors=True)


# ---------------------------------------------------------------------------
# J. 目标已换:在途事务作废清场(POSIX;断言部分两平台成立)
# ---------------------------------------------------------------------------

def section_supersede():
    if not POSIX_OK:
        print("== J. 目标已换(POSIX only,跳过)")
        return
    print("== J. 目标已换:在途事务作废")
    base = tempfile.mkdtemp(prefix="luban-updater-test-")
    try:
        root = make_flat_install(base, "1.0.0")
        archive2, digest2 = make_release(base, "2.0.0")
        archive3, digest3 = make_release(base, "3.0.0")

        # 先造一笔停在 needs-review 的 2.0.0 在途账
        skill = os.path.join(root, "skills/lubancode-config/SKILL.md")
        original = open(skill).read()
        with open(skill, "w") as f:
            f.write(original + "用户改过\n")
        rc, _ = run_updater(["update", "--install-root", root, "--version", "2.0.0",
                             "--archive", archive2, "--digest", "sha256:" + digest2])
        check_eq("前置 needs-review", 2, rc)
        with open(skill, "w") as f:
            f.write(original)

        # 改追 3.0.0:旧在途账作废清场,新账走完
        rc, out = run_updater(["update", "--install-root", root, "--version", "3.0.0",
                               "--archive", archive3, "--digest", "sha256:" + digest3])
        check_eq("改追新版退 0", 0, rc)
        records = [read_txn(root, name) for name in txn_files(root)]
        superseded = [r for r in records if r.get("state") == "failed"
                      and r.get("reason") == "superseded"]
        check_eq("旧账作废一笔", 1, len(superseded))
        stale_staging = [d for d in os.listdir(os.path.join(root, "staging"))
                         if read_txn(root, d + ".json").get("reason") == "superseded"]
        check("作废账 staging 清场", not stale_staging, str(stale_staging))
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    sections = [
        ("A", section_units),
        ("B", section_layout),
        ("C", section_lock),
        ("D", section_failure_paths),
        ("E", section_e2e),
        ("F", section_needs_review),
        ("G", section_rollback),
        ("H", section_gc),
        ("I", section_resume_download),
        ("J", section_supersede),
    ]
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for tag, func in sections:
        if only is None or only == tag:
            func()
    print("\n%d 项用例:%d 过,%d 挂" % (PASS + FAIL, PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
