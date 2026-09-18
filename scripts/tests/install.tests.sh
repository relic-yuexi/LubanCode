#!/bin/bash
# install.sh / install_plan.py / generate_manifest.py 的行为用例
# (GitHubRelease自动更新单 P0:技能保护、所有权判定、needs-review、备份)。
#
# 手工跑:bash scripts/tests/install.tests.sh
# CI 里由 ci.yml 的 install-scripts 腿跑(ubuntu 全量;windows 腿跑除
# install.sh 端到端外的部分——假 exe 是 shell 脚本,Windows 跑不动)。
#
# 覆盖:
#   1. 路径规则单元(三个实现同一契约:绝对路径/盘符/UNC/../ADS/保留名/结尾点空格)
#   2. generate_manifest.py:记账正确、确定性、--check 对账、篡改必红
#   3. install_plan.py:全新装记档 → 本地改动 → v2 升级,各类去向逐一断言;
#      重复安装零改动;预演只读
#   4. 无基线旧安装:默认 needs-review 退 3 且先完整备份;--baseline-dir 建基线;
#      --allow-unknown-replace 备份后整目录替换
#   5. install.sh 端到端(仅 unix):全新装(bin 布局落 share)、升级、
#      无基线退 3、同目录安装、用户 HOME 不被触碰

set -uo pipefail

# Windows 上 python 往 cp1252 控制台打中文会 UnicodeEncodeError,统一按 UTF-8 走
export PYTHONUTF8=1 PYTHONIOENCODING=utf-8

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$(dirname "$TESTS_DIR")")

PASS=0
FAIL=0

note() { echo "== $*"; }
ok() { PASS=$((PASS + 1)); echo "[PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); echo "[FAIL] $*"; echo "       期望: $2"; echo "       实际: $3"; }

assert_eq() {
    # assert_eq <名称> <期望> <实际>
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "$2" "$3"; fi
}

assert_file_text() {
    # assert_file_text <名称> <路径> <期望全文>
    local actual='<absent>'
    if [ -f "$2" ]; then actual=$(cat "$2"); fi
    assert_eq "$1" "$3" "$actual"
}

assert_rc() {
    # assert_rc <名称> <期望码> <实际码>
    assert_eq "$1" "$2" "$3"
}

# 探针:Windows 的 python3.exe 常是商店假桩(退出码 49),先验真身再选用
PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys' >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "错误:需要 python3(或 python)跑清单与所有权引擎测试。" >&2
    exit 1
fi

WORK=$(mktemp -d 2>/dev/null || mktemp -d -t lubancode-tests)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

cd "$ROOT"

# ------------------------------------------------------------------
note "1. 路径规则单元(三个实现同一契约)"

"$PY" - "$ROOT" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1] + "/scripts")
import generate_manifest as g
import install_plan as p

# generate_manifest 返 (ok, reason) 元组,install_plan 返 bool;统一解包
def gv(x):
    r = g.valid_relpath(x)
    return r[0] if isinstance(r, tuple) else r

bad = [
    "", "/abs.md", "a\\b.md", "C:/x.md", "a:b", "//server/share",
    "skills/../secrets", "./skills/a.md", "skills//a.md",
    "skills/a.md.", "skills/a.md ", "docs/CON", "docs/com1.md",
    "docs/a*b.md", "a/" * 300 + "x",
]
good = ["skills/lubancode-config/SKILL.md", "docs/中文指南.md", "Skills/A.md", "web/assistant/app.js"]

for b in bad:
    assert not gv(b), "generate_manifest 应拒绝:" + repr(b)
    assert not p.valid_relpath(b), "install_plan 应拒绝:" + repr(b)
for x in good:
    assert gv(x), "generate_manifest 应放行:" + repr(x)
    assert p.valid_relpath(x), "install_plan 应放行:" + repr(x)
print("valid_relpath 双实现一致 OK")
PYEOF
assert_rc "路径规则单元通过" 0 $?

# ------------------------------------------------------------------
note "2. generate_manifest.py"

