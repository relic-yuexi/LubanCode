# Lua Hook(中间件体系)

[文档首页](../../README.md) · [挂点手册](hook-points.md) · [中间件配置手册](middleware-config.md) · [Host API 手册](host-api.md) · [旧 command hooks](../extensions/hooks.md) · [示例包](../../../examples/hooks/)

Hook 是一套带挂点、执行顺序、同名实现覆盖和效果提交的中间件体系:既能
观察事件,也能参与决定后续怎么执行。对外编写 handler 的语言首版只有
**Lua**;其他语言生态的外部能力由 MCP 承接,不再并行建设第二套 hook
语言和加载器。

```text
C++ 宿主     执行时点、身份、准入、预算、取消、效果提交、session 管理
内置 handler  核心功能的默认实现(估算/容量/验收排程),与 Lua 共用注册池
Lua handler  在指定槽位新增或替换功能,参与有序中间件执行
MCP          接外部工具与服务;Lua 经宿主工具调用桥使用,不另起协议
```

三册手册各管一段:[挂点](hook-points.md)讲每个位置能做什么;
[中间件配置](middleware-config.md)讲清单、覆盖、排序与匹配;[Host API](host-api.md)
讲 Lua 里 `luban.*` 每个函数怎么调。六枚可跑示例在
`examples/hooks/`,每枚 `lubancode hook test <目录>` 即验。

## 快速上手

```bash
# 生成一枚可跑的脚手架(hook.json + main.lua + fixtures)
lubancode hook init my-first-hook

# 静态检查 + fixtures 试跑(真实 Lua runtime;HTTP/文件/工具走 fake adapter)
lubancode hook test my-first-hook
```

包装进会话:放 `~/.lubancode/hooks/<包名>/`(用户级)或
`<项目>/.lubancode/hooks/<包名>/`(项目级),下次装配时发现。项目层先装、
用户层后装,同键用户层胜出。

## 校验与试跑(四档分账)

`lubancode hook validate <目录> [--json]` 只跑静态档:清单 schema、entry
越界、Lua 语法与 handler 对账、依赖计划(发布裁决)、能力申请词表。

`lubancode hook test <目录> [--json]` 加跑 fixtures fake 档:fixtures/
下每枚 JSON 用真实 Lua runtime 派发一次,HTTP/工具按 fixture 编排的
fake adapter 应答(零网络、零业务文件),断言候选、效果、next 次数、
结局与错码。fixture 形状见 `src/runtime/hook_package_check.hpp` 头注。

报告四档,不互相冒充:

| 档 | 什么意思 |
| --- | --- |
| 静态过 | 形状合法、能编译。脚本可编译不等于行为已验证 |
| fake 过 | fixtures 在 fake adapter 下全绿 |
| 真实集成过 | 须装进会话沿既有授权观察,validate/test 不替信任流程开门 |
| 未验 | 明列(真实 HTTP/MCP 服务、平台边界等) |

退出码:0 全过 / 1 有 fail / 2 包读不到。

## 生命周期

- **按需加载**:目录发现只读 `hook.json`;脚本在发布期编译(显式预热),
  每次 invocation 建全新 Lua state——局部变量不跨调用,业务状态走
  `luban.state`(宿主持有,按包隔离、有限额)。
- **clear/exit 排空**:换会话写者时自动排空(BeginDrain → 等 in-flight
  invocation 收口 → 换绑);排空窗口里新 invocation 只留进程内能力,
  工具桥对新的子执行回 `hook.tool.drained`。
- **迟到结果归属**:子执行账钉在 invocation 起点的那只写者上——工具
  执行跨过换场点,终态仍落发起会话,不写进新 session。
- **资源回收**:先让 Host API 返回、Lua 栈退出、worker 收口,再销毁
  state;事件账的内存簿有界驻留(老 dispatch 台账回收)。
- **版本热换**:不做。分代热换装与状态保留另立设计单
  (`todos/Lua插件分代热换装与状态保留设计.todo`),本体系只保证在途
  调用持旧版 FrozenRegistry 不被 reload 强拆。

## 边界(写明,不冒充)

- 生产接线已覆盖 PreUser/PostUser/PreRequest(三段)与 PostTurn/goal.review
  槽位;PreSystem/PostSystem/Pre/PostAssistant、Pre/PostStep、Pre/PostAction
  的合同已冻结,接线沿挂点迁移批次推进(示例先行,fixtures 现在可验)。
- fork/btw 的会话运行时(§4.57/§4.58)在轨迹 v3 父单余项里;钩子层的
  purpose/deliveryMode 隔离与迟到归属合同已钉测试。
- 插件状态首版进程内,不跨进程持久化;需要持久记忆走记忆体系。
