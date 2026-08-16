// bootstrap_py.hpp 的两条嵌入源码。改这里 = 换 harness_revision(ptc-v1),
// 画像指纹随之变,旧 verified 降回 unknown。

#include "ptc/bootstrap_py.hpp"

namespace lubancode::ptc {

const char* kPtcRuntimePython = R"PTC(# -*- coding: utf-8 -*-
"""PTC runtime: framed RPC channel + guarded environment (LubanCode host side protocol v1)."""
import io
import json
import sys
import threading
import traceback

PROTOCOL_VERSION = 1

_channel_lock = threading.Lock()
_next_id = [1]
_results = {}
_events = {}
_abort_reason = [None]
_emitted = [False]
_calls_made = [0]
_frame_out = sys.stdout.buffer
_captured = io.StringIO()

# import 白名单:纯计算标准库 + 宿主生成的两个模块。名单外一律拒。
# sys 放行:解释器与 asyncio 的传递导入都离不开它;护栏防的是文件系统/
# 网络/进程,不是 sys 本身(脚本 sys.exit 自了断算脚本自己的失败)。
ALLOWED_MODULES = {
    "json", "math", "statistics", "re", "itertools", "functools", "collections",
    "datetime", "time", "decimal", "fractions", "random", "string", "textwrap",
    "unicodedata", "heapq", "bisect", "array", "typing", "enum", "abc",
    "operator", "asyncio", "sys", "io", "codecs", "encodings", "_thread",
    "threading", "selectors", "concurrent", "contextlib", "warnings",
    "luban_tools", "ptc_runtime",
}

# 名单内但含敏感面的模块,显式钉死禁用,防白名单手滑。
DENY_ALWAYS = {"os", "socket", "ssl", "subprocess", "multiprocessing", "ctypes",
               "pathlib", "shutil", "glob", "tempfile", "pickle", "importlib", "urllib",
               "http", "ftplib", "smtplib", "telnetlib", "posix", "nt", "_winapi",
               "winreg", "signal", "resource"}


class ToolCallError(Exception):
    """stub 调用失败:工具层错误(文件不存在等)或被拒(权限/hooks/限额)。"""

    def __init__(self, tool, message):
        super().__init__("%s: %s" % (tool, message))
        self.tool = tool
        self.message = message


class ToolResult(dict):
    """stub 成功结果(dict 子类)。支持 await:已同步完成,await 立即返回自身,
    asyncio.gather 可直接收拢(调用在求值时已完成,是串行 RPC)。

    __hash__ 显式给回 object 的按身份哈希:dict 子类默认不可哈希,而
    asyncio.gather 拿入参当字典键去重,不可哈希直接 TypeError。按身份
    哈希 + 字典相等虽不满足"相等即同哈希"的约定,但 gather 那个去重本来
    就不该把两枚不同调用折叠成一枚——不折叠正是我们要的。
    """

    __hash__ = object.__hash__

