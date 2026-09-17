# postaction-refine:结果加工(PostAction)

工具结果保存之后、提交唯一 tool message 之前做加工。演示:

- PostAction 的效果矩阵:只有 `result.supplement`/`result.replace`/
  `result.filter`,没有准入、没有输入改写(副作用已发生);
- isError 的结果不加工——错误不洗成成功;
- 派生内容走效果管道(候选先存,宿主验证后 applied),不冒充工具原话。

注意:PostAction 的生产接线沿挂点迁移批次推进;本示例的合同与 fixtures
现在就能用 `lubancode hook test .` 验。
