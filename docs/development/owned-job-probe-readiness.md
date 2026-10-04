# Owned Job：四场探针正文就绪

基 #316 `2cd01143`。Mac full job `111200867384` 实际 729 场过 728 场；继承的 Owned Job adoption 六场过五场，四场用例在读取 started 文件后未见换行。原完整日志 SHA256 `49482a311e4ba42dfb85eb889f9ba44213fbadecc5835bf27cef926804243cce` 已由唯一收件者另封。本笔不称生产出错，不称已证底层唯一根因，也不复跑旧头遮红。

## 只改夹具哪一口

`tests/support/command_limits_probe.cpp:15` 先开文件(create/trunc)，再 write、flush；`test_tool_job_owned_adoption.cpp:158` 原 AwaitFile 只等 regular file。四场约 `598–600` 随后另读 Bytes、查换行，两步之间存在尚未读全正文的窗口。只为这四场增完整正文等待，不改 probe 或生产。

新等待沿原十秒 deadline、每轮五毫秒间隔。每轮读取真实 started 字节，须见精确 tag、换行及其后非空完整绝对目录；目录用 `filesystem::equivalent(..., error_code)` 对实际场 cwd。Mac `/private` 或符号链接路径可等价，不能要求路径字节相同。缺文件、未读全、读错或路径不等价都未就绪；filesystem 查询用 error_code，转换/读取异常不逸出检查。

只有四场等待入口换用新 helper。旧 AwaitFile 全文及其它调用不改；原后续 Bytes/newline/equivalent、done 文件、错场拒绝、取消、四份 quota、限额、真实 raw/Post、Shutdown/image 退场与全部后续 ASSERT 逐字保留。六 CASE 与原 marker 不增减。不重启命令，不添睡眠帽或改 deadline，不把未就绪当成功。

## 失败留什么

等待失败可打印本次 owned last-read：状态、已读字节数、真实 filesystem error_code 与是否出现转换/读取异常。区分缺文件、正文不全、读失败、不等价。只读夹具文件，不读业务原文、不添 query/Pump、不造回执。诊断不抛，不盖原 REQUIRE；原失败材料不能倒填新值。

## 验收与交付边界

合同先单独提交，随后只交这一 CPP 候选，根代理与第二双眼审后再发布。文本门确认只有四场入口改动，其余 AwaitFile、probe、生产与六场原硬断言保留；目录及 CI 分类由根代理登记。远端 fresh 原 full、SDK-focused、ASan 完整 argv/JUnit/LastTest 再验，不拿不同头或窄册绿替本头 full。新头未跑原生，只报静态候选。本地不配置、编译、CTest 或运行原生程序。
