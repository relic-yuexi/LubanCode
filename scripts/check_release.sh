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

# 随包 ripgrep 的供应链钉子(ripgrep 迁移单 P0-6):manifest 三平台哈希
# 全为真值、MIT license 原文在、第三方声明在且点到 ripgrep。Release 打包
# 前由 release.yml 调 fetch_ripgrep.py 再对一遍哈希;这里先把"仓库自己"
# 该有的件查齐——缺了就红,不发半套。
RG_MANIFEST="$REPO_ROOT/third_party/ripgrep/manifest.json"
RG_LICENSE="$REPO_ROOT/third_party/ripgrep/LICENSE-MIT"
NOTICES="$REPO_ROOT/THIRD_PARTY_NOTICES.md"

[ -f "$RG_MANIFEST" ] || fail "缺 $RG_MANIFEST(随包 ripgrep 的版本/哈希账)"
[ -f "$RG_LICENSE" ] || fail "缺 $RG_LICENSE(上游 MIT 原文,随包分发的法定件)"
[ -f "$NOTICES" ] || fail "缺 $NOTICES(第三方声明,Release 包必带)"

rg_sha_count=$(grep -c '"sha256"' "$RG_MANIFEST")
[ "$rg_sha_count" -ge 3 ] || fail "manifest.json 里三平台 sha256 不齐(实得 $rg_sha_count 条)"
grep -q 'IMPLEMENTATION_GATE_FILL_FROM_UPSTREAM' "$RG_MANIFEST" && \
    fail "manifest.json 里还有占位哈希,不许这样发布"

for platform in windows-x64 macos-arm64 linux-x64; do
    grep -q "\"$platform\"" "$RG_MANIFEST" || fail "manifest.json 缺 $platform 资产"
done
grep -q 'ripgrep' "$NOTICES" || fail "THIRD_PARTY_NOTICES.md 里没点到 ripgrep"
grep -q 'MIT' "$NOTICES" || fail "THIRD_PARTY_NOTICES.md 里没写 MIT 许可口径"

# 源码里钉死的版本常量与 manifest 对账(search_ripgrep.hpp 的
# kBundledRipgrepVersion;运行时 smoke 精确比对的就是它)。
rg_version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' "$RG_MANIFEST" | head -1)
code_version=$(sed -n 's/.*kBundledRipgrepVersion[[:space:]]*=[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' "$REPO_ROOT/src/tools/search_ripgrep.hpp")
[ -n "$rg_version" ] || fail "manifest.json 里读不出 ripgrep version"
[ -n "$code_version" ] || fail "search_ripgrep.hpp 里读不出 kBundledRipgrepVersion"
[ "$rg_version" = "$code_version" ] || \
    fail "ripgrep 版本不合: manifest=$rg_version 代码钉死=$code_version"

tag=${1:-}
if [ -n "$tag" ] && [ "$tag" != "v$version" ]; then
    fail "tag $tag 与源码版本 v$version 不合"
fi

echo "release check passed: v$version${tag:+, tag=$tag}(随包 ripgrep $rg_version 三平台哈希钉住)"
