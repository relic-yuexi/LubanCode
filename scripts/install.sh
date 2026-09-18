#!/bin/sh
# lubancode 安装脚本(Linux / macOS,POSIX sh 兼容,不依赖 bash 专属语法)
#
# 用法:
#   ./install.sh                              自动挑安装目录(见下面优先级),脚本同目录找可执行文件
#   PREFIX=/opt/lubancode/bin ./install.sh    指定安装目录(PREFIX 就是可执行文件要落地的那个目录本身)
#   ./install.sh --scan                       扫描+预演:列出将替换/保留/备份/冲突项,不动安装
#   ./install.sh --backup-only                完整备份现有受管内容+列盘面清单,不安装
#   ./install.sh --baseline /path/to/old-pkg  旧安装无清单时,拿同版本可信原包目录建基线
#   ./install.sh --allow-unknown-replace      旧安装无清单时,先完整备份再整目录替换(需显式确认)
#
# 安装目录优先级(没设 PREFIX 时):
#   1. $HOME/.local/bin 已经在当前 $PATH 里 → 装这儿,不需要 sudo
#   2. 否则退到 /usr/local/bin;有写权限就直接装,没有就提示用 sudo 重跑
#
# 源文件查找:脚本同目录下找可执行文件,依次探测 lubancode 和 lubancode.exe。
# 这份脚本随 Linux/macOS Release 包分发,只装同目录可执行文件;远程下载由
# 用户先在 Releases 页面选对平台包,脚本不在本机猜架构。
#
# 资源覆盖策略(GitHubRelease自动更新单 §四/§五):所有权按"上次可信官方清单"
# (可执行文件旁边的 install-state.json + manifest.json)判定,不按目录名判定。
# 全新安装走纯 sh 直拷+记档;安装目录已有内容时交给同包的 install_plan.py
# 逐文件判定:官方未改换新(先备份)、本地改过保留+备份+报冲突、未知文件一律
# 保留、官方已删且本地未改随旧版退役。旧安装没有清单且与新包路径相撞:先完整
# 备份再报 needs-review 停手,等 --baseline 建基线或 --allow-unknown-replace
# 显式确认。来源与目标同一目录时不搬自己,只补记档。
# 没装 python3 的机器:全新装照常(记档走极简路径);已有内容的升级/预演跑
# 不了,退回完整备份+人工处理,绝不整目录盲删。
#
# 幂等:重复跑 = 覆盖安装。用户数据(~/.lubancode 等)从头到尾不碰。
# bin 目录按常见前缀布局把 skills/docs 放 ../share/lubancode;web/libexec/
# licenses 永远贴着可执行文件住(search 的生产定位是 ExecutableDir/libexec)。

set -eu

APP_NAME="lubancode"
REPO="relic-yuexi/LubanCode"

err() {
    echo "错误:$1" >&2
    exit 1
}

info() {
    echo "==> $1"
}

SCAN=0
BACKUP_ONLY=0
ALLOW_UNKNOWN_REPLACE=0
BASELINE_ARG=""
PREFIX_ARG=""
prev=""
for arg in "$@"; do
    if [ "$prev" = "--baseline" ]; then
        BASELINE_ARG="$arg"
        prev="$arg"
        continue
    fi
    case "$arg" in
        --scan) SCAN=1 ;;
        --backup-only) BACKUP_ONLY=1 ;;
        --allow-unknown-replace) ALLOW_UNKNOWN_REPLACE=1 ;;
        --baseline) : ;;
        --baseline=*) BASELINE_ARG="${arg#--baseline=}" ;;
        PREFIX=*) PREFIX_ARG="${arg#PREFIX=}" ;;
        *) err "不认识的参数:$arg(可用:--scan / --backup-only / --baseline <目录> / --allow-unknown-replace / PREFIX=<目录>)" ;;
    esac
    prev="$arg"
done
if [ "$prev" = "--baseline" ]; then
    err "--baseline 需要一个目录参数(同版本可信原包解压后的目录)。"
fi
if [ -n "$BASELINE_ARG" ] && [ ! -d "$BASELINE_ARG" ]; then
    err "--baseline 指定的目录不存在:$BASELINE_ARG(zip/tar 请先解压成目录再给)。"
fi

if [ -n "$PREFIX_ARG" ] && [ -z "${PREFIX:-}" ]; then
    PREFIX="$PREFIX_ARG"
fi

# 脚本自身所在目录——POSIX 写法,不用 bash 专属的 BASH_SOURCE
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

