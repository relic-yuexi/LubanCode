---
name: eval-smoke-audit
description: 评测冒烟的样板技能：读一份报告 JSON，核疑点清单有没有出处。凡问“这份清单齐不齐”，先读这份。
---

# 评测冒烟样板

纪律一条：每条疑点附出处。文件、字段，缺一样不算证据。

## 步骤

1. 读 report.json。
2. 核 findings 数组：每条有 source 与 field。
3. 缺出处的记为疑点。

## 验收

- report.json 可解析，findings 每条带 source 与 field。
- 人工复核：每条疑点的说法与账本记录一致。
