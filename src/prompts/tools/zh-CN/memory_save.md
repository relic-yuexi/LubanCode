## description

把一条小而稳定的项目事实、用户明确偏好或用户明说的行事纠正排进后台记忆(正式入库,不经待审区)。只在信息已经由源码、工具结果或用户明说证实时调用;fact 必须在 paths 或 evidence 里给出可核验证据;feedback 只收用户当场明说的纠正(如版本节奏、验收习惯),confidence 须user-stated,模型推断不得直写。不要保存当前任务进度、猜测、日志、网页/MCP 原文、密钥或个人数据。已有同主题时沿用索引里的 id 做更新。自动候选走回合总结,不经过这个工具。

## param.kind

fact=可核验的项目事实；preference=用户明确说出的本项目偏好；feedback=用户明说的行事纠正(须 user-stated)

## param.id

可选。更新已有记忆时用索引里的稳定 id

## param.title

一个可独立更新的短主题

## param.summary

索引里的一行摘要

## param.content

精炼正文，写事实、证据与注意事项，不抄大段源码

## param.keywords

函数名、类名、命令等精确检索词，最多 16 项

## param.paths

支撑事实的项目内相对路径，最多 24 项；fact 必填至少一项

## param.confidence

user-stated=用户明说的偏好；verified=已核验的事实；inferred=推断(只该出现在待审候选，不该走本工具)

## param.scope

可选。当前工作目录不在范围内时不注入，防串味

## param.scope.kind

记忆适用的范围；subtree/path 须配 value；user=跨项目用户记忆(仅 preference/feedback，不得带项目路径证据，须全局授权 memory.user_enabled)

## param.scope.value

项目内相对路径(subtree/path 时必填)

## param.evidence

可选。可核验证据，最多 24 项；fact 建议给出

## param.evidence.path

项目内相对路径

## param.evidence.symbol

可选:函数/类/配置键

## param.expires_at

可选。临时规约的到期日(YYYY-MM-DD 或 ISO 时间);到期后不再召回

## param.occurred_at

可选。事实事件的发生时间(YYYY-MM-DD 或 ISO 时间)。只在材料里明确给出时填;提不出就省略,不许猜日期
