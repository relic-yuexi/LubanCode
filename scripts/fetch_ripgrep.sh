#!/usr/bin/env bash
# fetch-ripgrep staging (manifest-pinned), pure shell edition.
# Replaces scripts/fetch_ripgrep.py by owner ruling 2026-09-02: no Python on
# the CI path — runner Pythons kept tripping (WindowsApps stub shadowing,
# charmap console vs CJK output). curl + sha256sum + unzip/tar/dpkg-deb are
# present on all three GitHub runners and in Git Bash.
#
# Contract is unchanged (see third_party/ripgrep/manifest.json notes):
#   - download (or reuse cache hit) the pinned archive for --platform,
#   - re-verify SHA-256 against the committed manifest,
#   - extract exactly ONE member (stdout pipe; no full unpack, no
#     path-traversal surface) into <target>/libexec/,
#   - copy the MIT license, run `rg --version` and pin the first line.
# Any mismatch -> non-zero exit. Nothing enters the repo/package unless green.
#
# Usage:
#   scripts/fetch_ripgrep.sh --platform KEY [--target DIR] [--cache DIR]
#   platforms: windows-x64 | macos-arm64 | linux-x64
#   --target 缺省时按 RuntimePaths 同一套语义自动认数据根(应用Worker接入
#   单 P1 遗留销账,冻结合同 docs/reference/capability-contract.md §13.1):
#     LUBANCODE_DATA_HOME(应用根在)
#     或 <LUBANCODE_HOME>/data(应用根在、数据根未设)
#     或 <home>/.lubancode(个人 CLI 旧布局,与 C++ 读口 search_ripgrep
#     的 UserStage 层逐字节同位);rg-stage 落 <数据根>/rg-stage。
#   坏值(空值/相对路径/根重叠/孤立 DATA_HOME)按启动门同口径明拒退出,
#   不静默回个人目录。--target 显式给定时本段完全不参与,旧调用面行为不变。
#
#   scripts/fetch_ripgrep.sh --print-target-root
#     只解析并打印 <数据根>/rg-stage,不下载、不碰 manifest——离线自检口,
#     ctest 的 scripts.fetch_ripgrep_paths 走这里断言解析矩阵。
set -euo pipefail

usage() { echo "usage: $0 --platform KEY [--target DIR] [--cache DIR] | --print-target-root" >&2; exit 2; }

TARGET="" PLATFORM="" CACHE="" PRINT_TARGET_ROOT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --target)  TARGET="$2";  shift 2 ;;
    --platform) PLATFORM="$2"; shift 2 ;;
    --cache)   CACHE="$2";   shift 2 ;;
    --print-target-root) PRINT_TARGET_ROOT=1; shift ;;
    *) usage ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="$SCRIPT_DIR/../third_party/ripgrep/manifest.json"

field() { # field <json-key-path-regex> -> trimmed value from manifest
  grep -oE "\"$1\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$MANIFEST" | head -1 | sed -E 's/.*"([^"]*)"$/\1/'
}

fail() { echo "fetch-ripgrep: $*" >&2; exit 1; }

# ---- 数据根解析(--target 缺省时参与;见文件头注释) ------------------------
# 纯 bash,不依赖外部命令:--print-target-root 走这条路,env -i 的干净
# 环境里也得能跑。

# 变量"设了"(含空串)与否——设了返回 0。set -u 下安全。
env_present() { [ -n "${!1+x}" ]; }

# 值归一:反斜杠折正斜杠(Windows env 常态),去尾斜杠(尾斜杠等值)。
norm_root() {
  local v="$1"
  v="${v//\\//}"
  while [ "$v" != "/" ] && [ "${v%/}" != "$v" ]; do v="${v%/}"; done
  printf '%s' "$v"
}

