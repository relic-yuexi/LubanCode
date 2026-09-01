# 外壳孵化(多前端外壳单·阶段 E)

`examples/shells/` 装两只壳:**Tauri 桌面壳**与 **Android WebView 壳**。
都不是产品——是"任何外壳零内核改动、只走 AppServer 协议就能呈现完整
浏览器工作台"这条验收线的证物(单子 §七阶段 E)。参考前端
(`examples/web-console/`)是唯一的前端事实源:两家壳都不复制它,
直指同一份代码。

```text
终端 TUI ─┐
Tauri 壳 ─┤ examples/shells/tauri(装页、开窗、CSP)
纯 Web 页 ─┼── examples/web-console(参考前端,四件套+触屏外套)
Android ──┘ examples/shells/android(全屏 WebView,assets 直指参考前端)
              │
              ▼ 全程只走 AppServer 协议(WS 承载)与承载面
              ▼ (GET /artifact/<名> 取字节,base64 永不进协议)
        lubancode app-server --app-server-ws <端口>
              └─ BrowserRuntime(Playwright sidecar)
```

## 两家壳各自怎么装参考前端

| 壳 | 装法 | 零复制口子 |
| --- | --- | --- |
| Tauri | `tauri.conf.json` 的 `build.frontendDist: "../../web-console"`——编译期把参考前端嵌进二进制。 | frontendDist 相对路径 |
| Android | `app/build.gradle.kts` 的 `assets.srcDir("../../web-console")`——打包期把参考前端打进 APK 的 assets。 | Gradle sourceSets |

浏览器 Tab 两家都走**镜像路**(单子 §3.2:镜像先行,直连后议):页签
画面 = screencast 帧经 artifact 口子取字节;输入 = 点元素清单的行
(`browser/action`,owner 由内核按连接裁定为 `user`)。

## 触屏手势怎么折输入(单子 §3.3)

折算层在参考前端里:`examples/web-console/web_console_touch.js`
(纯逻辑 `classifyTouch`/`mapGesture` 零 DOM,Node 冒烟与页上同一份;
DOM 接线只在浏览器)。

| 手势 | 折成 |
| --- | --- |
| tap | 原生 click(选中元素清单行) |
| double-tap | 合成 dblclick → `browser/action kind=click`(owner=user) |
| long-press | 合成 click 选中 + 聚焦 type 输入框(注入 `kind=type`) |
| swipe | **不折协议**——协议 1.1 动作面只有 click\|type\|select\|wait,没有 scroll;滚动交给原生,壳不冒充 |

## 跑法

### Tauri 壳(要 Rust 工具链;Windows 出 WebView2,系统自带)

```bash
# 1. 起服务(另开一窗)
lubancode app-server --app-server-ws 8765

# 2. 起壳(本目录)
cd examples/shells/tauri
cargo run            # 开发跑;cargo build --release 出发布二进制

# 3. 页顶填端口 8765,点连接
```

CSP 只给回环开了口(`ws://127.0.0.1:*`、`http://127.0.0.1:*`)——WS
与 artifact 字节口子照阶段 D 的设计走,不受同源策略拦(WS 无 CORS、
字节走 `<img src>`)。非回环连接要 token,见
`docs/features/app-server/README.md`《WS 承载》。

### Android 壳(要 JDK 17 + Android SDK 34;本仓开发机未装,脚手架如实标注未真机验)

