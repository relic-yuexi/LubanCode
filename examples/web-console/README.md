# 参考前端(多前端外壳单·阶段 D)

一只最小 localhost Web 页,**不是产品,是验收工具**:协议面缺什么,写
它的时候全暴露(单子 §五)。它绿了,别的壳(Tauri、Android WebView)都
只是换渲染层——换掉这页的渲染层,内核一行不动。

```text
浏览器页(本目录,纯静态)
    ├─ WebSocket ─────────► lubancode app-server --app-server-ws <端口>
    │                        ├─ 同一套协议/方法表(stdio 的第二副面孔)
    │                        └─ BrowserRuntime(Playwright sidecar)
    └─ GET /artifact/<名> ──► 截图/镜像帧的字节(承载面,同端口)
```

## 四件套

- **聊天流**:`thread/start`、`turn/start`、`turn/interrupt`;吃
  `item/started|delta|completed`、`turn/usage`、`turn/context`、
  `turn/completed`。
- **页签账 + Console/Network/Downloads 面板**:`browser/status|page/*`、
  `browser/console|network|downloads/query`(断线重连凭 `sinceSeq` 补账,
  丢的行有明账)。
- **镜像 + 输入注入**:`browser/screencast/start|stop` 收
  `browser/screencast/frame`(事件里只有 artifact 引用),字节经
  `GET /artifact/<内容寻址名>` 取;协议 1.1 的快照没有坐标,"点镜像"=
  点元素清单(快照的 `[ref=eN]`)的一行,动作走 `browser/action`
  (owner 由内核按连接裁定为 `user`,不排队、不问审批)。
- **审批弹层**:`permission/request` 反向请求,四键
  accept / acceptForSession / decline / cancel。

另有事件流水面板(底账)与 base64 警报灯(协议里不该出现图片 base64,
出现了页面上明说)。

## 跑法

```bash
# 1. 起服务(回环免鉴权;要 token 见 docs/features/app-server/README.md)
lubancode app-server --app-server-ws 8765

# 2. 开页(任选其一;不须构建步骤)
#    a) 直接开文件:浏览器打开 examples/web-console/index.html
#    b) 或用任意静态服务:npx serve examples/web-console
```

页顶填端口(缺省 8765)与 token(回环免鉴权可空),点连接。全链路本地
回环,不连任何真外网服务。

## 结构

| 文件 | 职责 |
| --- | --- |
| `web_console_core.js` | 纯逻辑零 DOM:协议通道(WS 上的 AppServer 协议)、事件账 reducer、快照 ref 解析、artifact 取址。浏览器与 Node 冒烟吃同一份。 |
| `web_console_app.js` | 渲染层:账画到 DOM,用户动作折成协议请求。换掉这层不动内核一行。 |
| `index.html` / `web_console.css` | 页壳。 |

## 纪律(单子阶段 D)

- **全程只走协议**:请求/通知/审批走 WS 文本帧;图片字节走承载面的
  `GET /artifact/<名>`(与 `app_server/auth` 同级,不是协议方法面)。
  不 import 内核头文件、不读内核盘上账、不碰 sidecar。
- 冒烟 `scripts/tests/app_server_web_console_smoke.js` **直接 require
  `web_console_core.js`** 驱真 `app-server`:页上怎么走协议,冒烟就怎么
  验——端到端一幕(开页 → 看账 → 点镜像 → Agent 收 `browser.stale_ref`)
  与 token 门、artifact 口子的负例都在册。
