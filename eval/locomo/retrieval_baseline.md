# LoCoMo-MC10 检索层基线(E1)

数据:扰动后(实体改名+日期平移+题目改写)十场对话,每 session 一条
fact 主题,走 LubanCode memory 正门(EnqueueSave→worker 落盘→
BuildTurnContext 生产召回路,Options 生产默认 max_results=3、
max_retrieval_bytes=8KiB)。判据口径见 scripts/eval_locomo_retrieval_report.py
头部注释;逐题排级账在 trace.json(gitignore,本地复算用)。

## 五类 Recall@k(分母=可定位题)

| 类别 | 题数 | 可定位 | R@1 | R@3 | R@5 |
|---|---|---|---|---|---|
| adversarial | 446 | 0 | - | - | - |
| multi_hop | 321 | 65 | 0.169 | 0.446 | 0.600 |
| open_domain | 841 | 367 | 0.392 | 0.594 | 0.659 |
| single_hop | 282 | 53 | 0.208 | 0.415 | 0.585 |
| temporal_reasoning | 96 | 13 | 0.308 | 0.538 | 0.538 |
| **合计** | 1986 | 498 | 0.341 | 0.554 | 0.641 |

## 注入面与拦截(全量题)

| 类别 | 题数 | 零命中 | 平均注入字节 | 平均注入条数 | 门槛拦截 | 预算拦截 |
|---|---|---|---|---|---|---|
| adversarial | 446 | 0/446 | 7870 | 2.95 | 27 | 10980 |
| multi_hop | 321 | 0/321 | 7839 | 2.99 | 16 | 7770 |
| open_domain | 841 | 0/841 | 7820 | 2.95 | 142 | 20888 |
| single_hop | 282 | 0/282 | 7906 | 2.96 | 63 | 6858 |
| temporal_reasoning | 96 | 0/96 | 7681 | 2.89 | 14 | 2326 |

可定位但排级榜无名的题(evidence topic 整库零得分): 0

adversarial 无 evidence(答案 Not answerable),不进 Recall 表,
只看注入面——B 组幻觉率在 E2 端到端量。