GEN="$ROOT/scripts/generate_manifest.py"
FIX="$WORK/fixture"
mkdir -p "$FIX/skills/lubancode-config/references" "$FIX/docs" "$FIX/web/assistant" "$FIX/libexec" "$FIX/licenses"
printf 'router skill v1' > "$FIX/skills/lubancode-config/SKILL.md"
printf 'see ../../docs' > "$FIX/skills/lubancode-config/references/document-map.md"
printf 'docs index' > "$FIX/docs/README.md"
printf '<html>assistant</html>' > "$FIX/web/assistant/index.html"
printf 'fake rg' > "$FIX/libexec/rg"
printf 'MIT' > "$FIX/licenses/ripgrep-MIT.txt"
printf 'fake exe' > "$FIX/lubancode"
printf 'notices' > "$FIX/THIRD_PARTY_NOTICES.md"
printf 'MIT' > "$FIX/LICENSE"

"$PY" "$GEN" --root "$FIX" --version 1.0.0 --platform test-x64 --channel stable >/dev/null
assert_rc "生成 manifest 退出码 0" 0 $?
test -f "$FIX/manifest.json"
assert_rc "manifest.json 落在包根" 0 $?

"$PY" - "$FIX" <<'PYEOF'
import json, sys, hashlib, os
root = sys.argv[1]
m = json.load(open(os.path.join(root, "manifest.json"), encoding="utf-8-sig"))
paths = {e["path"] for e in m["files"]}
expect = {
    "skills/lubancode-config/SKILL.md",
    "skills/lubancode-config/references/document-map.md",
    "docs/README.md", "web/assistant/index.html", "libexec/rg",
    "licenses/ripgrep-MIT.txt", "lubancode", "THIRD_PARTY_NOTICES.md", "LICENSE",
}
assert paths == expect, "paths 不合:%r" % (paths ^ expect)
assert m["file_count"] == len(m["files"]) == 9
assert "manifest.json" not in paths, "清单不能给自己记账"
sha = hashlib.sha256(open(os.path.join(root, "skills/lubancode-config/SKILL.md"), "rb").read()).hexdigest()
entry = [e for e in m["files"] if e["path"] == "skills/lubancode-config/SKILL.md"][0]
assert entry["sha256"] == sha and entry["size"] == len(b"router skill v1")
roles = {e["path"]: e["role"] for e in m["files"]}
assert roles["lubancode"] == "exe" and roles["skills/lubancode-config/SKILL.md"] == "skills"
raw = open(os.path.join(root, "manifest.json"), "rb").read()
raw.decode("ascii")
print("manifest 内容对账 OK")
PYEOF
assert_rc "manifest 内容对账" 0 $?

cp "$FIX/manifest.json" "$WORK/manifest-first.json"
"$PY" "$GEN" --root "$FIX" --version 1.0.0 --platform test-x64 --channel stable --out "$WORK/manifest-second.json" >/dev/null
cmp -s "$WORK/manifest-first.json" "$WORK/manifest-second.json"
assert_rc "同参数重生成逐字节一致(确定性)" 0 $?

"$PY" "$GEN" --root "$FIX" --version 1.0.0 --platform test-x64 --channel stable --check >/dev/null
assert_rc "--check 对账通过" 0 $?

printf 'tampered' >> "$FIX/docs/README.md"
"$PY" "$GEN" --root "$FIX" --version 1.0.0 --platform test-x64 --channel stable --check >/dev/null 2>&1
rc=$?
if [ "$rc" != 0 ]; then ok "篡改后 --check 必红"; else fail "篡改后 --check 必红" "非零" "0"; fi

# ------------------------------------------------------------------
note "3. install_plan.py:全新装 → 本地改动 → v2 升级"

PLAN="$ROOT/scripts/install_plan.py"

mk_pkg() {
    # mk_pkg <dir> <v标记> <docs内容>
    mkdir -p "$1/skills/lubancode-config" "$1/docs" "$1/web/assistant" "$1/libexec" "$1/licenses"
    printf 'fake exe %s' "$2" > "$1/lubancode"
    printf '%s' "$2" > "$1/skills/lubancode-config/SKILL.md"
    printf 'shared config' > "$1/skills/lubancode-config/shared.md"
    printf '%s' "$3" > "$1/docs/README.md"
    printf 'docs old page' > "$1/docs/old-page.md"
    printf '<html>%s</html>' "$2" > "$1/web/assistant/index.html"
    printf 'app %s' "$2" > "$1/web/assistant/app.js"
    printf 'fake rg %s' "$2" > "$1/libexec/rg"
    printf 'MIT' > "$1/licenses/ripgrep-MIT.txt"
    printf 'notices %s' "$2" > "$1/THIRD_PARTY_NOTICES.md"
    printf 'MIT' > "$1/LICENSE"
}

