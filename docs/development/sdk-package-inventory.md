# SDK Package 显式根有界盘点合同

本批基线 `335b57f5`。公开 `packages::v1::InventoryExplicitRoot(InventoryInput) -> Result<Inventory>`，只盘宿主明给的一只包根。先交合同，再写源码。原 `AnalyzeManifest` 和 CLI 十六案照旧；不接挂载、信任、组件执行、Lua 或跨包引用分析。

根路径须为非空、无 NUL、合法 UTF-8、绝对且词法规范的目录路径。根本身不能是 symlink、junction 或其它 Windows reparse point。检查根本体前先去末尾分隔符，保留文件系统盘根；带尾分隔符不能绕过链接检查。返回原输入根写法。调用开始把根解析为持值的 canonical 根；正常父路径别名只在这时解析。起止都复核原路径仍映到同根。宿主须在调用期间保持这棵树静止。

`InventoryLimits` 默认并以同值封顶：4096 条目录项、32 层深度、4096 字节根及相对路径、单文件 16 MiB、总读取 64 MiB。宿主可调低，不能放开。目录、未知项、零字节文件都计条目；根为深度零，直接子项为一。根清单另守现有 256 KiB 帽。无文件也合法；零预算无效。上限检查用减法，越帽即报错，不交残账或残账指纹。

捕获先收全体目录项，按 UTF-8 相对路径字节序排序，再逐个读取常规文件。所有目录与文件都查链接及实际类型；Windows 另查 reparse 属性，查失败即失败。包内路径只收规范的 `/` 相对写法，拒绝绝对路径、`..`、反斜杠和制表、换行等指纹分隔字符。非常规文件不打开、不省略，整次报错。迭代器构造及递增出错都报错。

文件读取复用 `ReadBoundedRegularFile`：末端不跟链接，拿到句柄后确认常规文件，遇 FIFO 或设备不阻塞等待。每只文件最多读到帽加一字节，用多出的一字节辨越帽。零字节文件的摘要仍是真实 SHA-256。根清单解析、文件大小和逐文件摘要同吃这一遍实际字节；二进制资产无需合法 UTF-8。整包指纹沿冻结 `PackageLedgerFingerprintV1` 的材料格式。

读前后核文件大小、类型、修改时间及路径映射；结束后重枚举目录账，核条目、类型、大小、时间和根映射。发现漂移报 `sdk.package.source_changed`，不重试，不把坏账包装成完整包。前后复核只是一轮有界观察，不能证明全树原子快照，也不能抵挡恶意并发祖先目录置换。盘点值不能充当安全沙箱、发布者认证、信任凭据或冻结执行材料。

返回值完整持值：原根、canonical 根、排序目录账、文件路径/实际大小/摘要、整包指纹、七类发现项、根清单分析和诊断。根清单缺失或 YAML parser 拒绝仍可列其它账；缺失与空文本分开记。根清单含非法 UTF-8 或 NUL 时沿旧分析入口报 `sdk.package.invalid_input`，整次失败，不扩旧接受面。坏清单不造 canonical 组件身份。显式宿主版本和平台复用原分析门；省略不查环境。文件帽、读失败、路径失败与变树是整次调用错误，不伪装成清单语法问题。

七类入口沿 CLI 目录规矩共用中立布局材料。SDK 只认捕获账里的常规入口文件；同名目录给诊断，不列成入口。Prompt Profile 沿目录列名。发现项不证明正文有效、local-id 合规、wire 名合规或引用闭合。组件 parser、Channel、Workflow、Plugin、catalog、MountPlan 与信任宿主不进入 SDK。

原 `inventory.cpp` 的中立路径/诊断规则可归唯一 engine owner，旧扫描与旧盘点仍留原接口、原接受语义。必要的共用布局材料与新 `inventory_snapshot` 也只归 engine；公开薄层只接输入门与持值投影。边界门只给精确源和头开例外，不放开 `package/` 整个前缀，不复制清单或组件 parser。

调用同步完成，文件句柄、迭代器、单文件临时字节和内部观察点随调用释放。没有 Session、注册表、writer、回调存活期或后台线程。返回值可在输入销毁、源树删除后继续查询。测试观察点只在内部捕获入口接入，实际增删改名或替换文件树；公开 SDK 不露故障开关。

新增 `test_lubancore_package_inventory.cpp` 固定八案：`owned`、`fingerprint`、`manifest`、`input`、`limits`、`links`、`changed`、`isolation`，各案完成后只打印一枚 `[sdk-package-inventory-path]` marker。真实 Windows junction、POSIX 文件/目录 symlink、实际变树、帽正反边界和源删除后的持值都进原生验收。安装消费者仅依赖公开 SDK/STL，迁位后现造包，调用盘点，删除包，再查持值。原清单八案、原 parser 十五案和 CLI 原盘点十六案不缩减。

所有 configure、编译、CTest 和原生程序都交远端三平台 CI。本地只读源码、查文档、核文本与纯数据。CMake owner、源码闭包、安装消费路由、两份 ASan 选择器和证据门由根任务统一接线；每份新名册都核实际完整 argv、非零全过断言、完成 marker、JUnit 与 LastTest。