# 绝对路径判据(POSIX 根锚 / Windows 盘符;反斜杠已在 norm_root 折正)。
is_abs_root() {
  case "$1" in
    /*) return 0 ;;
    [A-Za-z]:/*) return 0 ;;
  esac
  return 1
}

# 解析并打印 <数据根>/rg-stage。坏值走 fail(启动门同口径,不回个人目录)。
resolve_stage_target() {
  local home_value="" home_root="" data_root=""
  if env_present LUBANCODE_HOME; then
    home_root="$(norm_root "${LUBANCODE_HOME-}")"
    [ -n "$home_root" ] || fail "LUBANCODE_HOME 是空值:配置错误(启动门同口径),不回个人目录"
    is_abs_root "$home_root" || fail "LUBANCODE_HOME 须绝对路径: ${LUBANCODE_HOME-}"
    if env_present LUBANCODE_DATA_HOME; then
      data_root="$(norm_root "${LUBANCODE_DATA_HOME-}")"
      [ -n "$data_root" ] || fail "LUBANCODE_DATA_HOME 是空值:配置错误(启动门同口径)"
      is_abs_root "$data_root" || fail "LUBANCODE_DATA_HOME 须绝对路径: ${LUBANCODE_DATA_HOME-}"
    else
      data_root="$home_root/data"
    fi
    [ "$data_root" != "$home_root" ] || fail "数据根与参数根相同:读写不分(启动门同口径拒启)"
    case "$home_root" in
      "$data_root"/*) fail "参数根落数据根之内:读写不分(启动门同口径拒启)" ;;
    esac
  elif env_present LUBANCODE_DATA_HOME; then
    fail "孤立的 LUBANCODE_DATA_HOME(无 LUBANCODE_HOME)是配置错误(启动门同口径拒启)"
  else
    # 个人 CLI 旧布局:数据根=<主目录>/.lubancode。Windows 认 USERPROFILE
    # (与 platform::HomeDir 同源;Git Bash 里 HOME/USERPROFILE 常同时在,
    # USERPROFILE 优先),POSIX 认 HOME;空值当未设落下一枚。
    home_value="${USERPROFILE:-${HOME:-}}"
    [ -n "$home_value" ] || fail "找不到主目录(USERPROFILE/HOME 均未设),个人布局无从落 rg-stage"
    home_root="$(norm_root "$home_value")"
    data_root="$home_root/.lubancode"
  fi
  printf '%s/rg-stage' "$data_root"
}

if [ "$PRINT_TARGET_ROOT" = 1 ]; then
  # 离线自检口:只解析不下载、不碰 manifest。
  resolve_stage_target
  exit 0
fi

[ -n "$PLATFORM" ] || usage
if [ -z "$TARGET" ]; then
  TARGET="$(resolve_stage_target)" || exit 1
  echo "target root (auto, RuntimePaths 语义): $TARGET"
fi

[ -f "$MANIFEST" ] || fail "manifest not found: $MANIFEST"

VERSION="$(field version)"
RELEASE_TAG="$(field release_tag)"
URL_TEMPLATE="$(field download_url_template)"
LICENSE_FILE="$(field license_file)"
# Asset block for the platform: take the substring after "assets.<platform>",
# then read fields inside it up to the next asset key.
ASSET_BLOCK="$(sed -nE "/\"$PLATFORM\"[[:space:]]*:/,/\"(windows-x64|macos-arm64|linux-x64)\"[[:space:]]*:/p" "$MANIFEST")"
asset_field() {
  printf '%s\n' "$ASSET_BLOCK" | grep -oE "\"$1\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -1 | sed -E 's/.*"([^"]*)"$/\1/'
}
ARCHIVE="$(asset_field archive)"
FORMAT="$(asset_field archive_format)"
SHA="$(asset_field sha256)"
MEMBER="$(asset_field member)"
OUTPUT="$(asset_field output)"
[ -n "$ARCHIVE" ] && [ -n "$SHA" ] && [ -n "$MEMBER" ] && [ -n "$OUTPUT" ] \
  || fail "manifest incomplete for platform $PLATFORM"
echo "$SHA" | grep -qE '^[0-9a-f]{64}$' || fail "sha256 not 64-hex: $SHA"

URL="${URL_TEMPLATE/\{release_tag\}/$RELEASE_TAG}"
URL="${URL/\{archive\}/$ARCHIVE}"

mkdir -p "$TARGET/libexec" "$TARGET/licenses"
ARCHIVE_PATH=""
if [ -n "$CACHE" ]; then
  CACHED="$CACHE/$ARCHIVE"
  if [ -f "$CACHED" ]; then
    echo "cache hit: $CACHED"
    ARCHIVE_PATH="$CACHED"
  fi
fi
if [ -z "$ARCHIVE_PATH" ]; then
  TMP="$(mktemp)"
  trap 'rm -f "$TMP"' EXIT
  echo "download: $URL"
  curl -fsSL --retry 3 -o "$TMP" "$URL"
  ARCHIVE_PATH="$TMP"
fi

ACTUAL="$(sha256sum "$ARCHIVE_PATH" | awk '{print $1}')"
[ "$ACTUAL" = "$SHA" ] || fail "sha256 mismatch
  manifest: $SHA
  actual:   $ACTUAL"
echo "sha256 ok"

EXE="$TARGET/libexec/$OUTPUT"
case "$FORMAT" in
  zip)
    # Single member to stdout; no traversal surface.
    unzip -p "$ARCHIVE_PATH" "$MEMBER" > "$EXE"
    ;;
  tar.gz)
    tar -xzOf "$ARCHIVE_PATH" "$MEMBER" > "$EXE"
    ;;
  deb)
    # data.tar member inside the ar archive; member paths carry ./ prefix.
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK" "$TMP"' EXIT
    if command -v dpkg-deb >/dev/null 2>&1; then
      dpkg-deb -x "$ARCHIVE_PATH" "$WORK/root"
      cp "$WORK/root/$MEMBER" "$EXE"
    elif command -v ar >/dev/null 2>&1; then
      # RPM 系(如 manylinux_2_28 容器)无 dpkg-deb:.deb 是 ar 档,内含
      # data.tar.*(tar 自动识别压缩)。抽整档再拷成员,与 dpkg-deb 路同效。
      (cd "$WORK" && ar x "$ARCHIVE_PATH")
      data_tar=""
      for f in "$WORK"/data.tar.*; do data_tar="$f"; break; done
      [ -n "$data_tar" ] || fail "no data.tar.* inside deb (ar fallback)"
      mkdir -p "$WORK/root"
      tar -xf "$data_tar" -C "$WORK/root"
      cp "$WORK/root/$MEMBER" "$EXE"
    else
      fail "deb extraction needs dpkg-deb or ar (linux only)"
    fi
    ;;
  *) fail "unknown archive_format: $FORMAT" ;;
esac
[ -s "$EXE" ] || fail "extracted member is empty: $MEMBER"

LICENSE_SRC="$SCRIPT_DIR/../third_party/ripgrep/$LICENSE_FILE"
[ -f "$LICENSE_SRC" ] && cp "$LICENSE_SRC" "$TARGET/licenses/ripgrep-MIT.txt"

case "$OUTPUT" in
  *.exe) ;;
  *) chmod +x "$EXE" ;;
esac

FIRST_LINE="$("$EXE" --version 2>/dev/null | head -1)"
echo "$FIRST_LINE" | grep -qF "ripgrep $VERSION" \
  || fail "version check failed: want 'ripgrep $VERSION', got '$FIRST_LINE'"
echo "version ok: $FIRST_LINE"
echo "staged: $EXE"
