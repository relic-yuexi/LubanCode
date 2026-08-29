#!/usr/bin/env bash
# 浏览器协议冒烟的壳(阶段 3 验收):找构建产物,跑独立协议客户端。
# 依赖:node、browser/ 的 npm install(playwright)、构建好的 lubancode。
# 缺哪项客户端自己打 SKIP 退 0,不冒充通过。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

binary="${LUBANCODE_BINARY:-}"
if [[ -z "$binary" ]]; then
  for candidate in "$repo/build/lubancode.exe" "$repo/build/lubancode" \
                   "$repo/build/Debug/lubancode.exe" "$repo/build/Release/lubancode.exe"; do
    if [[ -e "$candidate" ]]; then
      binary="$candidate"
      break
    fi
  done
fi

node "$here/app_server_browser_client.js" ${binary:+--binary "$binary"}
