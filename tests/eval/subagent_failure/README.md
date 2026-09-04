# 实验 C:Subagent 失败分布(collect 骨架)

单子 §四。12 任务(只读检索 4、改写 4、长命令 4)× 三对照(主代理单人 / 派 1 只 Explore / 派 3 只并行)。记账:墙钟、token 总量、cost(cached/uncached)、失败类型分布(`background_unavailable`、`trajectory.subagent_start_failed`、`admission_depth_limit`、`admission_active_limit`、`hook_deny`、`tool_error`)、无进展指纹连续 step 数、独立评审完成率。验收:派工价值/成本两中位数。

本目录现状:只有 `collect.py` 骨架(三对照账 + 派工价值/成本两张表,基线缺位或分母为 0 记 `unavailable`)。原始账 schema 见脚本头注;12 任务夹具与三对照驱动是 P4 的活。

跑法(有原始账后):`python collect.py --raw results/raw_subagent_failure.jsonl --out-dir results`
