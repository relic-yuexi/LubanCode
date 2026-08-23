# PTC:Programmatic Tool Calling(程序化工具调用)

[文档首页](../../README.md) · [配置手册](../../reference/configuration.md) · [工具参考](../../reference/tools.md) · [安全模型](../../development/security.md) · [测试指南](../../development/testing.md)

依据论文 *The Bitter Lesson of Tool Calling*(arxiv 2608.06370)。PTC 指模型写
Python 脚本、经 typed stubs 编排工具,不是 Parallel Tool Calling。

## 一副骨架

1. 宿主把入选工具的 JSON Schema 编成带类型的 Python stub 模块
   `luban_tools.py`(`src/ptc/stub_generator.cpp`);
2. 模型调用 `programmatic_tool_calling` 工具,交一段 Python 脚本;
3. 脚本在沙箱子进程里跑(`src/ptc/runner.cpp` + 内嵌 `ptc_runtime.py`),
   每次 stub 调用走 framed stdin/stdout RPC 回宿主;
4. 宿主对每一枚调用执行与 JSON 后端**完全相同**的链(`agent::RunOneTool`:
   schema 复检 → PreToolUse → 确认档/PermissionRequest → 执行 → PostToolUse
   → 编码清洗 → 审计);
5. 脚本 `emit(摘要)` 收口,摘要送回模型;长链不再每步一轮推理。

## 脚本怎么写

```python
from luban_tools import read_file, search

hits = search(mode="grep", pattern="HookDispatcher", path="src")
files = [read_file(path=line.split(":")[0])
         for line in hits["content"].splitlines()[:8]]
emit({"hit_count": len(hits["content"].splitlines()),
      "read": [f["content"][:80] for f in files]})
```

约定:

- 成功结果是 dict:`{"content": str, ...}`;工具层失败与被拒(权限/hooks/
  限额)抛 `ToolCallError`,用 `try/except` 收口;
- 结尾必须 `emit(摘要)`,只许一次;
- `parallel([...])` 把一批调用线程并发上线(在飞窗口受 `max_concurrency`
  约束);`asyncio.gather` 也兼容,但调用在求值时已完成(串行);
- 定义 `async def main():` 会被自动调用,不要自己 `asyncio.run`;
- 默认禁网络/文件系统/子进程/环境变量/本机模块,只许纯计算标准库白名单
  (json/math/re/itertools/collections/asyncio……)。越界 import 按沙箱拒绝
  收场。

## 配置

```json
{
  "tool_calling": "json | programmatic | auto",
  "ptc": {
    "python": "python3",
    "wall_clock_ms": 30000,
    "cpu_ms": 20000,
    "memory_bytes": 536870912,
    "output_bytes": 8388608,
    "max_calls": 100,
    "max_concurrency": 8,
    "restricted_token": true,
    "tools": ["read_file", "search"]
  }
}
```

- `json`(默认):不挂 PTC 工具,行为与从前逐字节一致;
- `programmatic`:强制 PTC;Python 缺失或平台无可靠沙箱时启动打一行回落
  说明,状态栏显示 `tools ptc→json(原因)`;
- `auto`:按能力画像与任务形状选;**首版没有 verified 画像,恒落 json**,
  状态栏显示 `tools auto→json`。门槛(规格"基准"节):画像 `verified` +
  五条硬条件齐 + 预估链长 ≥4 或 fan-out ≥8。

五道上限,撞线指名哪道墙:墙钟/CPU/内存/输出字节/调用数与并发数。
Esc 取消链:先拒未开始的调用,在跑工具自然收尾,最后杀脚本进程树。

## 沙箱(如实交账)

- **Windows**:Job Object(KILL_ON_JOB_CLOSE + CPU/内存上限)+ 受限 token
  (CreateRestrictedToken,禁全部特权;造不出自动降级并记档)。
