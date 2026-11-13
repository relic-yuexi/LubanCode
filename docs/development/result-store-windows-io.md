# 结果仓 Windows 原生路径入口窄修

本笔基私有组合 `bef6663b3ffd7401521208f0406fbb86a10fd666`，先定合同，再实现；不改共享分支。
SDK 诊断头 b58 与组合 bef 都实报首工具 `tool.capture.persist_failed`，底层为结果仓打不开临时文件；子失败、Finish 已提交、Close 已关闭，父捕获障碍尚未挂上。
bef 完整 Windows 日志 SHA256 `24e6b3fe4bfbb53ff1d59b6544677f4efe991b7fff05b1643473dff0337b1ed7`，3553–3572 行，原路径摘要已截短。
这份日志证实临时文件 open 失败；尚未量到失败子路径，不据父目录 194 字符断言 MAX_PATH 已坐实。

## 唯一改点

复用现有 `platform::FileIoPath`，只在 ResultStore 原生文件入口转换：Open 的建目录/枚举，以及 WriteImmutable 的存在性检查、临时文件打开、rename 与失败清理。
临时后缀先接完，再独立转换；逻辑 final path 与 temporary path 分开留存。原生扩展拼法不写进结果 ID、artifact ref、metadata、listing 返回值或模型 display path。
Persist 的通道、metadata 与 PersistListing 仍共走唯一 WriteImmutable，不添测试镜像或第二结果仓。
保原号序、不可变名冲突拒绝、正文/hash/ref 与原权限、预览/捕获预算、错误分类。不开新重试，不换 AtomicWrite，不宣称新增 fsync 或跨进程不可变提交保证。
POSIX `FileIoPath` 原样返回；Windows 继续沿现函数对 reserved/trailing-dot-space 与显式 namespace 的规则，不另改路径解释。
本笔不把真实 child ReportedFailed 擅自改为 Unknown，也不改父 Cancel/Close 或执行状态投影。

## 验收与界限

复用 `tests/unit/trajectory_v3/test_v3_result_store.cpp` 原 17 TEST_CASE，在既有结果仓用例内加两枚 Windows SUBCASE，不跳过、不删原断言。
一枚用真实超过 Win32 普通路径帽的 target，验通道/metadata/listing 成功、原逻辑 refs、重开续号与同名拒绝不覆盖；另一枚精确 target 247 / `.tmp` 251 原生字符，验临时后缀跨 FileIoPath 的 248 字符阈值后仍正确发布。
夹具自行用原平台 FileIoPath 建目录、读真实字节，打印两枚准确路径计数和实际成功标记；不能靠短暂缩短根目录、放大预算或放宽状态断言染绿。
该生产原册进入 SDK focused 与必需 ASan 门，原来源并集照留；三平台 full、六套消费者与 SDK/Host/Worker/Runner/实际依赖图须验同一鲜头。
合同与静核不充原生通过；Windows 实际故障是否就此消失，须等新远端消费者同场回执。旧 b58/bef 失败原件另封，不拿旧部分绿替新头。
本地不 configure、不编译、不 CTest、不运行原生夹具；只静态、纯数据和文档检查。

## SDK-only 原册登记补口

`2f3d8bc7` 远端 Linux 与 macOS 安装消费均已跑过；SDK-only 随后在 dependency boundary 拒绝新增原册 `tests/unit/trajectory_v3/test_v3_result_store.cpp`。实际报告仅此一条：该来源已进 CMake focused 清单，却漏进 `SHARED_SDK_TEST_SOURCES`。
下一笔只登记这一条完整路径。仅 `BUILD_TESTING=ON`、`lubancore_sdk_tests` 所有者可用；OFF、别的 target、相邻 trajectory 原册、递归宿主头仍须拒绝。
用合成 File API 纯数据反例核这四条拒线，不跑 CMake 或原生进程。原 17 用例、两枚 Windows 路径标记、25 册 focused、30 册必需 ASan 与消费断言照留。
漏项旧头另封原报告；新头重走远端三平台，不拿这份清单修补充原生通过。