PKG1="$WORK/pkg1"; PKG2="$WORK/pkg2"
mk_pkg "$PKG1" v1 v1docs
mk_pkg "$PKG2" v2 v2docs
# v2:删掉 old-page(退役用),新增 new-page
rm "$PKG2/docs/old-page.md"
printf 'brand new' > "$PKG2/docs/new-page.md"
"$PY" "$GEN" --root "$PKG1" --version 1.0.0 --platform test-x64 --channel stable >/dev/null
"$PY" "$GEN" --root "$PKG2" --version 2.0.0 --platform test-x64 --channel stable >/dev/null

INST="$WORK/inst"

plan_run() {
    # plan_run <子命令> <来源包> <安装根> [其余参数...]
    sub=$1; src=$2; inst=$3; shift 3
    "$PY" "$PLAN" "$sub" \
        --source "$src" --record-dir "$inst" \
        --map "skills=$inst/skills" --map "docs=$inst/docs" --map "web=$inst/web" \
        --map "libexec=$inst/libexec" --map "licenses=$inst/licenses" \
        "$@"
}

# 3a. 全新装
plan_run apply "$PKG1" "$INST" >/dev/null
assert_rc "全新装退出码 0" 0 $?
assert_file_text "全新装:官方技能落地" "$INST/skills/lubancode-config/SKILL.md" "v1"
assert_file_text "全新装:docs 落地" "$INST/docs/README.md" "v1docs"
assert_file_text "全新装:rg 落地" "$INST/libexec/rg" "fake rg v1"
test -f "$INST/install-state.json"
assert_rc "全新装写了安装记录" 0 $?
test -f "$INST/manifest.json"
assert_rc "全新装抄了官方清单" 0 $?

# 3b. 本地改动:改官方件、加未知件、删一件官方件
printf 'my precious local edit' > "$INST/skills/lubancode-config/SKILL.md"
mkdir -p "$INST/skills/mine"
printf 'user skill stays' > "$INST/skills/mine/MINE.md"
printf 'unknown css' > "$INST/web/assistant/custom.css"
rm "$INST/docs/README.md"

# 3c. 预演(v2):只读,报得准
plan_run plan "$PKG2" "$INST" > "$WORK/scan-v2.txt" 2>&1
assert_rc "升级预演退出码 0" 0 $?
grep -q '\[plan\] conflict-modified skills/lubancode-config/SKILL.md' "$WORK/scan-v2.txt"
assert_rc "预演报出本地改过的 SKILL.md 冲突" 0 $?
grep -q 'missing-kept docs/README.md' "$WORK/scan-v2.txt"
assert_rc "预演报出本地删掉的官方件不复活" 0 $?
grep -q '\[plan\] retire docs/old-page.md' "$WORK/scan-v2.txt"
assert_rc "预演报出官方退役件" 0 $?

( cd "$INST" && find . -type f -exec sha256sum {} + | sort ) > "$WORK/before-scan.sha"
plan_run plan "$PKG2" "$INST" >/dev/null 2>&1
( cd "$INST" && find . -type f -exec sha256sum {} + | sort ) > "$WORK/after-scan.sha"
cmp -s "$WORK/before-scan.sha" "$WORK/after-scan.sha"
assert_rc "预演不动盘面一个字节" 0 $?

# 3d. v2 升级(有基线)
plan_run apply "$PKG2" "$INST" > "$WORK/apply-v2.txt" 2>&1
assert_rc "v2 升级退出码 0" 0 $?

assert_file_text "本地改过的官方件原样保留" "$INST/skills/lubancode-config/SKILL.md" "my precious local edit"
assert_file_text "本地删掉的官方 docs/README.md 不复活" "$INST/docs/README.md" "<absent>"
assert_file_text "官方已删且本地未改的 old-page.md 退役" "$INST/docs/old-page.md" "<absent>"
assert_file_text "新版新增 new-page.md 落地" "$INST/docs/new-page.md" "brand new"
assert_file_text "官方未改的 web 换新" "$INST/web/assistant/index.html" "<html>v2</html>"
assert_file_text "官方未改的 app.js 换新" "$INST/web/assistant/app.js" "app v2"
assert_file_text "官方未改的 rg 换新" "$INST/libexec/rg" "fake rg v2"
assert_file_text "未知 css 保留" "$INST/web/assistant/custom.css" "unknown css"
assert_file_text "未知用户技能保留" "$INST/skills/mine/MINE.md" "user skill stays"
grep -q 'conflict-modified skills/lubancode-config/SKILL.md' "$WORK/apply-v2.txt"
assert_rc "冲突清单里点名 SKILL.md" 0 $?