SRC_EXE=""
if [ -x "$SCRIPT_DIR/$APP_NAME" ]; then
    SRC_EXE="$SCRIPT_DIR/$APP_NAME"
elif [ -x "$SCRIPT_DIR/$APP_NAME.exe" ]; then
    SRC_EXE="$SCRIPT_DIR/$APP_NAME.exe"
fi

if [ -z "$SRC_EXE" ]; then
    err "本地没找到 $APP_NAME 可执行文件(脚本同目录下应该有 $APP_NAME 或 $APP_NAME.exe)。请从 https://github.com/$REPO/releases 下载对应平台的发行包,再运行包内这份脚本。"
fi

info "找到可执行文件:$SRC_EXE"

# 目标文件名跟源文件名一致,自然处理 .exe 有无的情况
EXE_BASENAME=$(basename -- "$SRC_EXE")

# 确定安装目录
if [ -n "${PREFIX:-}" ]; then
    INSTALL_DIR="$PREFIX"
    info "使用指定安装目录(PREFIX):$INSTALL_DIR"
else
    LOCAL_BIN="$HOME/.local/bin"
    case ":${PATH}:" in
        *":${LOCAL_BIN}:"*)
            INSTALL_DIR="$LOCAL_BIN"
            info "检测到 $LOCAL_BIN 已经在 PATH 里,装这儿(不用 sudo)。"
            ;;
        *)
            INSTALL_DIR="/usr/local/bin"
            info "$LOCAL_BIN 不在 PATH 里,退到 $INSTALL_DIR。"
            ;;
    esac
fi

# 受管树落点:bin 布局把 skills/docs 放 ../share/lubancode,其余贴着 exe
if [ "$(basename -- "$INSTALL_DIR")" = "bin" ]; then
    SHARE_ROOT="$(dirname -- "$INSTALL_DIR")/share/lubancode"
    TREE_SKILLS="$SHARE_ROOT/skills"
    TREE_DOCS="$SHARE_ROOT/docs"
else
    TREE_SKILLS="$INSTALL_DIR/skills"
    TREE_DOCS="$INSTALL_DIR/docs"
fi
TREE_WEB="$INSTALL_DIR/web"
TREE_LIBEXEC="$INSTALL_DIR/libexec"
TREE_LICENSES="$INSTALL_DIR/licenses"

# 记录件(manifest.json / install-state.json)都落在可执行文件旁边
RECORD_DIR="$INSTALL_DIR"
STATE_FILE="$RECORD_DIR/install-state.json"

PLAN_PY="$SCRIPT_DIR/install_plan.py"
PY3=""
# 探针:Windows 的 python3.exe 常是商店假桩(退出码 49),先验真身再选用
for _pycand in python3 python; do
    if command -v "$_pycand" >/dev/null 2>&1 && "$_pycand" -c 'import sys' >/dev/null 2>&1; then
        PY3="$_pycand"
        break
    fi
done

# 调 install_plan.py:参数走位置参数,不经未加引号的命令替换,路径带空格也稳
run_plan() {
    mode=$1
    shift
    extra_baseline=""
    if [ -n "$BASELINE_ARG" ]; then
        extra_baseline="--baseline-dir"
    fi
    set -- "$mode" --source "$SCRIPT_DIR" --record-dir "$RECORD_DIR" \
        --map "skills=$TREE_SKILLS" --map "docs=$TREE_DOCS" --map "web=$TREE_WEB" \
        --map "libexec=$TREE_LIBEXEC" --map "licenses=$TREE_LICENSES" "$@"
    if [ -n "$extra_baseline" ]; then
        set -- "$@" --baseline-dir "$BASELINE_ARG"
    fi
    "$PY3" "$PLAN_PY" "$@"
}

# 已有受管内容?任一树目录非空、或 exe/记录件已在位,都算"装过"
has_existing_content() {
    [ -f "$INSTALL_DIR/$EXE_BASENAME" ] && return 0
    [ -f "$STATE_FILE" ] && return 0
    [ -f "$RECORD_DIR/manifest.json" ] && return 0
    for d in "$TREE_SKILLS" "$TREE_DOCS" "$TREE_WEB" "$TREE_LIBEXEC" "$TREE_LICENSES"; do
        if [ -d "$d" ] && [ -n "$(ls -A "$d" 2>/dev/null)" ]; then
            return 0
        fi
    done
    return 1
}

same_file() {
    [ "$1" = "$2" ]
}

detect_version() {
    "$1" --version 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i ~ /^[0-9]/) {print $i; exit}}' || true
}

