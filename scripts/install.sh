#!/bin/sh
# lubancode 安装脚本(Linux / macOS,POSIX sh 兼容,不依赖 bash 专属语法)
#
# 用法:
#   ./install.sh                        自动挑安装目录(见下面优先级),脚本同目录找可执行文件
#   PREFIX=/opt/lubancode/bin ./install.sh   指定安装目录(注意 PREFIX 就是可执行文件要落地的那个目录本身)
#
# 安装目录优先级(没设 PREFIX 时):
#   1. $HOME/.local/bin 已经在当前 $PATH 里 → 装这儿,不需要 sudo
#   2. 否则退到 /usr/local/bin;有写权限就直接装,没有就提示用 sudo 重跑
#
# 源文件查找:
#   脚本同目录下找可执行文件,依次探测 lubancode 和 lubancode.exe(后者是为了兼容
#   Windows 下 Git Bash/WSL 场景——正式的 Linux/macOS 发行包里不会有 .exe 后缀,
#   这里只是不因为后缀而找不到文件)。目标文件名跟源文件名保持一致。
#   这份脚本随 Linux/macOS Release 包分发，只装同目录可执行文件；远程下载
#   由用户先在 Releases 页面选对平台包，脚本不在本机猜架构。
#
# 幂等:重复跑 = 直接覆盖安装。
# 官方 skills 与 docs 随包同行；重复安装时整包同步，不碰 ~/.lubancode/skills。
# 随包 ripgrep(ripgrep 迁移单 P0-6)三样也同步:libexec/、licenses/、
# THIRD_PARTY_NOTICES.md。libexec 永远装在可执行文件旁边(生产定位是
# ExecutableDir/libexec,不走 share),licenses 与声明跟着它,卸载只删
# LubanCode 自己目录里的,用户别处的 rg 不碰。

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

# 脚本自身所在目录——POSIX 写法,不用 bash 专属的 BASH_SOURCE
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

SRC_EXE=""
if [ -x "$SCRIPT_DIR/$APP_NAME" ]; then
    SRC_EXE="$SCRIPT_DIR/$APP_NAME"
elif [ -x "$SCRIPT_DIR/$APP_NAME.exe" ]; then
    SRC_EXE="$SCRIPT_DIR/$APP_NAME.exe"
fi

if [ -z "$SRC_EXE" ]; then
    err "本地没找到 $APP_NAME 可执行文件(脚本同目录下应该有 $APP_NAME 或 $APP_NAME.exe)。请从 https://github.com/$REPO/releases 下载对应平台的发行包，再运行包内这份脚本。"
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

if ! cp "$SRC_EXE" "$DEST"; then
    err "拷贝到 $DEST 失败。"
fi

if ! chmod +x "$DEST"; then
    err "chmod +x $DEST 失败。"
fi

# bin 目录按常见前缀布局把数据放到 ../share/lubancode/{skills,docs}；
# 自定义目录若不叫 bin，则把两棵资源树放在可执行文件旁。两处都保持
# skills/lubancode-config 到 docs 的相对路径是 ../../docs。
sync_official_tree() {
    tree_name=$1
    display_name=$2
    source_tree="$SCRIPT_DIR/$tree_name"

    if [ ! -d "$source_tree" ]; then
        info "安装包里没有 $tree_name 目录,保留已有$display_name 不动。"
        return
    fi

    if [ "$(basename -- "$INSTALL_DIR")" = "bin" ]; then
        destination="$(dirname -- "$INSTALL_DIR")/share/lubancode/$tree_name"
    else
        destination="$INSTALL_DIR/$tree_name"
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

sync_official_tree skills "官方技能"
sync_official_tree docs "官方文档"

# libexec/(随包 ripgrep)必须贴着可执行文件住:search 的生产定位只有一条
# ExecutableDir/libexec/rg,装去 share 它就找不着了。licenses/ 与
# THIRD_PARTY_NOTICES.md 同样贴着装(§8.1 平铺布局,卸载整目录一并带走)。
# 与 skills/docs 一样走 staging 原子换入:更新安装先整目录换新,再删旧,
# 不出现"旧 rg 删了、新 rg 没到"的半套窗口;rg 的执行位显式补一道。
sync_beside_exe_tree() {
    tree_name=$1
    display_name=$2
    source_tree="$SCRIPT_DIR/$tree_name"

    if [ ! -d "$source_tree" ]; then
        info "安装包里没有 $tree_name 目录,保留已有$display_name 不动。"
        return
    fi

    # 注意:不走 skills/docs 那条 share/ 分支——这两棵树永远在 exe 旁边。
    destination="$INSTALL_DIR/$tree_name"
    stage="$INSTALL_DIR/.$tree_name-new-$$"
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

sync_beside_exe_tree libexec "随包 ripgrep(libexec)"
if [ -f "$INSTALL_DIR/libexec/rg" ]; then
    chmod +x "$INSTALL_DIR/libexec/rg" 2>/dev/null || true
fi
sync_beside_exe_tree licenses "第三方许可证(licenses)"

notices_src="$SCRIPT_DIR/THIRD_PARTY_NOTICES.md"
if [ -f "$notices_src" ]; then
    notices_stage="$INSTALL_DIR/.THIRD_PARTY_NOTICES-new-$$"
    rm -f "$notices_stage"
    if ! cp "$notices_src" "$notices_stage"; then
        rm -f "$notices_stage"
        err "复制第三方声明失败。"
    fi
    if ! mv -f "$notices_stage" "$INSTALL_DIR/THIRD_PARTY_NOTICES.md"; then
        rm -f "$notices_stage"
        err "替换第三方声明失败。"
    fi
    info "已同步第三方声明:$INSTALL_DIR/THIRD_PARTY_NOTICES.md"
else
    info "安装包里没有 THIRD_PARTY_NOTICES.md,保留已有第三方声明不动。"
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