- **POSIX**:RLIMIT_CPU / RLIMIT_AS 资源墙;**没有文件系统/网络隔离,
  不算可靠沙箱**——按规格默认禁 PTC。开发/测试要开,设
  `LUBANCODE_PTC_ALLOW_NO_SANDBOX=1`,风险自担。
- Python 层另有护栏(import 白名单 + 内建 open/input 封禁 + stdout 捕获):
  防君子栏,不是保险箱;硬边界在 OS 层。

PTC 的资源墙与受限 token 不是强化沙箱。威胁边界见[安全模型](../../development/security.md)。

## 能力画像

`~/.lubancode/ptc_profiles.json`,键为指纹
`provider + endpoint + model + wire + python 版本 + harness(ptc-v1)`,
任一成分变,旧画像查不到,天然降回 `unknown`。四档:

- `unsupported`:硬条件不齐或探针稳定失败;
- `unknown`:未测(默认);
- `experimental`:基本能跑,只许 programmatic 显式强开;
- `verified`:探针过线,auto 才可选。

五条硬条件:可靠沙箱 / 模型能输出自由代码 / 上下文装得下 stub 集 /
入选工具接全链 / Python ≥ 3.9。

无副作用探针五项(每项连跑数轮,偶然跑通不算):

1. **单调用**:按签名调一枚 stub,参数与结果都对;
2. **真依赖链**:前一枚的真实返回值喂后一枚;
3. **八路 fan-out**:不漏不重;
4. **异常收口**:一枚失败,其余与摘要仍有账;
5. **编码**:中文/emoji/反斜杠/引号/换行穿透脚本与 RPC。

前四项的离线判卷在 `tests/integration/ptc/test_ptc_bench.cpp` 与 LubanBench-Tool 离线层;
**带真模型的探针要手跑**(见下)。

运行时熔断器:连续 3 次语法错/空脚本/RPC 协议错,本场降回 JSON,不再升回;
工具层失败、撞墙、取消不计入。

## LubanBench-Tool

八类场景(`src/ptc/bench.cpp`):单工具短参数 / 2-20 步依赖链 / 2-100 路
fan-out / 128 份 schema 找针 / 大结果摘要 / 一半失败 / 只读副作用混合 /
扩展交叉(hooks/MCP/LSP/技能)。每题记任务成功率、漏调用率、参数正确率、
模型轮数、首结果与总耗时、tokens、重复调用数、sandbox/权限/hooks 覆盖率、
副作用重复数。

- **离线层**(进 ctest):手写参考脚本过 runner,验证判卷尺与 harness;
- **在线层**(手测清单):同模型、同题、temperature 0,JSON 与 PTC 各五轮。

## 手测清单(带真模型)

1. `config.json` 设 `"tool_calling": "programmatic"`,起交互会话,确认启动
   无回落提示、状态栏出现 `tools ptc`;
2. 问一个需要多次读文件的问题(如"统计 src/tools 下各工具的行数"),
   观察模型产出脚本、单卡聚合行、Ctrl+O 展开调用账;
3. 跑五项探针各三轮,把结果写进 `ptc_profiles.json`(status=experimental);
4. `/tmp` 下用在线层跑 LubanBench-Tool,JSON 与 PTC 各五轮,对比模型轮数
   与漏调用率;
5. 脚本运行中按 Esc:确认未开始调用被拒、在跑工具收尾、进程树杀干净
   (任务管理器无残留 python);
6. 撞线:脚本写 `while True: pass`,确认 30 秒墙钟指名报错;
7. Windows 下确认受限 token 生效(进程属性里特权清空),POSIX 下确认默认
   禁用、豁免开关生效。

## 已知边界(如实交账)

- 入选集只有只读工具(read_file/search);写工具的事务账与不可盲回退是
  P3;
- 宿主侧执行仍单线程串行(UI 回调线程语义与 JSON 路一致);`parallel()`
  的并发体现在"在飞窗口",不是多线程并行执行工具;
- auto 的链长/fan-out 预估器未建,恒保守落 json;
- 完整八类基准的在线层要真 API key,未进 CI。
