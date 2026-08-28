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

借三省六部的骨架跑一件事:三案并陈、合案成书、独立拒审、批准后执行。

```
皇帝(requirement)
  ↓
往复澄清 fengbo(loop,默认最多 12 轮,用户可调,硬帽 20)
  ├─ Lao 谋议 moulue(parallel):最小改动/结构治理/风险优先三案并陈
  ├─ 中书 zhongshu(llm·lao):择案合并，写成方案书
  ├─ 门下 menxia(llm):只看方案书和拒绝标准，独立输出 approve/reject
  ├─ 御前 chengzhi(ask_user):展示方案书与判词，批准、打回或委托中书定案
  ├─ 未批准 → 带批语、驳词回入口，再拟三案
  ├─ 皇帝与门下都放行 → 尚书省 shangshu(llm·分牒)
  │                         ├─ 工房只收实现牒
  │                         ├─ 刑房只收测试牒
  │                         └─ 兵房只收构建牒
  │               ↓
  │             工房 gongfang(agent·coder)
  │               ↓
  │             刑兵并验 liubu(parallel,all)
  │               ├─ 刑房 xingfang(agent·tester)
  │               └─ 兵房 bingfang(agent·builder)
  │               ↓ joined
  │            缴旨收尾 shouwei(end)
  └─ exhausted → 未决收口 weijue(end,history 留账)
```

### 设计注记

**尚书省做什么?** 御批之前不露面。皇帝与门下都放行后，它接整份方案书，拆成工、刑、兵三封密封差遣。各房 input 只含自己的 `dispatch`，不含整份诏书，也不含别房任务。workflow 图仍掌握次序与汇合；尚书只作语义分工，不改诏、不添需求。

**封驳怎么循环?** 不画普通回边。`loop` 节点收住 body、停止条件与轮次帽。
每轮产物写进 `history`,上一轮放在 `previous`;门下放行便走 `success`,撞软帽
便走 `exhausted`。`review_limit` 默认 12；用户可用 `--review_limit=5` 放收。
`hard_limit` 写死在定义里,运行参数越不过。

三只候选节点都写 `model_role: lao`。宿主按会话 ModelRouter 给它们各造独立
backend，并行请求不会共抢一只 client。中书也走 Lao；门下不接候选案，只接成稿与
拒绝标准，免得中书的推演过程污染复审。中书只列高层 `workstreams`；御批后，尚书才把它们展开成可执行差遣。

`chengzhi` 把方案书、门下判词、待明确处一并摆给用户。用户批准，且门下也放行，
才算过闸；用户写修改意见，整份批语回入口重拟。点“不知道，请中书定方案”后，
委托标记会跨轮留账：中书按稳妥默认值修，门下继续真审；没审过便自动再修，
不再换一批小问题反复烦人。`review_history` 留着每轮 approve/reject，能查拒绝率是否长期归零。

**循环活在哪?** 禁环禁的是"图上回边",不是循环本身。三层载体:

1. **agent 内环**(最常用):工房的差事写"改到测试绿为止",跑测试→看红→改→再跑,环在一只节点里转。上限靠 `step_limit` 与工具参数连败账。
2. **节点 retry**:同节点 `retry.attempts` N 次;副作用节点无幂等键不许重试。
3. **map/foreach**:数据驱动。items 有几项便跑几项;它不拿上一轮判词决定何时停。
4. **loop**:条件驱动。body 顺次跑,until 命中便停;软帽可取输入,硬帽必须写死。

**门下不该吃中书上下文。** 两只节点各发独立请求。门下 input 只有原旨、方案书、
轮次与拒绝标准；没有三份候选案，也没有中书思路。若要给门下另配模型，可在节点上
另写 `model_role`；未写便走会话 normal。

### 改玩法

- **加房**:在 `liubu.branches` 添一只 agent 节点；中书 `workstreams`、尚书输出 schema 与 prompt 同步加字段，门下也补这房的职责拒绝标准。
- **换活**:三房按活分(营造/按验/演武),不按官名分。差事变了改 prompts,图可以不动。
- **调帽**:`limits` 里的 `timeout`/`tokens`/`tool_calls` 按差事轻重放收;刑兵二房并行吃 `max_concurrency`。

### 已知边界

- approval/ask_user 依赖 InteractionBroker——交互终端里可用；无人值守宿主没装 Broker 时会明报 `not_configured`,不挂死。
- agent 节点 `role` 当前无注册名录校验(能力表 agent_roles 为空),role 名自由;将来接了名录,validate 会点名。
- 工房先写，刑房与兵房随后只读查验。若往并行分支里添写操作，仍须先接 worktree 隔离，免得共用工作区互踩。