    def __await__(self):
        if False:  # pragma: no cover —— 让本函数成为 generator
            yield
        return self


def _send(payload):
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    with _channel_lock:
        _frame_out.write(len(data).to_bytes(4, "little"))
        _frame_out.write(data)
        _frame_out.flush()


def _fail(stage, error, tb=""):
    try:
        _send({"type": "fail", "stage": stage, "error": error, "traceback": tb[-4000:]})
    except Exception:
        pass


def _read_exact(n):
    buf = b""
    # 走 raw(无缓冲锁):daemon 读线程若在收尾时还堵在缓冲读上,解释器
    # 关闭阶段抢不到 BufferedReader 的锁,会以 Fatal Python error 收场。
    stdin = sys.stdin.buffer.raw
    while len(buf) < n:
        chunk = stdin.read(n - len(buf))
        if not chunk:
            raise EOFError("host closed stdin")
        buf += chunk
    return buf


def _close_stdin_raw():
    """收尾时叫醒堵在读上的 reader 线程:关掉原始 stdin,读到立刻失败退出。"""
    try:
        sys.stdin.buffer.raw.close()
    except Exception:
        pass


def _reader_main():
    try:
        while True:
            header = _read_exact(4)
            length = int.from_bytes(header, "little")
            if length > 32 * 1024 * 1024:
                raise ValueError("frame too large: %d" % length)
            payload = json.loads(_read_exact(length).decode("utf-8"))
            kind = payload.get("type")
            if kind == "result":
                cid = payload.get("id")
                with _channel_lock:
                    _results[cid] = payload
                    ev = _events.get(cid)
                if ev is not None:
                    ev.set()
            elif kind == "abort":
                _abort_reason[0] = payload.get("reason", "aborted by host")
                with _channel_lock:
                    events = list(_events.values())
                for ev in events:
                    ev.set()
            else:
                raise ValueError("unknown host frame type: %r" % kind)
    except EOFError:
        return
    except Exception as exc:  # noqa: BLE001 —— 协议错也要叫醒所有等待者
        _abort_reason[0] = "rpc protocol error: %s" % exc
        with _channel_lock:
            events = list(_events.values())
        for ev in events:
            ev.set()


def _call_tool(tool, payload):
    # 注意:不在这里提前看 abort——取消语义由宿主裁定,每一枚未开始的调用
    # 都要送进宿主记账后回绝(审计账上"取消(未开始)"),脚本侧自行收口。
    with _channel_lock:
        cid = _next_id[0]
        _next_id[0] += 1
        ev = threading.Event()
        _events[cid] = ev
    _calls_made[0] += 1
    _send({"type": "call", "id": cid, "tool": tool, "input": payload})
    ev.wait()
    with _channel_lock:
        _events.pop(cid, None)
        result = _results.pop(cid, None)
    if _abort_reason[0] is not None and result is None:
        raise ToolCallError(tool, "aborted: %s" % _abort_reason[0])
    if result is None:
        raise ToolCallError(tool, "rpc: no result for call %d" % cid)
    if not result.get("ok"):
        raise ToolCallError(tool, result.get("error", "rejected by host"))
    value = result.get("value", {})
    if isinstance(value, dict) and value.get("is_error"):
        raise ToolCallError(tool, str(value.get("content", "tool error")))
    return ToolResult(value)


def call_tool(tool, payload):
    """stub 的公共入口;luban_tools 里的每个函数都落到这里。"""
    return _call_tool(tool, payload)


def parallel(thunks):
    """并发发起一批 stub 调用:每个元素是零参 callable(如 lambda: read_file(...))。

    与 asyncio.gather 的差别:gather 收拢的调用在求值时已同步完成(串行 RPC),
    parallel 用线程同时把请求发上线——宿主按到达顺序串行执行,但在飞窗口
    真的会超过 1,受 ptc_max_concurrency 约束(超窗的调用会被拒,收
    ToolCallError)。返回结果列表,次序与输入一致;任一失败抛第一个异常。
    """
    results = [None] * len(thunks)
    errors = [None] * len(thunks)

    def worker(index, fn):
        try:
            results[index] = fn()
        except BaseException as exc:  # noqa: BLE001 —— 原样带回第一个异常
            errors[index] = exc

    threads = [threading.Thread(target=worker, args=(i, fn)) for i, fn in enumerate(thunks)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for exc in errors:
        if exc is not None:
            raise exc
    return results


def emit(value):
    """脚本的最终摘要:整段脚本收完后,这份值(精简过)送回模型。只许一次。"""
    if _emitted[0]:
        raise RuntimeError("emit() 只许调用一次")
    _emitted[0] = True
    _send({"type": "emit", "value": value})


class _ImportGuard:
    """sys.meta_path 前哨:白名单外一律 ImportError。防君子栏,不是保险箱。"""

    def find_spec(self, fullname, path=None, target=None):
        top = fullname.split(".")[0]
        if top in DENY_ALWAYS:
            raise ImportError("PTC sandbox: import %r is denied" % fullname)
        if top in ALLOWED_MODULES:
            return None  # 交给默认机制(标准库/磁盘上的 luban_tools)
        raise ImportError("PTC sandbox: import %r is not in the allowlist" % fullname)


def install_guard():
    """装护栏:清洗 sys.modules、插 import 前哨、封 open/input、捕获 stdout。

    敏感模块(os/socket/_winapi 这些)即便已被预热进缓存也照样删名——
    asyncio 一类模块在 import 时绑定的引用不受影响,脚本再 `import os`
    时走 meta_path 前哨,照样被拒。护栏只认名单,不认"谁先来过"。
    """
    for name in list(sys.modules):
        top = name.split(".")[0]
        if top in DENY_ALWAYS:
            del sys.modules[name]
    sys.meta_path.insert(0, _ImportGuard())
    import builtins

    builtins.open = None  # type: ignore[assignment]
    if hasattr(builtins, "input"):
        builtins.input = None  # type: ignore[assignment]
    sys.stdout = _captured
    sys.stderr = _captured
    return _captured
)PTC";

const char* kPtcMainPython = R"PTC(# -*- coding: utf-8 -*-
"""PTC 入口:读脚本 -> 握手 -> 上护栏 -> 跑脚本 -> emit/done/fail。"""
import sys

# -I 隔离模式不把脚本目录挂进 sys.path,显式补上(护栏装好后这里的引用
# 早已拿稳,不受清洗影响)。
_here = __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0]
sys.path.insert(0, _here)