# 备份里有原件
backup_found=""
for d in "$INST"/backups/*/; do
    if [ -f "$d/skills/lubancode-config/SKILL.md" ]; then backup_found=$d; fi
done
assert_file_text "备份里有本地改过的原件" "${backup_found}skills/lubancode-config/SKILL.md" "my precious local edit"
grep -q '"version": "2.0.0"' "$INST/install-state.json"
assert_rc "安装记录更新到 v2 清单" 0 $?

# 3e. 重复安装:零改动
plan_run apply "$PKG2" "$INST" > "$WORK/apply-again.txt" 2>&1
assert_rc "重复安装退出码 0" 0 $?
plan_run plan "$PKG2" "$INST" > "$WORK/plan-again.txt" 2>&1
grep -q 'skip-current=9' "$WORK/plan-again.txt"
assert_rc "重复安装:9 个官方件全部 skip-current" 0 $?

# ------------------------------------------------------------------
note "4. 无基线旧安装:needs-review / --baseline-dir / --allow-unknown-replace"

LEGACY="$WORK/legacy"
mkdir -p "$LEGACY/skills/lubancode-config"
printf 'old skill maybe touched' > "$LEGACY/skills/lubancode-config/SKILL.md"
printf 'fake exe old' > "$LEGACY/lubancode"

plan_run apply "$PKG2" "$LEGACY" > "$WORK/legacy-stop.txt" 2>&1
assert_rc "无基线默认 needs-review 退 3" 3 $?
assert_file_text "needs-review 后旧技能仍在原地" "$LEGACY/skills/lubancode-config/SKILL.md" "old skill maybe touched"
legacy_backups=$(ls -1 "$LEGACY/backups" 2>/dev/null | wc -l | tr -d ' ')
assert_eq "needs-review 先做了完整备份" "1" "$legacy_backups"

plan_run apply "$PKG2" "$LEGACY" --baseline-dir "$PKG1" > "$WORK/legacy-baseline.txt" 2>&1
assert_rc "--baseline-dir 建基线后放行" 0 $?
assert_file_text "基线判定:改过的 SKILL.md 保留(与基线官方不同)" "$LEGACY/skills/lubancode-config/SKILL.md" "old skill maybe touched"
grep -q 'conflict-modified skills/lubancode-config/SKILL.md' "$WORK/legacy-baseline.txt"
assert_rc "基线判定把差异点名为冲突" 0 $?
assert_file_text "基线判定:全新路径照常落" "$LEGACY/docs/new-page.md" "brand new"

# --allow-unknown-replace:备份后整目录替换
LEGACY2="$WORK/legacy2"
mkdir -p "$LEGACY2/skills/keepme"
printf 'replace me not silently' > "$LEGACY2/skills/keepme/OLD.md"
printf 'fake exe old' > "$LEGACY2/lubancode"
plan_run apply "$PKG2" "$LEGACY2" --allow-unknown-replace > "$WORK/legacy-replace.txt" 2>&1
assert_rc "--allow-unknown-replace 放行" 0 $?
assert_file_text "整目录替换后旧技能被新版顶掉" "$LEGACY2/skills/lubancode-config/SKILL.md" "v2"
wholesale_backup=$(ls -1 "$LEGACY2/backups" | wc -l | tr -d ' ')
assert_eq "整目录替换前有备份" "1" "$wholesale_backup"
backup_old=""
for d in "$LEGACY2"/backups/*/; do
    if [ -f "$d/skills/keepme/OLD.md" ]; then backup_old=$d; fi
done
assert_file_text "被顶掉的旧技能在备份里找得回" "${backup_old}skills/keepme/OLD.md" "replace me not silently"

# ------------------------------------------------------------------
note "5. install.sh 端到端(unix 原生;git-bash 实测也能跑,一并放行)"

IS_UNIX=1
case "$(uname -s)" in
    CYGWIN*) IS_UNIX=0 ;;
esac

