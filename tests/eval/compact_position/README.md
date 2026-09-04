# 实验 B1:compact 位置探针(collect 骨架)

单子 §二 B1,首仗。K 条关键事实按受控深度({0,5,…,95}% 共 20 档)插入仿会话长上下文,三处理对照(FULL 原文 / microcompact 折叠 / compact 六栏摘要),直接召回 + 更新冲突两类探针,中英措辞各一形,每档每处理 5 次。判卷确定性优先,LLM judge 只补同义表达。

本目录现状:只有 `collect.py` 骨架(位置-命中曲线 + 中段存活率两张表,零分母记 `unavailable`)。原始账 schema 见脚本头注;造稿器/三处理对照/判卷器是 P1 的活。

注意与 B2 主基准 Microsoft Lost in Conversation(`../compaction_benefit/`)是两码事:B1 借 *Lost in the Middle*(Liu et al., TACL 2024)的采样法,别混名。

跑法(有原始账后):`python collect.py --raw results/raw_position_probe.jsonl --out-dir results`
