#!/usr/bin/env bash
# 问题 9 冒烟(真实实测问题单):假 Responses 后端 + 管道模式,逐段断言
# /context 面板的 miss 分型与 /doctor cache probe 的回环直跑。
#
# 三段命中序列(各起一场会话,互不污染):
#   all-hit      全命中      -> 面板应有"命中"账、无"上游未命中"
#   all-miss     全 miss     -> 面板应明写"上游未命中"(本地前缀稳定)
#   alternate    交替命中    -> 同 epoch 内命中/上游未命中交替可见
# 另补一段 mixed-silent(奇数笔命中、偶数笔缺测),面板应写"未回报"。
# 探针:/doctor cache probe 4 对回环端直发(无确认门),应出分型"稳定命中"。
#
# 依赖:python3、构建好的 lubancode(找 build/debug/lubancode.exe 等)。
# 临时 USERPROFILE 隔离会话存档;不落任何密钥与请求正文。
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

binary="${LUBANCODE_BINARY:-}"
if [[ -z "$binary" ]]; then
  for candidate in "$repo/build/debug/Debug/lubancode.exe" "$repo/build/debug/lubancode.exe" \
                   "$repo/build/lubancode.exe" "$repo/build/debug/lubancode" \
                   "$repo/build/Release/lubancode.exe"; do
    if [[ -e "$candidate" ]]; then binary="$candidate"; break; fi
  done
fi
if [[ -z "$binary" || ! -e "$binary" ]]; then
  echo "SKIP: 找不到构建产物 lubancode(.exe),先 cmake --build --preset debug" >&2
  exit 2
fi

python="${PYTHON:-python}"
# Windows 的 python3 常是商店空壳(存在但不出活),默认用 python;
# 实在没有再退 python3 / py。
command -v "$python" >/dev/null 2>&1 || python=python3
command -v "$python" >/dev/null 2>&1 || python=py

# 起假后端(固定回环端口段里挑一只)。
port="${FAKE_BACKEND_PORT:-8737}"
backend_log="$(mktemp -t fake_backend.XXXXXX.log)"
"$python" "$here/fake_responses_backend.py" --port "$port" --script all-hit >"$backend_log" 2>&1 &
backend_pid=$!
trap 'kill "$backend_pid" 2>/dev/null; rm -f "$backend_log"' EXIT

for _ in $(seq 1 50); do
  if grep -q FAKE_BACKEND_READY "$backend_log" 2>/dev/null; then break; fi
  sleep 0.2
done
if ! grep -q FAKE_BACKEND_READY "$backend_log" 2>/dev/null; then
  echo "FAIL: 假后端没起来" >&2
  cat "$backend_log" >&2
  exit 1
fi
echo "== 假后端就绪: 127.0.0.1:$port(剧本切换靠重启,见 run_session)"

# 一场管道会话:三句用户输入(三次模型请求)+ /context + 退出。
# 注意剧本在会话内不可换(后端进程级),每段各起一场、各重启一次后端。
run_session() {
  local script="$1"; shift
  local port="$1"; shift
  local userprofile="$1"; shift
  kill "$backend_pid" 2>/dev/null
  wait "$backend_pid" 2>/dev/null
  backend_log="$(mktemp -t fake_backend.XXXXXX.log)"
  "$python" "$here/fake_responses_backend.py" --port "$port" --script "$script" >"$backend_log" 2>&1 &
  backend_pid=$!
  for _ in $(seq 1 50); do
    grep -q FAKE_BACKEND_READY "$backend_log" 2>/dev/null && break
    sleep 0.2
  done
  printf '第一句\n第二句\n第三句\n/context\nexit\n' | \
    USERPROFILE="$userprofile" \
    LUBANCODE_WIRE=responses \
    LUBANCODE_BASE_URL="http://127.0.0.1:$port" \
    LUBANCODE_API_KEY=smoke-dummy \
    LUBANCODE_MODEL=fake-model \
    LUBANCODE_THEME=plain \
    "$binary" 2>/dev/null
}

fresh_profile() {
  local dir="$(mktemp -d -t luban_smoke_profile.XXXXXX)"
  # Windows 的 Git Bash 里 USERPROFILE 要 Windows 路径。
  cygpath -w "$dir" 2>/dev/null || echo "$dir"
}

