# Memory 抽取共享底座合同

2026-10-09。第 2 步自动 Memory 前置。基线 `068db5d3`：#338 已验、已合，实际合并与验收源同树。本合同先提交，再迁代码；自动学习、Session owner 与写回尚未交付。

## 公开什么

先把 CLI 和 SDK 共用的同步抽取路迁入 `lubancode_engine`。新增 `src/agent/memory_extraction.hpp/.cpp`，namespace 为 `lubancode::agent::memory_extraction`。候选、抽取结果、严格错误、转写、分型、提示拼装、schema、Parse、Run、Finish 共用一份实现。`app/memory_extract.hpp` 保留原函数签名和名字，类型作 alias，函数作薄转接。

这片不添未接线的公开学习开关。SDK 不倒拉 app/CLI，也不另编 parser。原 cpp 后段调度账、环境开关和真实提交接口留宿主；共享抽取只产候选。公开 Off/Review/Auto、学习报告、候选管理和实际写回随后逐片接入，CLI 最终迁入同一 SDK owner。

## 谁拥有

输入消息、转写、提示、候选、正文与诊断都持值。采样沿 agent::SampleModel；Backend、取消旗和 recorder 只借到调用返回，沿 #338 先 join 后退栈。同步共享函数不添后台线程，不默认启动学习，也不写候审箱或项目正文。

提示沿原 `ModuleByPath`：先认内置模块白名单，再读显式 prompts_dir 覆盖，缺覆盖则用内置正文。未知模块返回空。不得改成任意拼路径读取，不读宿主 HOME、CLI 配置或可执行目录。

## 保住什么

用户材料 2 KiB、最终答复 3 KiB、最近六次工具合计 2 KiB；工具参数 160 字节、结果 240 字节，整体 max_bytes 与 UTF-8 边界照旧。候选至多三条，正文 8 KiB；坏业务候选跳过，字段类型坏则整次拒绝。纯 JSON、单层围栏与无歧义说明收口保留原例。多对象、坏 UTF-8、错类型、空正文和 provider 截断仍分档；日期不凭空生成。

Run 仍用 4096 token 帽和默认 45 秒预算，外部取消沿 Internal 归因。schema 留本地，不上 provider wire。保住五项 usage、provider response id 与输出终态；length/max_tokens/max_output_tokens 拒绝抽取，未知或缺失终止原因沿旧解析。任务提示、CLI 文案与稳定错误码不换名。

## 用量与来源

共享抽取接入时补齐 SampleModel 内部来源投影：usage_reported 取 assembler.usage_seen，再兼容旧 Backend 的五项非零推断。明确报零与没报分开；缓存读、写申报位及 usage_anomaly 取实际终帧，带到 SampleResult 和 OnUsageRecorded，不从正数补造来源。

recorder 的缓存 epoch 与 prefix_append_only 位沿旧缺省 0/true，再传实际缓存来源；同步抽取没有 epoch owner，不造新 epoch。主 Loop 已传这套来源，共享采样须保持同一口径。原账只读，不回写旧回执。

BackgroundCallAccounting 目前只有五项数字、usage_reported 和时长。RunMemoryExtraction 原来累加数字、覆盖本次 reported 和时长，这片保住单次抽取口径，不擅改成多次合账。随后公开学习报告须持每次请求事实，再投影合计；“有一轮报过”不能冒充“每一轮报齐”。公开 Usage 扩字段随完整公开学习报告合同接入。

## 怎样关场

共享同步入口返回前收讫 SampleModel 看门狗，失败半截 text/usage 照出账。CLI TurnMemoryExtractor 的 bool 启动口、异常出口、有界退出与晚归 ledger 另列 owner 欠账；迁函数不能冒称后台路径已安全。

实际 NewBypassBridge 还借 v2 recorder、v3 writer、V3SessionBooks、ledger 外层 io_errors_ 和 telemetry CommitObserver。只把 Impl 改成 shared_ptr，仍护不住其余借用。后续有界退休须列齐这些 owner，交完整持有或可撤销写口，并交代晚 usage 与未知提交；不凭“闭包持 shared”那句注释放行。

## 必需验收

保留 CLI 原转写、分型、提示覆盖、JSON/UTF-8/schema、候选、截断、usage、抽取器、候审与写队列案例。SDK 私有测试桥在真实 DLL 内调用同一共享入口；测试程序另份静态引擎不能代替 DLL 实例。私有头和桥不安装，不进普通模块。

新增明确报零、缺失用量、只报缓存读/写、异常来源与失败半截 usage，核实际 SampleResult 和 recorder；原非零旧 Backend 推断照留。核 native Connection 全字段及公开 Backend 可选 Usage 全零，不能靠假数证明来源。

Windows/Linux/macOS 全 CLI、SDK Lua ON/OFF、移位公开安装消费和 ASan 同源远端证据齐了才交付。依赖闭包核无 app/CLI 反向边；原测试、预算与注册名单不减。不跑本地 CI、configure、build、原生或 HTTP 验收。

## 验收收口

#339 已合入 `52b71aa8`，验收源 `5c463e9b` 与实际合并同树 `b63091fc`。原生 CI 37881597723、文档 CI 37881597738 成功。三平台全量 798/796/799 项，SDK ON/OFF 各 73/71 项；九套安装消费各 37/34 项及各 15 次真实 HTTP 均过。两路抽取各七案及实际路径标记齐；较 #338 全量只增三条登记，旧名单未删。

ASan 174 来源、1913 CASE、79290 断言过；749 份测试源码、私有附件及四份配置输入哈希吻合，清单不覆盖全部生产源码。77 册产物、1940 份原件与封存副本逐大小和 SHA256 核过。TSan 跳过，不计通过。未跑本地 CI。

本片交同步共享底座，公开自动 Memory owner 仍未交付。下一片先补[抽取线程启动失败](sdk-memory-worker-start-cleanup.md)；后台异常、可撤销旁路借用、完整持久身份与来源逐片交齐。
