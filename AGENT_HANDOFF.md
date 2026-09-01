# Agent Handoff — Tauri/Android 外壳打包发布账（第一批·封机存照）

**写入时间**：2026-09-01 15:40（本批 agent 接手后续跑一轮，因主人换机封机中断，存照供新机上续跑）

## 任务

照 todos/Tauri与Android外壳打包与发布账.todo 第一批清单干。主人 2026-09-01 口径：壳不捆绑核心（NSIS/apk 不塞 lubancode.exe，连用户已装的）；版本对齐=兼容声明+检测提示；tauri-cli 勘察、NSIS 出货、三平台矩阵、签名占位流程、Android 无 JDK/SDK 如实标注。

## 本轮真进展（对照上一份 handoff）

上一份 handoff 的"断点现场"已过时——那份写的是前一只 agent 的起步线。本轮实际干成了：

1. **tauri-cli 勘察有结论**：本机 `cargo tauri` 不在，但 npm 全局装着 `@tauri-apps/cli@2.11.4`（`npx --no-install @tauri-apps/cli` 直用）。`tauri info` 全绿（WebView2 151 / MSVC 2022 / rustc 1.89）。NSIS 工具链缓存在 `C:\Users\<用户>\AppData\Local\Tauri\NSIS`（首跑自动下载，新机要网）。
2. **NSIS 安装包真出货两枚**：`npx @tauri-apps/cli build` 全量 6m18s 编过；钉死 bundle 配置后 `tauri bundle --bundles nsis` 快速重出（cargo 不重编）。产物在 `examples/shells/tauri/target/release/bundle/nsis/LubanCode Console_0.1.0_x64-setup.exe`（1,759,966 B，SHA-256 `817dcb33b33f9151a5acba577858835a07db3fe20b719d562beae0dec0f990c6`）。target/ 已被 .gitignore 挡住，产物不进 git。
3. **版本对齐口径已落码**：`web_console_core.js` 加 `PROTOCOL_COMPAT {min:'1.1',max:'1.1'}` + `checkProtocolCompat()`（纯逻辑，major/minor 皆闭区间，没验过的不冒充兼容）；`web_console_app.js` 握手对表，不合转 `.state.warn` 状态条摆人话提示（检测提示不拦截），断线/失败清 compat 账。三只前端（浏览器页/Tauri/Android）同一份声明。
4. **bundle 配置钉死**：`tauri.conf.json` 加 `createUpdaterArtifacts: false`（升级通道不进首版，占位裁决）+ `windows.nsis`（installMode=currentUser、languages SimpChinese/English、installerIcon）。targets 维持 ["nsis"] = 首版只 Windows，矩阵定界已隐含写死，文档面待补。
5. **壳冒烟扩册**：`app_shell_smoke.js` 加第 [2] 节协议兼容声明 8 案，节号顺延，实测 **49/49 全绿**（原 41 案零回归）。三个改过的 JS `node --check` 全过。

## 装验四步做到哪一步（封机时刻实录）

四步 = 静默装（`setup.exe /S`）→ 验文件 → 起进程 → 卸干净。**只走了第一步且未成**：/S 起了安装器，bash 包装等回车 120s 超时转后台，封机令到被杀。实测收尾检查：`%LOCALAPPDATA%\Programs\LubanCode Console` **不存在**、无 setup 进程残留——**机器干净，无需卸载补救**。接手后四步从零重走（注意 /S 的返回要 `start /wait` 或 PowerShell `Start-Process -Wait` 收，别裸等管道）。

## 未提交改动（本 commit 一并入库）

`examples/shells/tauri/tauri.conf.json`、`examples/web-console/web_console_core.js`、`examples/web-console/web_console_app.js`、`scripts/tests/app_shell_smoke.js`、本文件。

## 剩余步骤清单（接手顺序）

1. `git merge main --no-edit`——本分支 HEAD a5e74c3d，**main 已挪到 00b341a0**（0.26.157 后有封机存照提交），先追平。
2. 真机装验四步（见上，注意连的是用户已装内核、不捆绑）。
3. `node scripts/tests/app_shell_smoke.js`（须 49/49）与 `node scripts/tests/app_server_web_console_smoke.js --binary <lubancode.exe>`（本机曾用主仓 `build/release/Release/lubancode.exe`；新机先构建）双双不红。
4. `ctest -C Release` 全量零回归：worktree `build/` 只 configure 过（exit 0，_deps/*-src 已从主仓拷入防卡网），**零目标编译**；接手先 `cmake --build build --config Release --target lubancode lubancode_tests` 再 ctest。退出码直收不经管道；ctest 设临时 USERPROFILE；缺 rg 从主仓 build/release/libexec 拷；LNK1104 假锁 taskkill 清 lubancode_tests.exe 孤儿重链再判。
5. 还没动的第一批件：GH Actions 壳流水线（Windows 出 NSIS、ubuntu 出 APK——ubuntu runner 自带 JDK17+SDK，仓库无 gradle wrapper，用 `gradle/actions/setup-gradle` 钉版本跑 `gradle :app:assembleDebug`；两个冒烟挂进 ci.yml windows 腿复用现建 exe）；两冒烟进 CI 必跑（单子 §四.3）；Android `signingConfigs` 接线（keystore.properties gitignore 掉，keytool 生成命令写文档）；shells README 补版本对齐/矩阵定界/签名占位（无证书 SmartScreen 如实告知）/升级通道裁决（不进首版+后补路：tauri signer keygen、TAURI_SIGNING_PRIVATE_KEY、latest.json）；PACKAGING.md 或 README 节落打包流程脚本。
6. 勾单子第一批 checkbox + 状态行（未验项注明工具链原因：Android JDK/SDK 缺、装验中断）。
7. 完工单笔 commit（中文+Co-Authored-By: Claude <noreply@anthropic.com>），不 push，版本号不动。**本轮是封机存照 commit，单子未勾——半成的勾是撒谎。**

## 接手铁律

1. 红线：不动 src/ 内核、channel/、insights/、telemetry/、workspace 单文件；不碰 app-server 协议面；构建产物/签名材料不进 git；不写真密钥。
2. `examples/web-console/` 是参考前端唯一事实源，壳不复制它（frontendDist/assets.srcDir 直指）；改它要过两册冒烟。
3. WSL 语法验新改文件。禁例：NSDMI 默认实参、atomic<shared_ptr>、CAPTURE 单参。
