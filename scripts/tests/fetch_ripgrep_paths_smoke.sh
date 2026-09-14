#!/usr/bin/env bash
# fetch_ripgrep.sh 数据根解析的离线断言(应用Worker接入单 P1 遗留销账):
# 只跑 --print-target-root——零网络、零下载、不碰 manifest,矩阵盖
# 个人布局/应用根默认 data/显式数据根/合法嵌套/Windows 反斜杠/尾斜杠
# 等值/坏值明拒(空值、相对、根重叠、孤立 DATA_HOME、无主目录)。
#
# 注册:tests/CMakeLists.txt 的 scripts.fetch_ripgrep_paths(有 bash 且非
# Windows 才注册——Windows 的 System32\bash.exe 是 WSL bash,吃不了仓库的
# Windows 路径;该腿如实缺证据,与 node e2e 同款注册法)。
#
# 环境控制用 env -i:runner 环境里残留的 USERPROFILE/LUBANCODE_* 一概
# 不许串场;被测路径只有内置命令,干净环境照样能跑。
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/fetch_ripgrep.sh"
BASH_BIN="$(command -v bash)"

PASS=0
FAILURES=0

note_fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }

# expect_ok <描述> [ENV=值 ...] -- <期望输出>
expect_ok() {
  local desc="$1"; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done
  local expected="$2"
  local out
  out="$(env -i PATH="$PATH" "${envs[@]}" "$BASH_BIN" "$SCRIPT" --print-target-root 2>&1)"
  if [ "$out" = "$expected" ]; then
    PASS=$((PASS + 1))
  else
    note_fail "$desc: 期望 '$expected',实得 '$out'"
  fi
}

# expect_reject <描述> [ENV=值 ...] —— 期望明拒退出(非零,带 stderr 人话)。
expect_reject() {
  local desc="$1"; shift
  local out rc=0
  if [ $# -gt 0 ]; then
    out="$(env -i PATH="$PATH" "$@" "$BASH_BIN" "$SCRIPT" --print-target-root 2>&1)" || rc=$?
  else
    out="$(env -i PATH="$PATH" "$BASH_BIN" "$SCRIPT" --print-target-root 2>&1)" || rc=$?
  fi
  if [ "$rc" -ne 0 ]; then
    PASS=$((PASS + 1))
  else
    note_fail "$desc: 期望明拒退出,实得 0('$out')"
  fi
}

# ---- 个人 CLI 旧布局:数据根=<主目录>/.lubancode,与 C++ 读口同位 ----------
expect_ok "个人布局(HOME)" "HOME=/t/home" -- "/t/home/.lubancode/rg-stage"
expect_ok "个人布局(USERPROFILE 优先,反斜杠折正)" \
  "USERPROFILE=C:\\t\\home" "HOME=/t/posix" -- "C:/t/home/.lubancode/rg-stage"
expect_ok "个人布局(USERPROFILE 空值落 HOME)" \
  "USERPROFILE=" "HOME=/t/home" -- "/t/home/.lubancode/rg-stage"
expect_reject "个人布局无主目录(USERPROFILE/HOME 均未设)"

# ---- 应用根语义:数据根=DATA_HOME 或 <HOME>/data --------------------------
expect_ok "应用根默认数据根" \
  "LUBANCODE_HOME=/t/app" "HOME=/t/home" -- "/t/app/data/rg-stage"
expect_ok "应用根尾斜杠等值" \
  "LUBANCODE_HOME=/t/app/" -- "/t/app/data/rg-stage"
expect_ok "应用根反斜杠折正(Windows 形态)" \
  "LUBANCODE_HOME=C:\\t\\app" -- "C:/t/app/data/rg-stage"
expect_ok "显式数据根" \
  "LUBANCODE_HOME=/t/app" "LUBANCODE_DATA_HOME=/t/state" -- "/t/state/rg-stage"
expect_ok "数据根在参数根之内(合法嵌套)" \
  "LUBANCODE_HOME=/t/app" "LUBANCODE_DATA_HOME=/t/app/st" -- "/t/app/st/rg-stage"

# ---- 坏值明拒:启动门同口径,不静默回个人目录 -------------------------------
expect_reject "孤立数据根(无参数根)" "LUBANCODE_DATA_HOME=/t/state"
expect_reject "LUBANCODE_HOME 空值" "LUBANCODE_HOME="
expect_reject "LUBANCODE_HOME 相对路径" "LUBANCODE_HOME=rel/app"
expect_reject "LUBANCODE_DATA_HOME 空值" "LUBANCODE_HOME=/t/app" "LUBANCODE_DATA_HOME="
expect_reject "LUBANCODE_DATA_HOME 相对路径" "LUBANCODE_HOME=/t/app" "LUBANCODE_DATA_HOME=rel/state"
expect_reject "数据根==参数根" "LUBANCODE_HOME=/t/app" "LUBANCODE_DATA_HOME=/t/app"
expect_reject "数据根==参数根(尾斜杠等值)" "LUBANCODE_HOME=/t/app/" "LUBANCODE_DATA_HOME=/t/app"
expect_reject "参数根落数据根之内" "LUBANCODE_HOME=/t/st/cfg" "LUBANCODE_DATA_HOME=/t/st"

echo "fetch_ripgrep 数据根解析: $PASS 过,$FAILURES 挂"
[ "$FAILURES" -eq 0 ] || exit 1
