#!/usr/bin/env bash
# WS 承载冒烟的壳(多前端外壳单阶段 A 的验收单):找构建产物,跑独立
# WS 协议客户端。依赖:node、构建好的 lubancode(浏览器面等价集另要
# browser/ 的 npm install,缺了自己打 SKIP,不冒充通过)。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

binary="${LUBANCODE_BINARY:-}"
if [[ -z "$binary" ]]; then
  for candidate in "$repo/build/debug/lubancode.exe" "$repo/build/lubancode.exe" \
                   "$repo/build/lubancode" "$repo/build/Debug/lubancode.exe" \
                   "$repo/build/Release/lubancode.exe"; do
    if [[ -e "$candidate" ]]; then
      binary="$candidate"
      break
    fi
  done
fi

node "$here/app_server_ws_client.js" ${binary:+--binary "$binary"}