```bash
# 1. 起服务(与壳同机或 adb 可达的机器)
lubancode app-server --app-server-ws 8765

# 2. 本机反向代理(单子 §3.3 首版口径:只做 adb 反向)
adb reverse tcp:8765 tcp:8765

# 3. 构建安装(Gradle wrapper 未随仓走,本机装了 gradle 即可)
cd examples/shells/android
gradle :app:assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

明文账(Android 9+ 拦回环明文):`network_security_config.xml` 全局
放行明文——NSC 的 `<domain>` 按主机名匹配、不认 IP 字面量,回环放不了
小灶。参考壳不连任何非回环明文服务;真产品壳另议(见打包发布账单)。

## 工具链勘察账(2026-09-01,本仓开发机)

| 工具 | 版本 | 状态 |
| --- | --- | --- |
| cargo / rustc | 1.89.0 | 在,`cargo check` 与 `cargo build` 全绿 |
| node / npm | 24.0.0 / 11.12.1 | 在,冒烟跑的就是它 |
| JDK | — | **未装**,Android 侧只验到脚手架与静态账 |
| Android SDK/NDK | — | **未装**,同上 |

2026-09-02 换机复勘(打包发布账第一批装验机):rustc 1.89 在;
tauri CLI 走 `npx --no-install @tauri-apps/cli`(npm 全局 @2.11.4);
NSIS 工具链缓存随 `%LOCALAPPDATA%\Tauri\NSIS` 在(新机首跑会自动
下载,要网);JDK/Android SDK/gradle 仍未装——Android 出包走 CI。

## 验证口径

- 壳侧 JS(触屏折算)与两家壳的静态账:Cargo/Gradle/清单/CSP/资源
  引用——`node scripts/tests/app_shell_smoke.js` 一册全验(不依赖
  Android/Tauri 工具链,缺文件明 FAIL 不冒充)。
- Tauri 壳:`cargo check`/`cargo build` 过即编译级验证;开窗一幕要
  真机桌面,归打包发布账单里做。
- Android 壳:JDK/SDK 未装,`gradle assembleDebug` 与模拟器/真机一幕
  未验——如实记账,不伪造。
- 阶段 D 既有册不红:`scripts/tests/app_server_web_console_smoke.js`
  照跑(参考前端动了 index.html/app.js/css,冒烟须全绿)。

## 版本对齐

壳版本(0.1.0)与 lubancode 内核版本(0.26.x)**解耦**,各发各的,
不互相牵引 tag。对齐靠协议兼容声明,不靠版本号:

- 声明只此一份,在参考前端里:`examples/web-console/web_console_core.js`
  的 `PROTOCOL_COMPAT = { min: '1.1', max: '1.1' }`(major/minor 皆
  闭区间,没验过的不冒充)。三只前端——浏览器页、Tauri、Android——
  吃的是同一份代码,声明天然同步。
- 握手时对表:`initialize` 回的协议版本对声明范围跑
  `checkProtocolCompat()`,不合就把状态条转 `.state.warn`、摆一句
  人话提示(内核协议 X,本前端声明兼容 Y——请对齐所连内核版本再试)。
  **检测提示,不是拦截**:连不上号的内核照连,壳只报账,不替用户做主。
  断线/失败清 compat 账,重连重对。
- 发布节奏:内核协议动 minor,前端的声明窗口跟上、三册冒烟过绿,
  壳才发新包。壳自己发版打 `shell-v*` tag,与内核 `v*` 分流。

## 打包流程(第一批实录)

壳不捆绑内核:NSIS 安装包与 APK 里都没有 lubancode.exe,装的是壳,
连的是用户已装的 app-server(装法见上文"跑法")。

### Windows:NSIS 安装包

```bash
cd examples/shells/tauri
npx --yes @tauri-apps/cli@2.11.4 build --bundles nsis   # 全量(约 6 分钟)
npx --no-install @tauri-apps/cli tauri bundle --bundles nsis   # 快速重出包,cargo 不重编
```

产物:`target/release/bundle/nsis/LubanCode Console_<版本>_x64-setup.exe`
(0.1.0 实测 1,759,966 B;`target/` 不进 git)。NSIS 工具链缓存在
`%LOCALAPPDATA%\Tauri\NSIS`,tauri CLI 首跑自动下载,新机要网(不通
试代理)。bundle 配置已钉死在 `tauri.conf.json`:`targets=["nsis"]`、
`installMode=currentUser`、中英双语、`createUpdaterArtifacts=false`。

### 真机装验四步(2026-09-02,Windows 11 实录)

1. **静默装**:PowerShell `Start-Process -FilePath <setup.exe>
   -ArgumentList '/S' -Wait -PassThru` 收 `ExitCode=0`。/S 起的是
   安装器,别裸等管道收不到返回。
2. **验文件**:`%LOCALAPPDATA%\LubanCode Console\` 下两件——
   `lubancode-console-shell.exe`(7,559,680 B,Tauri 单文件,参考
   前端嵌在里头)与 `uninstall.exe`;HKCU 卸载项在(DisplayName=
   LubanCode Console,0.1.0)。注意 currentUser 模式落点是
   `%LOCALAPPDATA%\<产品名>`,不带 `Programs\`。
3. **起进程**:exe 起后 6 秒仍活,`MainWindowTitle` = "LubanCode
   控制台(参考壳·镜像路)"——窗口真开了。页顶要人手点"连接",
   连 8765 走四件套的交互一幕未走(无 UI 自动化),归单子 §二.4。
4. **卸干净**:`uninstall.exe /S` 收 `ExitCode=0`;安装目录、HKCU
   卸载项、桌面/开始菜单快捷方式、进程,全无残留。

### Android:APK

```bash
cd examples/shells/android
gradle :app:assembleDebug      # debug 包(直装侧载)
gradle :app:assembleRelease    # 配好 keystore.properties 后出签名包
```

本机无 JDK/SDK,如实未验;CI(ubuntu runner 自带 JDK 17 + SDK 34)
出 debug APK。AGP 8.5.2 钉在 `settings.gradle.kts`,gradle 8.9 钉在
CI(`gradle/actions/setup-gradle`)——仓库无 wrapper,版本两头钉死。

## 矩阵定界

首版只 **Windows x64**:`bundle.targets` 写死 `["nsis"]`,WebView2
系统自带,壳不携带运行时。macOS/Linux 不进首版——要进时扩
`make_icons.js` 生成 icns 等资源、`targets` 加平台、CI 加腿,壳代码
不动。Android 侧只出 debug APK 侧载,Play 上架不在账上。

## 签名占位

**Windows**:没有 Authenticode 证书,安装包不签——SmartScreen 会拦
("更多信息 → 仍要运行"),如实告知用户,不冒充已签。证书到位后
tauri CLI 读 `WINDOWS_CERTIFICATE_THUMBPRINT` /
`WINDOWS_CERTIFICATE_PASSWORD` 环境变量自动签;CI 注入即 secrets
传环境变量,流水线结构不用改。

**Android**:

```bash
keytool -genkeypair -v -keystore lubancode-console.jks \
  -alias lubancode-console -keyalg RSA -keysize 2048 -validity 10000
```

`examples/shells/android/keystore.properties`(不进仓,.gitignore 挡):

```properties
store.file=lubancode-console.jks
store.password=<库密码>
key.alias=lubancode-console
key.password=<密钥密码>
```

接线已落 `app/build.gradle.kts`(`signingConfigs` 读上件);文件不在
时 `assembleRelease` 退回 debug 签名并打横幅——仅供本机验装,发布
必须配真密钥。密钥库本身(jks/keystore)同样不进仓。

## 升级通道(首版不进,裁决已记)

首版不带自动升级:`createUpdaterArtifacts=false`,壳升级靠重装。
后补路(真做时照走):

1. `npx @tauri-apps/cli signer keygen -w <私钥路径>` 生成更新签名
   钥对;公钥进 `tauri.conf.json` 的 updater 配置,私钥离线保管。
2. CI 注入 `TAURI_SIGNING_PRIVATE_KEY` /
   `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`(secrets),
   `createUpdaterArtifacts` 翻 `true`,出包带 `.sig`。
3. 静态托管 `latest.json`(平台、版本、下载地址、签名),壳轮询
   比对提示升级。

版本对齐先于升级通道:协议声明窗口没对上,升了级也白升。

## CI

- `.github/workflows/shells.yml`:壳流水线。windows 出 NSIS、ubuntu
  出 APK,两条腿都先跑 `app_shell_smoke.js` 静态门;产物走 workflow
  artifacts(不进 git),打 `shell-v*` tag 时附到 GitHub Release。
- `.github/workflows/ci.yml`:windows 腿在内核 Build + Test 后挂两册
  壳冒烟(`app_shell_smoke.js` 静态册 + `app_server_web_console_smoke.js
  --binary build/Release/lubancode.exe` 端到端册),复用现建 exe,
  不另编。

## 打包与发布

剩余账(真机交互一幕、Playwright 直连路、局域网配对等)在
`todos/Tauri与Android外壳打包与发布账.todo`,做完一批勾一批。
