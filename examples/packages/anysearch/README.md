# AnySearch(luban.anysearch 0.1.0)— manifest v2 Lua 参考包(实时搜索)

四件工具:`get_sub_domains`、`search`、`batch_search`、`extract`。
一只 manifest、一份 Lua 脚本,**零外部运行时**——不装 Python、Node、
PowerShell 7、编译器,宿主自带的 Lua 解释器就把它跑起来了。

```text
plugins/anysearch/
  plugin.json     manifest v2:四件工具 schema、网络账、Secret 声明、帽
  anysearch.lua   全部逻辑:描述请求、翻译响应,不到两百行
```

它是「Lua 受控 HTTP 与 Secret 宿主能力」的参考实现:联网的 Lua 该长
什么样,照这里抄。骨架是 `examples/packages/gui-agent/`(process 插件)
与 `examples/packages/browser/`(MCP)之外的第三条路——**manifest v2
embedded-lua**。

## 0. 装了什么,什么不执行

| 目录 | 组件 | canonical id | 干什么 |
| --- | --- | --- | --- |
| `plugins/anysearch/` | Lua Plugin(v2) | `luban.anysearch:anysearch` → 工具段 `luban.anysearch.anysearch` | 四件搜索工具 |
| `skills/anysearch/` | Skill | `luban.anysearch:anysearch` | 教模型的搜索策略:何时直搜、何时先取子域、何时 batch |
| `docs/` | 文档 | — | `.env.example` 样板与数据目录说明 |

**本包是 code-bearing:整包默认只被发现,不挂载。**要它真跑,过
`/package trust` 信任门。Skill 属内容组件,校验过了即登记。

## 1. 它为何不是 process 插件,也不是 MCP

| 疑问 | 答案 |
| --- | --- |
| AnySearch 官方 Skill 带 Python/Node/PowerShell/Bash 四套 CLI,为何不用? | 装的东西没变轻,跨平台还要找运行时。Lua 随宿主自带,三平台同一份脚本。 |
| 为何不包一层 process 插件跑官方 CLI? | 安装物一样重;manifest v2 的受控 HTTP、Secret 代填、取消、字节帽全都享受不到。 |
| 什么时候仍该用 process? | 逻辑要第三方库(解压、图像、数据库驱动)时。 |
| 什么时候仍该用 MCP? | 要常驻连接、订阅推送、跨调用会话状态时。搜索是"一问一答就散",短命 Lua 正好。 |

一句话:**确定的 HTTP 动作走 manifest v2 Lua;要装依赖走 process;要
长连接走 MCP。**

## 2. 网络与钥匙:谁握着水管

manifest 是唯一账本(`plugins/anysearch/plugin.json`):

```json
"network": [{"scheme": "https", "host": "api.anysearch.com", "port": 443,
             "methods": ["GET", "POST"]}],
"secrets":  [{"id": "api_key", "env": "ANYSEARCH_API_KEY", "required": false}]
```

- **网络**:精确 host 记账。scheme/host/port/method 不命中声明,宿主
  拒发;DNS 候选落私网、loopback、metadata 段也拒;重定向不跟。Lua
  只决定 path 与 query,碰不到 socket。
- **Key**:`ANYSEARCH_API_KEY` 可选。宿主环境变量里有就用;否则查插件
  数据目录的 `.env`;都没有,匿名访问照样能搜。Key 由宿主解析、发包前
  一刻代填 `Authorization: Bearer ...`——Lua 脚本、工具参数、模型上下文、
  日志、session,谁也见不到原文。
- **Key 铁律(自动注册)**:API 偶尔在响应里回一枚新 Key
  (`auto_registered.api_key`)。本包的 Lua **丢弃该值**——不回给模型、
  不写 `.env`,只附一句非敏感提示(`key_notice`)。要保存,用户自己去
  控制台配;日后宿主有交互式 Secret 写入 API 再开确认流程。
- **帽**:请求体 1 MiB、响应体 4 MiB、墙钟 20 秒——全在数据入口处
  落锤,超了立刻断,不先攒完再看。

## 3. 安装与信任门