if [ "$IS_UNIX" = 1 ]; then
    HOME_FAKE="$WORK/home"
    mkdir -p "$HOME_FAKE/.lubancode/skills/mine"
    printf 'user home skill' > "$HOME_FAKE/.lubancode/skills/mine/SKILL.md"

    mk_shpkg() {
        # mk_shpkg <源pkg> <目标> —— 换成可执行假 exe 后重生成清单
        cp -R "$1" "$2"
        printf '#!/bin/sh\necho "lubancode 9.9.9"\n' > "$2/lubancode"
        chmod +x "$2/lubancode" "$2/libexec/rg"
        cp "$ROOT/scripts/install.sh" "$2/"
        cp "$ROOT/scripts/install_plan.py" "$2/"
        chmod +x "$2/install.sh"
        rm -f "$2/manifest.json"
        "$PY" "$GEN" --root "$2" --version 9.9.9 --platform test-x64 --channel stable >/dev/null
    }
    SHPKG1="$WORK/shpkg1"; SHPKG2="$WORK/shpkg2"
    mk_shpkg "$PKG1" "$SHPKG1"
    mk_shpkg "$PKG2" "$SHPKG2"

    SHINST="$WORK/shroot"
    # 5a. 全新装:bin 布局,skills/docs 落 ../share/lubancode
    HOME="$HOME_FAKE" sh "$SHPKG1/install.sh" PREFIX="$SHINST/bin" > "$WORK/sh-fresh.txt" 2>&1
    assert_rc "install.sh 全新装退出码 0" 0 $?
    grep -q 'lubancode 9.9.9' "$SHINST/bin/lubancode"
    assert_rc "exe 落 bin" 0 $?
    assert_file_text "bin 布局:skills 落 share" "$SHINST/share/lubancode/skills/lubancode-config/SKILL.md" "v1"
    assert_file_text "libexec 贴着 exe" "$SHINST/bin/libexec/rg" "fake rg v1"
    test -f "$SHINST/bin/install-state.json"
    assert_rc "install.sh 全新装写记录" 0 $?

    # 5b. 升级:改官方件再跑 v2
    printf 'local hack' > "$SHINST/share/lubancode/skills/lubancode-config/SKILL.md"
    HOME="$HOME_FAKE" sh "$SHPKG2/install.sh" PREFIX="$SHINST/bin" > "$WORK/sh-upgrade.txt" 2>&1
    assert_rc "install.sh 升级退出码 0" 0 $?
    assert_file_text "升级后本地改动保留" "$SHINST/share/lubancode/skills/lubancode-config/SKILL.md" "local hack"
    assert_file_text "升级后 web 换新" "$SHINST/bin/web/assistant/index.html" "<html>v2</html>"

    # 5c. 无基线:删记录,应退 3 且先备份
    rm -f "$SHINST/bin/install-state.json" "$SHINST/bin/manifest.json"
    HOME="$HOME_FAKE" sh "$SHPKG2/install.sh" PREFIX="$SHINST/bin" > "$WORK/sh-legacy.txt" 2>&1
    assert_rc "install.sh 无基线退 3" 3 $?
    assert_file_text "退 3 后本地改动仍在" "$SHINST/share/lubancode/skills/lubancode-config/SKILL.md" "local hack"

    # 5d. 同目录安装:不搬自己,补记档
    ( cd "$SHPKG2" && HOME="$HOME_FAKE" sh ./install.sh PREFIX="$SHPKG2" > "$WORK/sh-samedir.txt" 2>&1 )
    assert_rc "同目录安装退出码 0" 0 $?
    grep -q '同目录安装' "$WORK/sh-samedir.txt"
    assert_rc "同目录安装按'不搬自己'处理" 0 $?
    test -f "$SHPKG2/install-state.json"
    assert_rc "同目录安装补了记档" 0 $?

    # 5e. 用户 HOME 全程不动
    assert_file_text "用户 HOME 技能未被触碰" "$HOME_FAKE/.lubancode/skills/mine/SKILL.md" "user home skill"
else
    note "跳过 install.sh 端到端(Windows/git-bash:假 exe 是 shell 脚本)"
fi

# ------------------------------------------------------------------
echo ""
echo "共 $((PASS + FAIL)) 项,通过 $PASS,失败 $FAIL"
if [ "$FAIL" != 0 ]; then
    exit 1
fi
