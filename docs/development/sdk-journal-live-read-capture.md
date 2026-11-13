# SDK 活场 Journal 读取合同

本笔只迁十处已经开场的读取：SDK Core 六处、Command Jobs 三处、Job
operation 绑定一处。每处仍在原相位读账；工具结果缓存、Memory 收尾、Job
完成泵若先追加新事实，随后读取必须看见这些事实。不得拿开场快照代管整场。

## 读什么，谁持有

内部读口从当前 `V3Writer::CaptureJournal()` 取得实际文件锚与不可变字节，
交给 `ReadV3LedgerCaptured()` 验链、投影。它不另建 File reader，不导出公开
Provider，也不引数据库。临时捕获只在这次读取存活；核验结束后 checked
Close 释放锚。捕获、解析、关闭失败均回到调用者原错误或结果入口，解析失败
优先保留。返回账只持值，不持 Writer、SessionService、Runtime 或公开 Session。

原活场读取没有整账字节帽，本笔仍沿不限量策略，不悄悄添新默认帽。锁定恢复
沿原 `RecoveryReadLimits`；128MiB 管逐份恢复材料，不能称总驻留上限。分行、
JSON 投影和旧恢复值副本仍可能同时存活。后续外部 JournalStore 与驻留预算各
有合同，不在这里占空接口。

## 谁调，何时读

恢复 system 与装载旧操作在实际 SessionService 开好 Writer 后捕获；这不证明
开场预检、恢复和 continuation 已共用一份材料。五处模块 Prepare 预读及原锁
移交另收一笔，先后顺序和首错误照留。

Complete 中 Memory recall、Memory save、child report 各守原读点，只复用原代码
已经复用的账。Job Post 在实际 invocation 追加后读；完成泵在原 coordinator
追加后读。Job 绑定仍核实际前缀、父场、turn 和已采用回执，不因换读口省校验。

关闭后的公开结果查询沿已冻结索引和独立 named-result 读能力，不重开主账。
本笔不把内部账对象变成权限凭据，也不改变默认 Preview 或 Full 双开关。

## 验收

既有 Journal 七条 owner 案、三条公开 SDK 案及真实主账 guard 保留；原 Command
Jobs、Memory、子 Agent 恢复与关闭验收照跑。现有 owner 案追加真实前后读取：
第二次捕获看见后追加消息，旧捕获仍保旧字节；坏链或缺文件必须报错。原案数、
时限、模型与工具计数不改。

源码与数据门可本地核；配置、编译、CTest 与原生程序只在远端 CI 跑。当前
Named 发布未知和 Journal 材料核验修复先收口，本笔单独留私有候选，不能借旧
CI 宣布通过。
