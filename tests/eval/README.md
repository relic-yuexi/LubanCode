# Q2 量化评测装置(tests/eval/)

《工具与上下文治理量化评测_Q2》单的装置落位处。目录即实验,共用件在本层。

## 跑法(opt-in,默认不编)

```sh
cmake -B build/eval -DLUBANCODE_BUILD_EVAL=ON <源码根>
cmake --build build/eval --target eval_smoke eval_tool_search_threshold
ctest --test-dir build/eval -L eval          # 多配置生成器加 -C <cfg>
```

`LUBANCODE_BUILD_EVAL` 默认 OFF:默认构建与 CI 三腿不进本目录,零新目标零新测试。所有 eval 测试带 `eval` label。

## 目录

| 目录 | 实验 | 现状 |
|---|---|---|
| `tool_search_threshold/` | A:工具检索退化阈值 | **P0 已通**:A2 档(40 只)disabled 单步 T1 落账 |
| `compaction_benefit/` | B2:compact 真实对照 | 备料四件套(lock/取数/README/smoke)+ collect 骨架 |
| `compact_position/` | B1:位置探针 | collect 骨架(造稿器/判卷器 P1 落) |
| `subagent_failure/` | C:Subagent 失败分布 | collect 骨架(驱动 P4 落) |

## 共用件

- `fake_backend.{hpp,cpp}`——`FakeStreamingBackend` 通用假后端。蓝本 `tests/unit/agent/test_loop.cpp` 的脚本式 FakeBackend(全仓 33 处私有假后端的代表支),通用化三件:可编程响应序列、注错误/断流、usage 记账回调。只给 eval 驱动用,不进 `lubancode_tests`;既有 33 处私有假后端原样不动(各有私有变体,强行统一必破行为零变,其中 3 处在 `tests/unit/app/` 手术禁区)。
- `collect_common.py`——四份 collect 的公共件:JSONL 读取、分桶、中位数/P95(线性插值)、**零分母记 `unavailable` 不填 0**(单子 §二 B)、落件 csv+md(pandas/pyarrow 齐时加 parquet;本机 python 3.13 无此二件,实测 2026-09-05,TODO 见该文件头)。

## 数据件约定

原始账 JSONL 与 collect 产物落各实验 `results/` 子目录(gitignore,可重生成);一行一跑,带 `commit`(configure 时 `git rev-parse --short` 注入)、`model`、`provider`(P0 全程假 backend,记 `fake-eval-model`/`fake`;真模型 B 实验 opt-in,模型钉死 ccmoon/gpt-5.6-luna)。字节口径是 `api::Request` 中立投影的 JSON dump 长度——四家 wire 序列化各有方言,档与模式之间可比的是同一投影,不是某家 wire 的真实请求体。
