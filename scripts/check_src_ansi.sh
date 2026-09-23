#!/usr/bin/env bash
# src/ 八进制 ESC 裸转义 lint(TUI 排版批 8 收口)。
# 批 6 已把全仓硬编码 ANSI 清零、统一 \x1b 十六进制写法,彩色收进
# cli::Theme 字段;这道门把成果焊死——src/ 里再出现 \033(含 \033[ 一切
# 变体)即红。约定出处:docs/development/tui_style.md 总规矩 3(颜色与
# 边框不写死 ANSI)。注释与字符串一视同仁:现仓 src/ 注释也统一 \x1b
# 写法、零 \033 存量,不搞"注释豁免"启发式——规则一刀切,好查好守。
# 用法:bash scripts/check_src_ansi.sh [--selftest]
#   --selftest 先种一枚 \033 探针验证 lint 真能红(防"常绿的假门"),
#   再查真身。CI 的 changes 门房带 --selftest 跑。
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

# git grep 退出码 0=有命中(即违规);-I 跳过二进制。只扫 git 跟踪的
# 工作树文件——未跟踪的编辑器临时件不搅局。
scan() {
    (cd "$REPO_ROOT" && git grep -n -I '\\033' -- src)
}

hits=$(scan || true)
if [ -n "$hits" ]; then
    count=$(printf '%s\n' "$hits" | grep -c .)
    # 逐处发 GitHub 注解:PR 页与运行日志点开即达文件行号。
    printf '%s\n' "$hits" | while IFS=: read -r file line _rest; do
        echo "::error file=$file,line=$line::src/ 出现八进制 ESC 转义 \\033。改法:转义序列改写 \\x1b 十六进制同款(如 \\033[1m 改 \\x1b[1m);彩色一律走 cli::Theme 字段、框线横线走 cli::frame/divider 基件,不写死 ANSI(约定:docs/development/tui_style.md 总规矩 3)"
    done
    printf 'src ANSI lint 红:%s 处 \\033 裸转义(上方注解逐处标红):\n%s\n' \
        "$count" "$hits" >&2
    exit 1
fi
echo "src ANSI lint 绿:\\033 裸转义零命中。"

if [ "${1:-}" = "--selftest" ]; then
    probe="src/_ansi_lint_probe.tmp"
    printf 'const char* kProbe = "\\033[31m";\n' >"$REPO_ROOT/$probe"
    git -C "$REPO_ROOT" add -f "$probe" >/dev/null
    probe_seen=0
    scan >/dev/null 2>&1 || probe_seen=$?
    git -C "$REPO_ROOT" reset -q -- "$probe"
    rm -f "$REPO_ROOT/$probe"
    if [ "$probe_seen" -ne 0 ]; then
        echo "self-test 红:种下的 \\033 探针没被 lint 抓住——门自身坏了,这盏绿灯不可信。" >&2
        exit 1
    fi
    echo "self-test 绿:lint 对种下的违规如实报红,门是真的。"
fi
