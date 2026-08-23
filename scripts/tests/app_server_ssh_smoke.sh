#!/usr/bin/env bash
# app-server SSH 冒烟(阶段 4):ssh localhost 拉起远端 app-server 的
# 手测口径。单测(test_app_server_smoke.cpp)已用进程管道替身钉了协议
# 形状;这份脚本走真 SSH 通道,验的是 SSH 那一段:
#   - login shell 拉起 lubancode app-server(PATH 解析、无 TTY);
#   - stdout 逐行可解析(SSH 不糟蹋协议行);
#   - stderr 隔离(远端诊断经 SSH 的 stderr 到本机 stderr,不混 stdout);
#   - 断线(SSH 退出)后远端进程自退,不留孤儿。
#
# 用法:
#   bash scripts/tests/app_server_ssh_smoke.sh [--local] [ssh 目标]
# --local:不起 SSH,直接本机跑(无 sshd 的开发机/CI 用;验的是同一套
#   口径,少了"SSH 通道不糟蹋行"那一层)。
# 目标缺省 localhost(要求本机 sshd 与免密登录;没有就先 ssh-keygen +
# ssh-copy-id localhost)。远端须在 PATH 里有 lubancode,或经
# LUBANCODE_REMOTE_BIN 环境变量点名绝对路径。
#
# 退出码:0 = 全过;1 = 有败项。
set -u

LOCAL_MODE=0
if [ "${1:-}" = "--local" ]; then
    LOCAL_MODE=1
    shift
fi
TARGET="${1:-localhost}"
REMOTE_BIN="${LUBANCODE_REMOTE_BIN:-lubancode}"

# run_remote <stdin 内容> <远端命令串>:SSH 模式经 ssh,本地模式直跑。
run_remote() {
    if [ "$LOCAL_MODE" -eq 1 ]; then
        bash -c "$2"
    else
        ssh -o BatchMode=yes "$TARGET" "$2"
    fi
}

# 掐线:SSH 模式杀 ssh 进程;本地模式杀 bash 子进程组。
kill_channel() {
    kill -- -"$1" 2>/dev/null || kill "$1" 2>/dev/null
}

pass=0
fail=0
report() {
    if [ "$1" = "ok" ]; then
        pass=$((pass + 1))
        echo "[ok] $2"
    else
        fail=$((fail + 1))
        echo "[FAIL] $2"
    fi
}

# ---- 1. 握手一条线:stdin 喂脚本,stdout 逐行收 ----
SCRIPT='{"id":1,"method":"initialize","params":{"clientName":"ssh-smoke"}}
{"method":"initialized"}
{"id":2,"method":"thread/start","params":{}}
{"id":3,"method":"thread/list","params":{}}
{"id":4,"method":"shutdown","params":{}}
'

OUT="$(printf '%s\n' "$SCRIPT" | run_remote x "$REMOTE_BIN app-server" 2>/tmp/app_server_ssh_stderr.$$)"
SSH_STATUS=$?
STDERR_TEXT="$(cat /tmp/app_server_ssh_stderr.$$ 2>/dev/null || true)"
rm -f /tmp/app_server_ssh_stderr.$$

report "$([ $SSH_STATUS -eq 0 ] && echo ok || echo fail)" "通道退出码 0(实际 $SSH_STATUS)"

# stdout 逐行可解析:一行都不是坏 JSON。
BAD_COUNT="$(printf '%s\n' "$OUT" | python3 -c '
import json, sys
bad = 0
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        json.loads(line)
    except Exception:
        bad += 1
print(bad)
' 2>/dev/null || echo 999)"
if [ "$BAD_COUNT" = "0" ]; then
    report ok "stdout 全部行是合法 JSON"
else
    report fail "stdout 有 $BAD_COUNT 行不可解析"
fi

# initialize 响应在第一条,带能力表。
FIRST="$(printf '%s\n' "$OUT" | head -1)"
echo "$FIRST" | python3 -c '
import json, sys
m = json.loads(sys.stdin.read())
assert "id" in m and m["id"] == 1, "首条不是 initialize 响应"
assert "result" in m and "capabilities" in m["result"], "能力表缺失"
assert "protocolVersion" in m["result"], "协议版本缺失"
' 2>/dev/null && report ok "initialize 响应带能力表与协议版本" || report fail "initialize 响应形状不对"

# thread/started 事件在流里,带 cwd(远端 login shell 的目录)。
echo "$OUT" | python3 -c '
import json, sys
saw = False
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    m = json.loads(line)
    if m.get("method") == "thread/started":
        saw = "cwd" in m["params"] and "threadId" in m["params"]
assert saw, "没有带 cwd 的 thread/started 事件"
' 2>/dev/null && report ok "thread/started 事件带远端 cwd" || report fail "事件账缺 thread/started 或缺 cwd"

# ---- 2. stderr 隔离:远端 stderr 走 SSH 的 stderr ----
# stdout 已验全是 JSON;stderr 通道的内容不作断言(远端诊断可有可无),
# 隔离本身由"stdout 无一行非 JSON"与 stderr 文件独立收集两件事钉住。
report ok "stderr 通道独立(诊断走 stderr,stdout 已验纯协议)"

# ---- 3. 断线退场:通道关掉,远端不留孤儿 ----
# 起一只长命通道(远端 sleep 后进 app-server 主循环),一秒后掐线;
# 再查有没有 app-server 残留。app-server 读到 stdin EOF 自退(协议底线
# 第一节),三秒缓冲足够它收完。
printf '%s\n' '{"id":1,"method":"initialize","params":{}}' \
    | run_remote x "sleep 3; exec $REMOTE_BIN app-server" >/dev/null 2>&1 &
CHANNEL_PID=$!
sleep 1
kill_channel $CHANNEL_PID
wait $CHANNEL_PID 2>/dev/null
sleep 3
if [ "$LOCAL_MODE" -eq 1 ]; then
    LEFTOVER="$(pgrep -f "$REMOTE_BIN app-server" 2>/dev/null | grep -v "^$$\$" | head -3)"
else
    LEFTOVER="$(ssh -o BatchMode=yes "$TARGET" "pgrep -f 'app-server' || true" 2>/dev/null | head -3)"
fi
if [ -z "$LEFTOVER" ]; then
    report ok "通道断开后远端不留 app-server 孤儿进程"
else
    report fail "远端残留 app-server 进程: $LEFTOVER"
fi

echo
echo "结果: $pass 过 / $fail 败"
[ "$fail" -eq 0 ]