# 纯 sh 记档:来源带官方清单就逐字节照抄;state 走极简字段。
# 下次升级的基线兜底读 manifest.json(install_plan.py / install.ps1 都认)。
write_minimal_records() {
    if [ -f "$SCRIPT_DIR/manifest.json" ] && ! same_file "$SCRIPT_DIR/manifest.json" "$RECORD_DIR/manifest.json"; then
        cp "$SCRIPT_DIR/manifest.json" "$RECORD_DIR/manifest.json"
    fi
    ver=$(detect_version "$INSTALL_DIR/$EXE_BASENAME" 2>/dev/null || true)
    [ -n "$ver" ] || ver=""
    now=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo "")
    {
        printf '{\n'
        printf '  "schema": 1,\n'
        printf '  "installed_at_utc": "%s",\n' "$now"
        printf '  "version": "%s",\n' "$ver"
        printf '  "install_mode": "fresh",\n'
        printf '  "manifest_provenance": "official-package",\n'
        printf '  "installer": "install.sh",\n'
        printf '  "manifest": null,\n'
        printf '  "pending_conflicts": []\n'
        printf '}\n'
    } > "$STATE_FILE"
}

# ---- 完整备份(纯 sh,不依赖 python) ----
full_backup_sh() {
    stamp=$(date -u '+%Y%m%dT%H%M%SZ' 2>/dev/null || date '+%Y%m%dT%H%M%SZ')
    backup_root="$RECORD_DIR/backups/$stamp-sh$$"
    mkdir -p "$backup_root"
    for spec in "skills=$TREE_SKILLS" "docs=$TREE_DOCS" "web=$TREE_WEB" "libexec=$TREE_LIBEXEC" "licenses=$TREE_LICENSES"; do
        tree=${spec%%=*}
        dir=${spec#*=}
        if [ -d "$dir" ] && [ -n "$(ls -A "$dir" 2>/dev/null)" ]; then
            mkdir -p "$backup_root/$tree"
            cp -R "$dir/." "$backup_root/$tree/"
        fi
    done
    for f in "$RECORD_DIR"/*; do
        [ -f "$f" ] || continue
        base=$(basename -- "$f")
        case "$base" in
            manifest.json|install-state.json) continue ;;
        esac
        cp "$f" "$backup_root/$base"
    done
    echo "$backup_root"
}

# ===================== --backup-only:完整备份,不安装 =====================
if [ "$BACKUP_ONLY" = 1 ]; then
    if [ ! -d "$INSTALL_DIR" ]; then
        info "安装目录不存在,无事可备份。"
        exit 0
    fi
    if ! has_existing_content; then
        info "安装目录没有受管内容,无需备份。"
        exit 0
    fi
    backup_root=$(full_backup_sh)
    info "完整备份完成:$backup_root"
    echo "[backup-only] 盘面文件清单:"
    for d in "$TREE_SKILLS" "$TREE_DOCS" "$TREE_WEB" "$TREE_LIBEXEC" "$TREE_LICENSES"; do
        [ -d "$d" ] && find "$d" -type f | sort
    done
    find "$RECORD_DIR" -maxdepth 1 -type f ! -name 'manifest.json' ! -name 'install-state.json' | sort
    exit 0
fi

# ===================== --scan:只读预演 =====================
if [ "$SCAN" = 1 ]; then
    if [ ! -d "$INSTALL_DIR" ] || ! has_existing_content; then
        count=$(find "$SCRIPT_DIR" -type f ! -name manifest.json | wc -l | tr -d ' ')
        info "全新安装预演:将从 $SCRIPT_DIR 拷入 $count 个文件到 $INSTALL_DIR(bin 布局时 skills/docs 落 ../share/lubancode)。"
        exit 0
    fi
    if [ -z "$PY3" ]; then
        err "已有内容的预演需要 python3(驱动 install_plan.py)。装 python3 后重试,或先用 --backup-only 备份再人工比对。"
    fi
    rc=0
    run_plan plan || rc=$?
    exit "$rc"
fi

# ===================== 正常安装 =====================

# 目录不存在就建;建不了多半是权限问题
if [ ! -d "$INSTALL_DIR" ]; then
    if ! mkdir -p "$INSTALL_DIR" 2>/dev/null; then
        err "创建安装目录 $INSTALL_DIR 失败,像是权限不够。试试:sudo PREFIX=$INSTALL_DIR sh $0"
    fi
fi

if [ ! -w "$INSTALL_DIR" ]; then
    err "$INSTALL_DIR 没有写权限。试试:sudo sh $0(或者 sudo env PREFIX=$INSTALL_DIR sh $0)"
fi

DEST="$INSTALL_DIR/$EXE_BASENAME"

if [ "$SRC_EXE" != "$DEST" ] && has_existing_content; then
    # ---- 已有内容:所有权引擎接管 ----
    if [ -z "$PY3" ]; then
        backup_root=$(full_backup_sh)
        err "安装目录已有内容且本机没有 python3,跑不了所有权判定。已先完整备份:$backup_root
处理建议:1) 装 python3 后重跑;2) 按备份人工迁移后清空安装目录再装。绝不整目录盲删。"
    fi
    rc=0
    if [ "$ALLOW_UNKNOWN_REPLACE" = 1 ]; then
        run_plan apply --allow-unknown-replace || rc=$?
    else
        run_plan apply || rc=$?
    fi
    [ "$rc" = 0 ] || exit "$rc"
elif [ "$SRC_EXE" != "$DEST" ]; then
    # ---- 全新安装:纯 sh 直拷(不依赖 python) ----
    if ! cp "$SRC_EXE" "$DEST"; then
        err "拷贝到 $DEST 失败。"
    fi
    chmod +x "$DEST" || err "chmod +x $DEST 失败。"

    sync_official_tree() {
        tree_name=$1
        display_name=$2
        destination=$3
        source_tree="$SCRIPT_DIR/$tree_name"

        if [ ! -d "$source_tree" ]; then
            info "安装包里没有 $tree_name 目录,保留已有$display_name 不动。"
            return
        fi

        parent=$(dirname -- "$destination")
        stage="$parent/.$tree_name-new-$$"
        if ! mkdir -p "$parent"; then
            err "创建$display_name 目录 $parent 失败。"
        fi
        rm -rf "$stage"
        if ! cp -R "$source_tree" "$stage"; then
            rm -rf "$stage"
            err "复制$display_name 失败。"
        fi
        rm -rf "$destination"
        if ! mv "$stage" "$destination"; then
            rm -rf "$stage"
            err "替换$display_name 目录 $destination 失败。"
        fi
        info "已同步$display_name:$destination"
    }

    sync_official_tree skills "官方技能" "$TREE_SKILLS"
    sync_official_tree docs "官方文档" "$TREE_DOCS"
    sync_official_tree web "助理网页" "$TREE_WEB"
    sync_official_tree libexec "随包 ripgrep(libexec)" "$TREE_LIBEXEC"
    sync_official_tree licenses "第三方许可证(licenses)" "$TREE_LICENSES"
    if [ -f "$TREE_LIBEXEC/rg" ]; then
        chmod +x "$TREE_LIBEXEC/rg" 2>/dev/null || true
    fi

    # 根级文件(EXE 上面已拷;这里:声明/许可证/README/脚本)
    for f in LICENSE THIRD_PARTY_NOTICES.md README.md README.en.md install.sh install_plan.py; do
        if [ -f "$SCRIPT_DIR/$f" ] && ! same_file "$SCRIPT_DIR/$f" "$RECORD_DIR/$f"; then
            cp "$SCRIPT_DIR/$f" "$RECORD_DIR/$f" || err "复制 $f 失败。"
        fi
    done

    # 记档:有 python 走完整记档(嵌入 manifest);没有走极简记档
    rc=0
    if [ -n "$PY3" ] && [ -f "$PLAN_PY" ]; then
        run_plan write-state --install-mode fresh || rc=$?
    fi
    [ "$rc" = 0 ] || write_minimal_records
else
    # ---- 来源与目标同一目录:文件已在位,不搬自己,只补记档 ----
    info "同目录安装,文件已在位,不重复搬动,只补记档。"
    if [ -n "$PY3" ] && [ -f "$PLAN_PY" ]; then
        rc=0
        run_plan write-state --install-mode same-dir || rc=$?
        [ "$rc" = 0 ] || write_minimal_records
    else
        write_minimal_records
    fi
fi

info "安装完成:$DEST"

if ! "$DEST" --version; then
    err "跑 $DEST --version 失败,安装可能有问题,自己检查一下。"
fi

case ":${PATH}:" in
    *":${INSTALL_DIR}:"*)
        info "$INSTALL_DIR 已经在 PATH 里,直接就能用 $APP_NAME 命令。"
        ;;
    *)
        echo "提示:$INSTALL_DIR 不在当前 PATH 里,把下面这行加进 shell 配置(~/.bashrc、~/.zshrc 之类),再开个新终端:"
        echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac
