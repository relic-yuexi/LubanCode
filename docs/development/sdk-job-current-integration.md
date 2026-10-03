# SDK Job 当前前置组合

本笔只合已经发布的内部前置，不新增产品入口。起点为 #321 `35f58c2301f6e650c10806d14a71e55fddbf8351`，依次正常合入 #320 `6511a52dcedbf32ed9fbe451156345fa0ad0a168` 与 #318 `1ca361956e7c0f44133979043df3f0cb0eb62f8b`。#320 包含 Health batch 夹具与同次 full 抽件、owned Job 四场 probe payload 就绪；#318 包含命令执行阶段诊断与四场 Memory Save 公开值诊断。

这三份来源固定在私有 `codex/sdk-job-current-integration`。原 #315 分支与远端照留，不合 shared feature/main。合同先单独提交，之后才合源码；冲突只取真实并集，不从旧支线捡回旧 API。

## 范围与保全

中间件异常实际出口和 Next 所见、共用 DeferredEffects、原生 Hook 回执、Job 注册与接管等已有语义照留。两份诊断仍只记录实际阶段或已有公开返回值，不补确认、不猜根因、不加查询或重试。各源原 CASE、硬断言、预算、marker、失败清场及元数据口径原样保留。

公开 SDK Jobs 仍关闭，J1 缺实际能力仍拒 Accepted。本笔不定用户尚未答复的 Close、父取消或 Job 许可范围，不拿 live cause、记录字段或父 Operation 充业务 Job 授权，也不宣称恢复会重派。

各旧红与原件另簿保留。父支线绿色不能替这份组合作证；本头只有独立鲜 CI 才能验收，不覆盖旧 seal、不 rerun 旧轮遮红。

## 组合门

合并时保住两个 ASan selector、实际 required 名册、SDK-only 精确来源与宿主闭包、九份真实 FileAPI 图，以及原 CLI/full/安装消费路径。Health 从同一次 full 的 LastTest、JUnit 和单源 registration 抽五册原件，失败件先留后报错，不再跑一次 native 补证。

预核目标为 41 focused 来源、47 ASan 来源、25 installed consumer 和 365 个原生 CASE 登记。实施后须从实际表、两处 selector 与原源码重数；这些数字只记静态名册，不能冒充实跑。合完交 exact source/tree/parents、冲突处理与纯数据结果，根代理二读后才推远端独立组合 CI。

本地只读源码、Git、文档和纯 Python；不 configure、编译、CTest 或运行原生程序。
