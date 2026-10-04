# 私有 ASan 编译画像

这笔只收 CI 构建范围。公开 SDK、生产源码、测试正文与默认构建不改。

旧 ASan 腿以 `Release`、GCC、`-fsanitize=address`、四路并行构建 `lubancode_tests`。42eca2c3 在 90 分钟处仍未链接这份程序。原日志记下 686 册测试编译起头，其中 534 册不在两条 ASan 选择器内；SDK、Host 两份参考程序也随依赖先后构建。

`LUBANCODE_ASAN_TEST_PROFILE` 默认 OFF。只有显式 ON 才启用；须同时启用 CLI、SDK、Lua、测试及地址消毒器。CI 先从原 workflow 两条实际选择器和全册路径生成本轮清单，再把绝对清单路径传给私有 CMake 入口 `LUBANCODE_ASAN_PROFILE_FILE`。缺清单、重复源、外来路径或不满足画像前提，都拒绝配置。

本轮两条选择器并集为 154 册。第一条 required60 只是其中一层；第二条另含 94 册，必须实跑，不能裁掉。选择器、全册 CTest 登记、原程序名、完整参数、90 分钟 job 预算、四路并行、每册测试帽、环境与资源锁均沿原值。只裁 `lubancode_tests` 内未被选中的编译正文。其它登记在这套私有画像里不构成全套执行证据；CI 仍只运行原选择器。

原六份 support、PCH、六份私有 SDK 实现、六份公开消费 helper、生产库、探针定义及原夹具依赖都保留。SDK、Host 参考 target 仍定义，并保持 `EXCLUDE_FROM_ALL`；仅在画像内断开它们与总测试程序间那两条冗余构建依赖。SDK 搜索与命令限额探针仍显式构建。`LUBANCORE_TEST_JOB_POST_SDK=1` 和两枚探针路径仍挂在总测试程序，不能随参考 target 一并跳过。

清单与校验都归 CI，不添公开 SDK ABI。默认 combined、SDK-only 与 Lua 两套画像继续走原图；剥去这笔窄接线后，原 CMake、选择器和旧验收函数须逐字还原。

远端先用实际 File API 核总程序编译源、原支持件与私有实现、目标类型、生产依赖、探针路径及参考 target 定义。随后对读两份实际 CTest 登记、完整原生参数、JUnit 和 LastTest：两条选择器并集必须与实际编译清单相等，154 册各跑一次、无跳过、无重复、无失败。原六案正文及各模块完成标记继续过旧门。这套画像不拿文字清单冒充实际构建，也不保证未经远端实测的节省时长。

本地只读源码、生成文本、跑 Python 数据反例和 AST/YAML/Bash 语法检查。配置、编译、CTest 与原生程序均交远端 CI。