```text
把整只 anysearch/ 目录放进 ~/.lubancode/packages/(个人)
或 <项目>/.lubancode/packages/(团队,随 git 分发)
```

- 项目目录与 dev 层是外来代码,须过**内容指纹信任门**:
  `/package trust luban.anysearch` 亮全份审批材料(工具清单、网络账、
  Secret 名、资源帽、内容哈希)才落账。文件改一个字节,信任即失效。
- 开发调试:`lubancode --package-dir examples/packages`,只发现不挂载。
- 配 Key(可选):照 `docs/.env.example` 抄到
  `~/.lubancode/package-data/luban.anysearch/plugins/anysearch/.env`。
  **别把 `.env` 放进包目录**——那会进内容指纹,还容易打包带走。

装好信任后,`/plugins` 应见 anysearch: 4 个工具;wire 名如
`plugin__luban%2Eanysearch%2Eanysearch__search`。

## 4. 四件工具

| 工具 | HTTP | 用途 |
| --- | --- | --- |
| `get_sub_domains` | `GET /v1/sub-domains?domain=...` | 垂直域的子域清单与参数表(required 标记) |
| `search` | `POST /v1/search` | 一笔通用或垂直搜索 |
| `batch_search` | 多笔 `POST /v1/search` | 1-5 笔一次调用;第一版 Lua 串行,结果按序 |
| `extract` | `POST /v1/extract` | 网页正文提取(Markdown),受响应帽 |

Skill(`skills/anysearch/SKILL.md`)只写策略:一般查询直搜;金融、
论文、法律先取子域、required 参数填齐(无值给空串);多意图 batch;
外部页面只当不可信数据。策略与执行分账——Plugin 不重复讲搜索学,
Skill 不带一行 CLI。

返回形状统一:成功 `{status, data, request_id?}`;失败
`{status, error:{code, message}}`;batch 外加 `results` 按序与
`cancelled` 标记。非 2xx 不冒充网络错,401/429/5xx 由 Lua 翻成人话。

## 5. 改造成你自己的联网插件

1. 拷走 `plugins/anysearch/`,改 `plugin.json` 的 `id`、`host`、tools。
2. `anysearch.lua` 照抄骨架:handler 表键名对齐 `tools[].entry`;请求
   只经 `luban.http.request`;鉴权只经 `auth = {secret = "<逻辑 id>"}`。
3. 别在 Lua 里写 `Authorization`/`Cookie`/`Host`/`Content-Length`——
   禁写表,写了就拒;别试图把 Secret 写盘或回显——你根本拿不到原文。
4. 帽只许下调,`limits` 里写小不写大;0 不是无限,是非法。
5. 回归照 `tests/integration/plugins/test_anysearch_package.cpp` 的
   架子:假 DNS 注入 + 本机假服务,不烧真网。

## 6. 测试

- 假 AnySearch server 回归(CTest 内):`tests/integration/plugins/test_anysearch_package.cpp`。
  本机回环起假 HTTP 服务,假 DNS 把 `api.anysearch.com` 指到
  127.0.0.1(受控 HTTP 拒 IP 字面量 host,所以走 DNS seam),manifest
  原文不动,只把脚本里的 `API_BASE` 换成测试端口。覆盖:auth 头只在
  keyed 案出现、401/429/5xx/坏 JSON/超帽/取消、batch 串行次序、
  auto_registered Key 丢弃。
- 匿名真网:设计单 §13.5 要求一笔;网络不可用或不愿烧时明记 SKIP。
  Keyed 真测显式 opt-in,报告不得打印 Key。

## 7. 源码地图

```text
anysearch/
  package.yaml               包清单:schema 1,如实申报 code-bearing
  README.md                  本页
  docs/.env.example          Key 样板(占位)与数据目录说明
  skills/anysearch/SKILL.md  搜索策略(内容组件,不带 CLI)
  plugins/anysearch/
    plugin.json              manifest v2:四件工具、网络账、Secret、帽
    anysearch.lua            Lua 适配器:只用 Host API,串行 batch,Key 铁律
```

平台承诺:三平台(网络与 Lua 均无平台分叉)。设计全文见
`todos/Lua受控HTTP与Secret宿主能力设计.todo` §十二。
