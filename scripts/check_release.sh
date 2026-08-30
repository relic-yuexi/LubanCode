#!/usr/bin/env bash
# 核对源码版本、CHANGELOG 与可选 release tag。只读，不建 tag、不发包。

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
VERSION_HEADER="$REPO_ROOT/src/app/version.hpp"
CHANGELOG="$REPO_ROOT/CHANGELOG.md"
VCPKG_MANIFEST="$REPO_ROOT/vcpkg.json"

fail() {
    echo "release check failed: $1" >&2
    exit 1
}

version=$(sed -n 's/.*kVersion[[:space:]]*=[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' "$VERSION_HEADER")
line_count=$(printf '%s\n' "$version" | awk 'NF { count++ } END { print count + 0 }')
[ "$line_count" -eq 1 ] || fail "src/app/version.hpp 须恰有一枚 x.y.z 版本号"

grep -Fq "## [v$version] - " "$CHANGELOG" || \
    fail "CHANGELOG.md 缺 v$version 条目"

manifest_version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' "$VCPKG_MANIFEST")
[ "$manifest_version" = "$version" ] || \
    fail "vcpkg.json 版本 $manifest_version 与源码版本 $version 不合"

grep -Fq '"license": "Apache-2.0"' "$VCPKG_MANIFEST" || \
    fail "vcpkg.json 须声明 Apache-2.0"

tag=${1:-}
if [ -n "$tag" ] && [ "$tag" != "v$version" ]; then
    fail "tag $tag 与源码版本 v$version 不合"
fi

echo "release check passed: v$version${tag:+, tag=$tag}"