fail=0
expect() { # expect <说明> <产物> <须含>
  if grep -q "$3" "$2"; then
    echo "  ok: $1"
  else
    echo "  FAIL: $1(找不着: $3)" >&2
    fail=1
  fi
}
expect_absent() { # expect_absent <说明> <产物> <须不含>
  if grep -q "$3" "$2"; then
    echo "  FAIL: $1(不该出现: $3)" >&2
    fail=1
  else
    echo "  ok: $1"
  fi
}

echo "== 段一: all-hit(全命中)"
out1="$(mktemp -t smoke_allhit.XXXXXX.out)"
run_session all-hit "$port" "$(fresh_profile)" >"$out1" 2>&1
expect "面板有逐请求缓存块" "$out1" "逐请求缓存命中"
expect "面板有追加律诊断" "$out1" "前缀追加"
expect "面板有稳定前缀条数" "$out1" "稳定前缀"
expect "分型小计有命中账" "$out1" "窗口内分型"
expect_absent "全命中不该点破上游未命中" "$out1" "上游未命中"
expect_absent "全命中不该有未回报" "$out1" "另记缺测"

echo "== 段二: all-miss(本地前缀稳定、上游报零 -> 明写上游未命中)"
out2="$(mktemp -t smoke_allmiss.XXXXXX.out)"
run_session all-miss "$port" "$(fresh_profile)" >"$out2" 2>&1
expect "面板明写上游未命中" "$out2" "上游未命中"
expect "追加律仍是前缀追加(锅不在本地)" "$out2" "前缀追加"
expect "分型小计把上游未命中记上" "$out2" "上游未命中(本地前缀稳定) 2"
expect_absent "本地没断不该写前缀断" "$out2" "前缀断"

echo "== 段三: alternate(交替 -> 同 epoch 内两类并见)"
out3="$(mktemp -t smoke_alt.XXXXXX.out)"
run_session alternate "$port" "$(fresh_profile)" >"$out3" 2>&1
expect "交替场景追加律仍成立" "$out3" "前缀追加"
expect "交替场景也见上游未命中" "$out3" "上游未命中"
expect "交替场景 epoch 没断" "$out3" "epoch 1"
expect_absent "交替场景不该断 epoch" "$out3" "断:"

echo "== 段四: mixed-silent(缺测 -> 未回报,不冒充 0%)"
out4="$(mktemp -t smoke_unrep.XXXXXX.out)"
run_session mixed-silent "$port" "$(fresh_profile)" >"$out4" 2>&1
expect "缺测行明写未回报" "$out4" "另记缺测"
expect "分型小计把未回报记上" "$out4" "未回报 1"

echo "== 段五: /doctor cache probe 4(回环端直发,无确认门)"
out5="$(mktemp -t smoke_probe.XXXXXX.out)"
kill "$backend_pid" 2>/dev/null; wait "$backend_pid" 2>/dev/null
backend_log="$(mktemp -t fake_backend.XXXXXX.log)"
"$python" "$here/fake_responses_backend.py" --port "$port" --script all-hit >"$backend_log" 2>&1 &
backend_pid=$!
for _ in $(seq 1 50); do grep -q FAKE_BACKEND_READY "$backend_log" 2>/dev/null && break; sleep 0.2; done
printf '/doctor cache probe 4\nexit\n' | \
  USERPROFILE="$(fresh_profile)" \
  LUBANCODE_WIRE=responses \
  LUBANCODE_BASE_URL="http://127.0.0.1:$port" \
  LUBANCODE_API_KEY=smoke-dummy \
  LUBANCODE_MODEL=fake-model \
  LUBANCODE_THEME=plain \
  "$binary" >"$out5" 2>&1
expect "探针发了 4 轮" "$out5" "第 4 轮"
expect "探针报了 usage 命中" "$out5" "cached_tokens 命中"
expect "探针报了公共前缀字节" "$out5" "公共前缀"
expect "探针出了分型" "$out5" "分型"
expect "首轮 miss 后续全命中,分型应为稳定命中" "$out5" "稳定命中"
expect_absent "回环端不该问确认" "$out5" "确认发送"

if [[ "$fail" -eq 0 ]]; then
  echo "== 冒烟全绿"
else
  echo "== 冒烟有断言未过,产物在上面各 mktemp 文件" >&2
fi
exit "$fail"
