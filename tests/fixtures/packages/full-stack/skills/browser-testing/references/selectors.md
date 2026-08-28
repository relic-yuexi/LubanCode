# 查什么,怎么记

这份是 SKILL.md 第 2 步的口径补充,照 checklist 的项一一对应。

## 链接

- 数 href 总数,单列:空串、`#`、`javascript:`、`localhost`、写死 IP。
- 记法:`链接 12,可疑 3:javascript:void(0) x2,localhost x1`。

## 表单

- 每只 form 记:有无 action、有无提交按钮(input[type=submit] 或 button 不带 type=button)。
- 缺项的写全位置:`form#signup 缺提交按钮`。

## 标题

- h1 恰一个;h1 到 h6 层级不跳。
- 记法:`h1 x1;跳级:h2->h4 一处`。

## 图片

- img 总数,缺 alt 的数。
- 记法:`img 8,缺 alt 2`。

## 证据格式

每项一行:`[项] 结论 —— 证据`。例:

```text
[标题] 通过 —— h1 仅一个: "Full Stack Sample"
[表单] 未通过 —— form#signup 缺提交按钮
[外链] 未验证 —— 摘要未含目标状态,需截图复核
```
