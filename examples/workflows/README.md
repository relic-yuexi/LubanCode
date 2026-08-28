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

## sansheng-liubu — 三省六部 2.0

借三省六部的骨架跑一件事:几案并陈、合案拟诏、独立封驳、御批画敕、照路由表分牒、六部办差、复命缴旨。

### 两层宪制

宪法层固定,写死在图里:

```
皇帝(requirement)
  ↓
谋议 moulue(map,items=inputs.lanes,并发 3)
  └─ 谋士 mouyi(llm·lao):沿 item 立场独立拟一案
  ↓ success
往复澄清 fengbo(loop,默认最多 12 轮,硬帽 20)
  ├─ 中书 zhongshu(llm·lao):合案拟诏,诏首必有一节"需求复述"
  ├─ 门下 menxia(llm):只见诏书,不见原话,独立 approve/reject
  ├─ 御前 chengzhi(ask_user):画敕——批准、批语、委托或墨敕
  ├─ 未过闸 → 驳词批语进 previous,下一轮中书修订
  └─ exhausted → 未决收口 weijue(end,history 留账)
  ↓ success
尚书 shangshu(llm·lao):照 inputs.ministries 路由表拆密封差遣
  ↓
御前加签 jiaqian(ask_user):照单全发,或批注增删改
  ↓
定牒 dingdie(llm):誊单并批
  ↓
发牌 fapai(map,items=差遣单,并发 3)
  └─ 差役 banshi(agent·coder):密封差遣办差
  ↓
复命 fuming(llm):御史汇奏
  ↓
缴旨收尾 shouwei(end)
```

行政层是数据,全在 `inputs`:

- `lanes`:几案并陈。默认最小改动/结构治理/风险优先三案。
- `ministries`:职官路由表,每行 `id`/`name`/`mandate`/`tools`。默认工(改)/刑(测)/兵(构建)三部。
- `review_limit`:封驳轮帽,默认 12,可用 `--review_limit=5` 放收。

添一案、添一部,改的是数组,图不动。旧版把三案三房焊在节点与 schema 里,加一部要改四处;2.0 起官制归数据。

### 设计注记

**谋议在 loop 外。** moulue 是 map,items 引 `inputs.lanes`,body 只收 mouyi 一只节点 id。item/index 由引擎注入 body 输入,prompt 教模型从用户消息 JSON 里读 item 字段——body 的 input 写不得 `${item}`,resolver 只认 inputs/vars/nodes/artifacts/run。谋议只献策一次,不随封驳重炒;map 产物是 `{items, failures}`,中书取候选走 `${nodes.moulue.output.items}`。

**门下不见原话。** menxia 的 input 只有诏书与拒绝标准,没有 requirement。诏书 memorial 首节"需求复述"是原意唯一凭据,门下头一条便审"复述与正文是否自洽"。两节点各发独立请求,中书的推演不污染复审。门下不设 model_role,走会话 normal;谋士与中书写 `model_role: lao`。

**封驳怎么循环?** 不画普通回边。`loop` 收住 body、停止条件与轮次帽,until 读 chengzhi 的 `complete`。每轮产物写进 `history`,上一轮放在 `previous`(含 `outputs.menxia.reasons` 驳词与 `outputs.chengzhi.answers` 御批),中书下一轮逐条吸收。皇帝点"不知道,请中书定案"后,委托标记跨轮留账:中书按稳妥默认值修,门下继续真审,不再换一批小问题反复烦人。

**差遣怎么密封?** 御批后尚书照路由表把诏书拆成若干封差遣,每封必含 `ministry`/`objective`/`allowed_scope`/`forbidden_scope`/`acceptance`/`ambiguity_action`,数量随诏书裁,不凑数。加签后定牒誊一遍,发牌 map 逐封并走。banshi 是一只 agent 节点,input 无自有字段,只吃引擎注入的 item——六部一体,身份与职司从 item 里读,forbidden_scope 是禁域,越界即停手回报。

**llm 只输出纯 JSON。** llm 节点的 output_schema 引擎不接线,不强制;执行器对输出做 json::parse,解析不成整段退成 `{content: 文本}`,下游就没得读了。prompts 里已下死命令。

### 皇帝三特权

- **steering 接管**:banshi 办差途中,皇帝可插话续指令,差役接着办,不必等收工。
- **加签(jiaqian)**:差遣发牌前过一道御前。照单全发,或写批注增删改;批注文字进 answers,由定牒并进单子。
- **墨敕(chengzhi 的 override_answers)**:门下虽驳,皇帝选"墨敕:门下虽驳,朕意已决",诏书强行放行——approved/complete 直接置真,产物带 `overridden: true`。走此路责任在朕。

### 改玩法

- **添案**:`inputs.lanes` 加一行,谋议便多一案。
- **添部**:`inputs.ministries` 加一行,尚书便多一路可遣。整表也可运行时传入:`/workflow run sansheng-liubu --requirement="..." --ministries='[{"id":libu,"name":"吏部","mandate":"管文档","tools":["read_file","edit_file","search"]}]'`(JSON 数组)。
- **硬工具帽**:banshi 的 allowed_tools 是四件并集。要收紧,拆两条 map——落笔一票走 `[read_file, edit_file, search]`,只读一票走 `[read_file, search, run_command]`,items 各引各的差遣子表。
- **调帽**:`limits` 里的 `timeout`/`tokens`/`tool_calls` 按差事轻重放收;发牌并发吃 `max_concurrency`。

### 已知边界

- approval/ask_user 依赖 InteractionBroker——交互终端里可用;无人值守宿主没装 Broker 时会明报 `not_configured`,不挂死。
- agent 节点审批已接终端确认链:差役动用写盘、执行类工具,照原确认机制守门,workflow 不替用户扩权限。
- 六部并行办差,同一工作区里写写可能互踩。尚书拆单时用 `allowed_scope` 划清地界;要真隔离,须先接 worktree。
- agent 节点 `role` 当前无注册名录校验(能力表 agent_roles 为空),role 名自由;将来接了名录,validate 会点名。
