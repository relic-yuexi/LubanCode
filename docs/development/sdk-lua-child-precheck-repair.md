# Lua 恢复与子场捕获夹具补正

远端源 `605d200f` 的 ASan 实跑有两处失败。本笔只改验收夹具。生产校验、发布器、CASE 名册和原有时限不动。

## Lua 恢复拒绝

新 Command Jobs 计划先核恢复材料。已采用 system 若换了 sessionId，Jobs 绑定先报 `sdk.job.plan_invalid`；开场失败，模型未调用。原夹具将这次拒绝一概认作 Lua 计划错误，误报失败。

原 LuaRestoreBad 仍只认 `sdk.lua.plan_invalid`。另设明确的 foreign-system 入口，只认 `sdk.job.plan_invalid`，只用于篡改已采用 system 身份那一轮。其余坏 Lua 声明照旧。不得接受任意错误码，也不挪动生产开场顺序。

同一原生 CASE 再添一轮：只改已采用 system 的 Lua 指纹，保留场身份和 Jobs 绑定。它必须越过 Jobs 预检，由 Lua 绑定校验拒开。每轮仍查零模型调用、原账本与计划字节不变；最后恢复原件，健康开场照常通过。

## 子场捕获失败

旧夹具占住固定的 capture-000002.combined.txt.tmp。新不可变发布器使用带进程号和序号的独占临时名，旧障碍已碰不到真实写入。

夹具等大兄弟真实捕获完成、ResultStore 已持有下一编号，再占住最终 capture-000002.combined.txt 目录。真实 no-replace 发布必须失败。不得猜临时名，不添生产钩子，不让父场 writer 跟着坏掉。

夹具持有障碍目录和大兄弟两份原件快照。真实 capture 返回后，核目录仍在、第二份 metadata 未发布、无遗留临时文件、大兄弟 channel 与 metadata 字节未动。原父场停转、零 summary/后续工具、子场真实终态、首次失败缓存及重复调用不续账断言全保。Rig 先退场，外层 Directory 再删自己持有的临时树。

## 交付与验证

只改公开消费 helper、Lua 原生恢复夹具、子场共享夹具及枚举引用。本地仅做文本守卫、Python 纯数据门、AST 与文档检查；不配置、不编译、不运行原生程序。三平台和 ASan 的真实结果由后续远端 CI 给出。
