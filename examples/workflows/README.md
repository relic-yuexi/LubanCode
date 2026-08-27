# 示例 Workflows

本目录放可直接取用的 workflow 包。每个子目录是一只包:`workflow.yaml` 加 `prompts/` 等资源,整目录自成一体。

## 启用

catalog 扫两处,包放哪一处的子目录都行:

| 位置 | 作用域 |
|---|---|
| `<项目>/.lubancode/workflows/<包名>/` | 项目级,遮同名用户级 |
| `~/.lubancode/workflows/<包名>/` | 用户级,全项目可用 |

把示例包整个目录拷过去即可:

```
cp -r examples/workflows/sansheng-liubu .lubancode/workflows/
```

验明正身再跑:

```
/workflow list                 # catalog 现扫现用
/workflow validate sansheng-liubu
/workflow graph sansheng-liubu ascii
/workflow run sansheng-liubu --requirement="给 README 补一段安装说明,写清命令与验收" --review_limit=5
```

定义了 alias 的可以直呼:`/sansheng-liubu 给README补一段安装说明`。
裸敲 `/sansheng-liubu` 时，朝廷会先问“皇上，您有什么需求？”，接住下一句再开跑。

---

## sansheng-liubu — 三省六部

借三省六部的骨架跑一件事:分权起草、动态封驳、先改后验。

```
皇帝(requirement)
  ↓
往复封驳 fengbo(loop,默认最多 12 轮,用户可调,硬帽 20)
  ├─ 中书 zhongshu(llm):起草或据上一轮理由修订
  ├─ 门下 menxia(llm):审核,approved=true 当场停
  ├─ success → 工房 gongfang(agent·coder)
  │               ↓ success
  │             刑兵并验 liubu(parallel,all)
  │               ├─ 刑房 xingfang(agent·tester)
  │               └─ 兵房 bingfang(agent·builder)
  │               ↓ joined
  │            缴旨收尾 shouwei(end)
  └─ exhausted → 未决收口 weijue(end,history 留账)
```

### 设计注记

**尚书省去哪了?** 没设节点。workflow 图本身就是尚书省——确定性代码分发,不费模型。这是这套引擎的本分:编排归图,判断归模型。

**封驳怎么循环?** 不画普通回边。`loop` 节点收住 body、停止条件与轮次帽。
每轮产物写进 `history`,上一轮放在 `previous`;门下放行便走 `success`,撞软帽
便走 `exhausted`。`review_limit` 默认 12；用户可用 `--review_limit=5` 放收。
`hard_limit` 写死在定义里,运行参数越不过。

终端的 workflow 执行器已经接上 `InteractionBroker`。这份示例仍让门下模型按
结构化判词收口；要人工圣裁,可在 `exhausted` 后添 `approval` 或 `ask_user`。

**循环活在哪?** 禁环禁的是"图上回边",不是循环本身。三层载体:

1. **agent 内环**(最常用):工房的差事写"改到测试绿为止",跑测试→看红→改→再跑,环在一只节点里转。上限靠 `step_limit` 与工具参数连败账。
2. **节点 retry**:同节点 `retry.attempts` N 次;副作用节点无幂等键不许重试。
3. **map/foreach**:数据驱动。items 有几项便跑几项;它不拿上一轮判词决定何时停。
4. **loop**:条件驱动。body 顺次跑,until 命中便停;软帽可取输入,硬帽必须写死。

**门下省要换强模型。** 同模型自审自己,等于皇帝兼中书令。当前 llm 节点统一用会话模型,节点级 `model_role` 未接——升级路:装配层给 menxia 单独 backend。

### 改玩法

- **加房**:在 `liubu.branches` 添一只 agent 节点,诏书 schema 的 `tasks` 同步加字段,中书/门下的 prompt 里补这房的差事规矩。
- **换活**:三房按活分(营造/按验/演武),不按官名分。差事变了改 prompts,图可以不动。
- **调帽**:`limits` 里的 `timeout`/`tokens`/`tool_calls` 按差事轻重放收;刑兵二房并行吃 `max_concurrency`。

### 已知边界

- approval/ask_user 依赖 InteractionBroker——交互终端里可用；无人值守宿主没装 Broker 时会明报 `not_configured`,不挂死。
- agent 节点 `role` 当前无注册名录校验(能力表 agent_roles 为空),role 名自由;将来接了名录,validate 会点名。
- 工房先写，刑房与兵房随后只读查验。若往并行分支里添写操作，仍须先接 worktree 隔离，免得共用工作区互踩。
