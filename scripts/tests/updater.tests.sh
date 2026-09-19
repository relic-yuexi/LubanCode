#!/bin/bash
# updater.py / updater_tests.py 的壳(GitHubRelease自动更新单 P1)。
#
# 手工跑:bash scripts/tests/updater.tests.sh
# CI 里由 ci.yml 的 install-scripts 腿跑(windows + ubuntu)。重活都在
# updater_tests.py 里;这里只做 python 探针与退出码转交。
set -uo pipefail

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$(dirname "$TESTS_DIR")")

# Windows 上 python 往 cp1252 控制台打中文会 UnicodeEncodeError,统一按 UTF-8 走
export PYTHONUTF8=1 PYTHONIOENCODING=utf-8

# 探针:Windows 的 python3.exe 常是商店假桩(退出码 49),先验真身再选用
PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys' >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "错误:需要 python3(或 python)跑更新器测试。" >&2
    exit 1
fi

cd "$ROOT"
"$PY" scripts/tests/updater_tests.py
