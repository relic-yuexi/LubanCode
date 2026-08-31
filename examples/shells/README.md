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

## 打包与发布

另立单:`todos/Tauri与Android外壳打包与发布账.todo`(签名、CI、版本
对齐、自动升级、Tauri 直连 Playwright 调试端口的路,都在那张单上)。
