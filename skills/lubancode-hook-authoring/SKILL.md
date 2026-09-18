---
name: lubancode-hook-authoring
description: 制作、修改或排障 LubanCode 的 Lua hook 包时使用。按用户描述选挂点、生成 hook.json 与 main.lua、写 fixtures、跑 lubancode hook validate/test 校验，再指引用户装进 ~/.lubancode/hooks 或项目 .lubancode/hooks。写普通提示词、工具插件或 Workflow 时不必使用。
---

# LubanCode Lua Hook 制作指引

官方文档根固定为 `<技能目录>/../../docs`。生成脚本前**必读**对应手册，
用真实绝对路径打开，不把占位文字当目录：

1. 选挂点、定效果：读 `features/hooks/hook-points.md`(挂点一览与效果
   矩阵——位置不合法的代码写出来也过不了校验)。
2. 写 hook.json(覆盖/排序/匹配/能力申请)：读
   `features/hooks/middleware-config.md`。
3. 用 `luban.*` Host API:读 `features/hooks/host-api.md`——只使用手册
   里写明的函数与字段，不猜函数名;`capabilities` 只用那七个词。

## 生成合同

- 包形状:`<目录>/<id>/hook.json + main.lua + fixtures/*.json`,entry
  相对包根,不许 `../` 越界。`lubancode hook init <名字>` 可落官方
  scaffold,在此基础上改。
- main.lua 顶层只构造函数与常量;handler 形状冻结为
  `function(ctx, input, next)`,`next` 至多一次,业务拒绝用
  `ctx.deny(code, message)`。局部变量不跨调用,要存状态用
  `luban.state`。
- 每个行为写至少一枚 fixture(正例)+ 一枚边界(未命中/失败/取消);
  fixture 形状见 `src/runtime/hook_package_check.hpp` 头注。
- 样例参考:`<仓库>/examples/hooks/` 六枚,挑形状最接近的改。

## 校验流程(不跳步)

```bash
lubancode hook test <包目录>          # 静态 + fixtures(fake adapter)
lubancode hook validate <包目录> --json   # 看解析后的实际计划
```

- 静态挂了按报告逐项修(清单 schema/语法/对账/依赖/能力词表);
  fixtures 挂了按断言差异修脚本或修预期。
- 报告的"未验项"要如实转告用户:fake 过不等于真实服务可用;真实集成
  须装进会话沿既有授权观察,不得为校验访问外部服务或写业务文件。

## 装载与信任

- 落点:`~/.lubancode/hooks/<id>/`(用户级)或
  `<项目>/.lubancode/hooks/<id>/`(项目级)。同键用户层胜出;项目层先装。
- 申请了 http/fs/tools 的包,生产里须用户显式授权(发现不等于授权);
  生成完要指引用户完成授权与信任确认,不代签。

## 边界(不越)

- 不生成 TS/WASM/DLL/外置进程 hook;外部语言生态走 MCP。
- 不绕过宿主:不伪造 tool_call、不直连 MCP Client、不写授权根外的文件。
- 版本热换(分代热换装/状态保留)另立设计单,不在 hook 包里实现。
