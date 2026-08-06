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
# 官方 skills 随包同行；重复安装时整包同步，不碰 ~/.lubancode/skills。

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

# bin 目录按常见前缀布局把数据放到 ../share/lubancode/skills；自定义目录
# 若不叫 bin，则把 skills 放在可执行文件旁，程序两处都认。
if [ -d "$SCRIPT_DIR/skills" ]; then
    if [ "$(basename -- "$INSTALL_DIR")" = "bin" ]; then
        SKILLS_DEST="$(dirname -- "$INSTALL_DIR")/share/lubancode/skills"
    else
        SKILLS_DEST="$INSTALL_DIR/skills"
    fi
    SKILLS_PARENT=$(dirname -- "$SKILLS_DEST")
    SKILLS_STAGE="$SKILLS_PARENT/.skills-new-$$"
    if ! mkdir -p "$SKILLS_PARENT"; then
        err "创建官方技能目录 $SKILLS_PARENT 失败。"
    fi
    rm -rf "$SKILLS_STAGE"
    if ! cp -R "$SCRIPT_DIR/skills" "$SKILLS_STAGE"; then
        rm -rf "$SKILLS_STAGE"
        err "复制官方技能失败。"
    fi
    rm -rf "$SKILLS_DEST"
    if ! mv "$SKILLS_STAGE" "$SKILLS_DEST"; then
        rm -rf "$SKILLS_STAGE"
        err "替换官方技能目录 $SKILLS_DEST 失败。"
    fi
    info "已同步官方技能:$SKILLS_DEST"
else
    info "安装包里没有 skills 目录,保留已有官方技能不动。"
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