import ptc_runtime


def _read_script():
    path = _here + "/ptc_script.py"
    with open(path, "r", encoding="utf-8") as handle:  # 护栏装好前读入
        return handle.read()


def main():
    try:
        script = _read_script()
    except Exception as exc:  # noqa: BLE001
        ptc_runtime._fail("runtime", "cannot read ptc_script.py: %s" % exc)
        return 1
    ptc_runtime._send({"type": "hello", "protocol": ptc_runtime.PROTOCOL_VERSION,
                       "python": sys.version.split()[0]})
    reader = ptc_runtime.threading.Thread(target=ptc_runtime._reader_main, daemon=True)
    reader.start()
    # asyncio 预热:它的传递导入(socket/_winapi...)在护栏下会被拒,先装好
    # 缓存再上护栏;护栏清洗时把敏感模块从 sys.modules 删名(asyncio 自己
    # 绑定的引用不受影响),脚本 `import asyncio` 走缓存照常可用。
    import asyncio

    captured = ptc_runtime.install_guard()

    # 脚本命名空间的内建件:emit/parallel/ToolCallError 不必 import 就能用
    # (规格示例就是裸调 emit(...));luban_tools 里的重导出照旧可 import。
    namespace = {"__name__": "__main__", "__file__": "ptc_script.py",
                 "emit": ptc_runtime.emit, "parallel": ptc_runtime.parallel,
                 "ToolCallError": ptc_runtime.ToolCallError}
    exit_code = 0
    try:
        exec(compile(script, "ptc_script.py", "exec"), namespace)  # noqa: S102 —— PTC 的约定就是跑模型脚本
        entry = namespace.get("main")
        if asyncio.iscoroutinefunction(entry):
            asyncio.run(entry())
    except SyntaxError as exc:
        ptc_runtime._fail("syntax", "SyntaxError: %s (line %s)" % (exc.msg, exc.lineno))
        exit_code = 1
    except ImportError as exc:
        # 护栏拒绝的 import 单独记 guard 阶段(沙箱拒绝,与普通 import 错
        # 分开报,方便熔断器与用户分辨)。
        stage = "guard" if str(exc).startswith("PTC sandbox") else "import"
        ptc_runtime._fail(stage, str(exc))
        exit_code = 1
    except ptc_runtime.ToolCallError:
        ptc_runtime._fail("runtime", "uncaught ToolCallError", ptc_runtime.traceback.format_exc())
        exit_code = 1
    except Exception:  # noqa: BLE001
        ptc_runtime._fail("runtime", "script crashed", ptc_runtime.traceback.format_exc())
        exit_code = 1
    if exit_code == 0 and not ptc_runtime._emitted[0]:
        ptc_runtime._fail("runtime", "script finished without emit(): 用 emit(摘要) 收口")
        exit_code = 1
    if exit_code == 0:
        ptc_runtime._send({"type": "done", "captured_stdout": captured.getvalue()[-4000:],
                           "calls": ptc_runtime._calls_made[0]})
    # 收尾:关原始 stdin 叫醒 reader 线程,再退场,不留 Fatal error。
    ptc_runtime._close_stdin_raw()
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
)PTC";

}  // namespace lubancode::ptc
