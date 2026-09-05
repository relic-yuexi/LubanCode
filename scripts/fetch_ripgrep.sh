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
#   scripts/fetch_ripgrep.sh --target DIR --platform KEY [--cache DIR]
#   platforms: windows-x64 | macos-arm64 | linux-x64
set -euo pipefail

usage() { echo "usage: $0 --target DIR --platform KEY [--cache DIR]" >&2; exit 2; }

TARGET="" PLATFORM="" CACHE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --target)  TARGET="$2";  shift 2 ;;
    --platform) PLATFORM="$2"; shift 2 ;;
    --cache)   CACHE="$2";   shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$TARGET" ] && [ -n "$PLATFORM" ] || usage

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="$SCRIPT_DIR/../third_party/ripgrep/manifest.json"

field() { # field <json-key-path-regex> -> trimmed value from manifest
  grep -oE "\"$1\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$MANIFEST" | head -1 | sed -E 's/.*"([^"]*)"$/\1/'
}

fail() { echo "fetch-ripgrep: $*" >&2; exit 1; }

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
