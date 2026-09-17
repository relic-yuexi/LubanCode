# 中间件配置手册

[返回](README.md) · [挂点手册](hook-points.md) · [Host API 手册](host-api.md)

逻辑键 `(hookPoint, name)` 标识一个功能槽位;实现为 builtin(C++)或
lua。这册讲清单怎么写、同名怎么覆盖、顺序怎么排、条件怎么匹配。

## 包的形状

```text
~/.lubancode/hooks/my-hook/     用户级;或 <项目>/.lubancode/hooks/my-hook/ 项目级
  hook.json                     清单:定义元数据集中在这,Lua 不复制清单
  main.lua                      入口:返回 handler 表
  fixtures/                     试跑用例(hook test 用)
    input.json
```

目录发现只读清单;entry 相对包根解析,绝对路径与 `../` 越界整包拒绝,
不依赖当前工作目录。

## hook.json(schemaVersion 1)

```json
{
  "schemaVersion": 1,
  "id": "prompt-normalize",
  "version": "1.0.0",
  "entry": "main.lua",
  "hooks": [
    {
      "hookPoint": "PreUser",
      "name": "prompt.normalize",
      "handler": "normalize",
      "priority": 100,
      "after": [],
      "before": [],
      "match": {"origin": "human", "purpose": "interactive", "deliveryMode": "steer"},
      "capabilities": ["prompt.read"],
      "failurePolicy": "abort",
      "replayPolicy": "manual",
      "required": false,
      "observer": false,
      "limits": {"activeTimeoutMs": 500}
    }
  ]
}
```

| 字段 | 说明 |
| --- | --- |
| `id` / `version` | 包身份;`implementationRef` 记成 `hooks/<id>#<handler>@<version>` |
| `entry` | 入口文件,相对包根 |
| `hookPoint` + `stage` | 位置。PreRequest 另收 `mutate`/`estimate`/`capacity`,其余只认缺省 |
| `name` | 功能名(逻辑键后半);`context.token_estimate` 等是内置槽位名,同名即覆盖 |
| `handler` | `main.lua` 返回的 handler 表字段名;加载时对账,缺了整包拒 |
| `priority` | 数值小者先;同值按逻辑键稳定排序 |
| `before` / `after` | 依赖指向**逻辑键**(`"Point/name"` 或同挂点裸名),覆盖后自动连到获选实现 |
| `match` | 触发条件:`origin` / `purpose` / `deliveryMode`,精确匹配;未命中记 `skipped_no_match`,不暗跑被覆盖的旧实现 |
| `capabilities` | 能力申请(词表见 Host API 手册);申请 ∩ 槽位允许 ∩ 宿主授权,缺一头不开 |
| `failurePolicy` | `abort`(缺省,失败即整体失败)/ `keep_original`(optional 纯转换失败以原输入继续) |
| `observer` | 只读观察者:可并发、无 next、不许带依赖、不许 required |
| `limits.activeTimeoutMs` | handler 自用墙钟(缺省 500ms;不含 next 下游等待) |

## 同名覆盖(overwrite 的真身)

覆盖是"这个功能槽位换一份实现",不是加一枚回调、也不是改写本次输入:

```text
候选定义:
  PostUser/skill.resolve  -> builtin.skill_resolve
  PostUser/memory.recall  -> builtin.memory_recall      [builtin 层]
  PostUser/memory.recall  -> hooks/my_memory#recall     [user 层]

配置解析后(来源层级 builtin < extension < project < user < session):
  PostUser
    -> skill.resolve  [内置]
    -> memory.recall  [用户 Lua]      ← 高层胜出,只跑获选项
```

- 被覆盖的 builtin 保留定义来源,进 `overridden` 清单,不进执行计划
  (`lubancode hook validate --json` 的 `plan.overridden` 可查)。
- 同层同键两条 = 冲突,整版拒绝(不按文件扫描先后猜赢家)。
- 槽位合同随槽定档:required/阶段以 builtin 声明为准,替代实现不能解除
  ——删掉估算器不会让请求绕过容量检查。
- 获选实现 match 未命中或失败,不暗中补跑被覆盖的内置;要 fallback 就
  显式再声明一枚,账上看得见实际选用。

## 排序

阶段 → 依赖(before/after)→ priority(小者先)→ 逻辑键稳定排序。
循环依赖、缺失依赖、阶段倒置(mutate 依赖 capacity 一类)整版拒绝,
错误带稳定码(`hook.plan.*`)。

## 匹配与作用域

- 消息钩子按**真实** origin/purpose/deliveryMode 触发,不扫描 wire role
  猜来源。常见值:`origin`: human / hook / skill / compact / …;
  `purpose`: interactive / title / compact / btw / …;
  `deliveryMode`: steer(普通输入)/ followup(slash 与子报告)。
- 普通用户输入默认 steer;slash/子报告按目标 followup;purpose=btw 的
  旁问不命中 interactive 专属钩子(btw/title/compact 各走各的 purpose)。
- 写"给主线用"的钩子就写 `"purpose": "interactive"`;要让 compact 请求
  也吃到,别写 purpose 或写两枚定义。

## 失败与预算

- 预算覆盖编译、指令(2e8)、内存(256 MiB/state)、墙钟(500 ms)与
  Host API;超限的稳定码是 `hook.lua.budget_*`。
- `keep_original` 两条路:未消费 next 失败 → 以进入本 handler 的版本
  继续一次;已消费 next 失败 → 采用下游已完成的收据。required 槽位恒
  abort。
- 业务 deny(deny_code/deny_message)与脚本错误(错误码表)分开;执行
  已完成、结果保存失败也分开——缺记录不能证明未执行。

## 查看解析后的实际计划

```bash
lubancode hook validate <目录> --json      # plan.points 逐挂点列获选项
                                          # (order/source/implementationRef/
                                          #  stage/priority/handlerKind)
                                          # plan.overridden 列被覆盖项
```

会话内每次 dispatch 固定 registryRevision 与定义 hash,执行中注册表改版
不影响本次;在途调用持旧版计划,reload 不强拆(热换归分代热换装单)。
