# replace-estimator:同名替换内置槽位(PreRequest/estimate)

逻辑键 `PreRequest/context.token_estimate` 与内置估算器同键,用户层压过
builtin——获选项进计划,内置退出执行计划(报告的 plan 字段里看得见
overridden)。演示:

- 覆盖的是"这个功能用哪份实现",不是"加一枚回调":依赖指向逻辑键,
  自动连到获选实现;
- 槽位合同不因替换解除:estimate 阶段只产出 EST1 形状的测量结果,输入
  已冻结,改写一律拒;compact/title 等旁路请求的估算走同一槽,不自带
  第二份公式;
- 这版是 CJK 友好的字符口径(UTF-8 码点计数),系数可调。

跑法:`lubancode hook test .`;`lubancode hook validate . --json` 的
plan.overridden 里能看到被压下的 builtin.token_estimate_v1。
