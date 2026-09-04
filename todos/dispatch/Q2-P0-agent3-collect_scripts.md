# 派工单｜Q2-P0-agent3｜三套 collect.py 模板与数据落点

> 母单：`todos/工具与上下文治理量化评测_Q2.todo`（第 P0 章）
> 派活形态：one_shot
> 钥匙清单：本单独占 `tests/eval/<实验名>/collect.py`、`tests/eval/<实验名>/README.md`、`tests/eval/<实验名>/results/` 空目录。三实验互不撞——A、B、C 三套脚本落三处

## 一、做什么

给三份实验各写一份 collect.py 模板与 README，定义：

- 入参：实验名、模型 id、provider、commit 号、五次重复的开关
- 出参：`<实验名>/results/<commit>-<model>-<run>.parquet` + 同名 `.md` 摘要
- 字段表：每实验先钉一份 JSON Schema，agent-1 的 `tests/eval/CMakeLists.txt` 不引用本目录，所以不撞

三套脚本骨架（每套独立）：

### A. `tests/eval/tool_search_threshold/collect.py`

入参：工具表档（A0..A4）、模式（disabled/legacy/proxy）、任务（T1/T2/T3）

字段：

```json
{
  "threshold档": 0,
  "模式": "disabled",
  "任务": "T1",
  "首字节延迟ms": 0,
  "tools字段字节": 0,
  "system字段字节": 0,
  "总请求字节": 0,
  "模型决策正确": false
}
```

输出 parquet 列：上述字段全展开 + 一列 `run_seq`（1..5）。

### B. `tests/eval/compaction_benefit/collect.py`

入参：会话 fixture 名、压缩模型 id、是否对照 FULL

字段：

```json
{
  "会话fixture": "...",
  "pre_tokens": 0,
  "post_tokens": 0,
  "compact请求input": 0,
  "compact请求output": 0,
  "compact是否成功": false,
  "拒收原因": "",
  "任务一致性查询命中": false
}
```

### C. `tests/eval/subagent_failure/collect.py`

入参：任务 fixture 名、对照模式（baseline/1sub/3sub）

字段：

```json
{
  "任务fixture": "...",
  "对照模式": "baseline",
  "总墙钟ms": 0,
  "model_tokens_in": 0,
  "model_tokens_out": 0,
  "usage_cached": 0,
  "usage_uncached": 0,
  "失败类型": "",
  "空转连续步数": 0,
  "任务完成": false
}
```

## 二、验收

- `python -m pytest --collect-only tests/eval/<实验名>/` 能发现 collect 入口（不真跑，只查 shape）。
- 三份 `README.md` 列：依赖（pandas + pyarrow）、跑法（命令行参数）、输出路径、字段字典、注意事项。
- `results/` 三目录为空但可写（chmod 755）。
- 不开 `-DLUBANCODE_EVAL=ON` 时，CMake 不读到本目录。

## 三、不做什么

- 不动 C++ 代码、不动 CMake。
- 不跑真实验——本单只落脚本骨架。
- 不引第三方依赖的安装脚本（pip / poetry 都不要）。
- 不写 README.md 之外的人话文档。

## 四、Worktree 操作

- 从 `origin/main` 开：`git worktree add ../wt-q2-p0-agent3 -b eval/q2-p0-agent3 origin/main`
- 不 merge main、不合 main、不 push。

## 五、报告路径

- 落点：三套 `collect.py` + 三份 `README.md` + 三个 `results/` 空目录
- 报告落 `todos/dispatch/Q2-P0-agent3.report.md`
- 不需要编译日志。