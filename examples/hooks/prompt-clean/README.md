# prompt-clean:改写候选(PreUser)

清洗用户输入的首尾空白与连续空行。演示挂点手册里 PreUser 的核心合同:

- `input` 是候选副本:本地随便改,不经 `next(candidate)` 提交就不算数;
- 改写只发生在 freeze 前(PreUser 天然在接纳之前);
- match 未命中(如 purpose=btw 的旁问)记 `skipped_no_match`,不动原文。

跑法:`lubancode hook test .`(本目录)。
