# 冒烟清单

验收网页照这份逐项打勾。口径与记法见 `skills/browser-testing/references/selectors.md`。

| 项 | 合格线 |
| --- | --- |
| 标题 | h1 恰一个;层级不跳 |
| 链接 | 无空链接、无 `javascript:` 占位、无写死的 localhost |
| 表单 | 每只 form 有 action 与提交途径 |
| 图片 | img 均带 alt;空 alt 须是装饰性 |
| 文案 | 无占位符残留(Lorem、TODO、FIXME) |

判级:全过为通过;一项不过为未通过;证据不足的项标未验证,计入"部分通过"。
