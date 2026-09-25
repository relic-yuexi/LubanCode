# TUI 排版约定:frame / 列对齐 / 键提示

> TUI 界面美化单(`todos/tui优化todo.todo`)批 0 落地的渲染基件约定。
> 批 1-8 的所有 slash 命令渲染段、常驻面板、CLI 子命令照此办理——排版是
> UX 的横切关注点,不是某条命令的事。

## 总规矩

1. **render 段只调 `cli::frame::*`、`cli::format::*`、`divider::line`**,
   不再手拼多行字符串塞 `TermOut`。长报告类(insights/usage/prompt-audit/
   evolve)保留"先收集 `lines: vector<string>` 再 flush"的模式,只在每条
   `push_back` 之前改用助手。
2. **不直接 `std::cout`**;输出端口不切换(`TermOut`/`TermErr` 照旧)。
3. **颜色与边框不写死 ANSI**:全部从 `cli::Theme` 字段取(见下表)。错误
   与警告仍走 `theme.error`/`theme.stats`,不另立色。
4. **文案全由调用方递**:三助手零内嵌文案(空态提示、标题、表头都是
   调用方的 `tr()/trf()` 文案),i18n 键不新增。
5. **机器消费分支不动**:`--json`/`--format` 分支字节级不变,新排版只走
   "人看"分支。

## 三套助手(`src/cli/terminal_frame.hpp`,命名空间 `cli::frame`)

| 助手 | 输入 | 行形 | 谁用 |
| --- | --- | --- | --- |
| `RenderList` | `Row{label, value, hint?, bullet?}` | `<•> label  value  hint` | 条目清单(memory 列表、菜单、搜索结果) |
| `RenderTable` | 列定义 + 二维内容 + 单元格 tone | 列宽自适应的表格 | 多列记录(list/jobs/状态表) |
| `RenderKeyValues` | `Field{key, value, accent?}` | `key  value` 两列对齐 | 状态摘要(status/doctor/会话信息) |

三件都是纯函数:吃 `Theme` + 结构化数据,吐 `vector<string>`(行内无换行
符),空输入给空输出(空态文案调用方自己递)。落盘由调用方逐行走
`TermOut`。

### width 口径

`width` 是**整行显示宽预算**(含边框与衬空共 4 列开销),一般传探测到的
终端列数;`0` = 按内容自适应不设帽。超预算时列内截断保字头(列表先丢
hint 再截 value;表格从最宽列起逐列削)。宽度全按 UTF-8 显示列算(中文/
emoji 占双列),截断不劈半个宽字。

## BoxStyle 三档

```cpp
frame::BoxStyle // 默认 Light;Ascii()/Light()/Double() 三工厂
```

| 档 | 字形 | 何时用 |
| --- | --- | --- |
| `Light` | `┌─┐│└┘`(U+2500 族) | 默认,一切人看输出 |
| `Double` | `╔═╗║╚╝`(U+2550 族) | 重框(顶层报告封面、向导) |
| `Ascii` | `+-|`(纯 7 位) | 调用点明确要降级时(编码受限的管道/日志) |

标题嵌进上边框(`┌─ Title ───┐`),标题色 `frame_title`、边框色
`frame_border`。

## 降级合同(单子合同第 3 条)

plain 主题(`theme.reset` 空串——全仓既有探针,覆盖 `--no-color`、管道
模式、T3 终端能力禁用)下,**不管传哪档 BoxStyle**:

- 隐藏装饰:不画框、不出边框字形;
- 零转义字节:输出里一个 `\x1b` 都没有;
- 项目符退 `-`,表格表头下垫一条 `-` 横线顶替颜色分隔;
- 对齐保住:列间补空不变,可读性不降。

单测钉死:`tests/unit/cli/test_frame_helpers.cpp` 的 plain 两册。

## 列对齐约定

- 列间**两格**空;首列左对齐、走 `row_label`(主题里配的是加粗档);
  数值列 `align_right` 右对齐。
- 对齐/截断只准走 `cli::format::AlignLeft/AlignRight`
  (`src/cli/format_utils.hpp`),不许各写一份 pad/truncate。
- 分组横线一律 `divider::line(style, width)`
  (`src/cli/divider.hpp`):`Ascii "-"` / `Light "─"` / `Heavy "━"`;
  "── 组名 ──"这类手拼横线批 5-6 收口时全部替换。

## 主题字段语义(批 0 新增 11 枚,`src/cli/theme.hpp`)

| 字段 | 用在哪 |
| --- | --- |
| `frame_title` | frame 标题文字(三助手共用) |
| `frame_border` | frame 边框字符 |
| `row_label` | 列表 label 列 / 键值对 key 列 / 表格首列 |
| `row_value` | 列表 value 列 / 键值对 value 列默认档;空 = 默认前景 |
| `row_muted` | 淡色附注(键值对 Muted accent、空态文案) |
| `table_header` | 表头行 |
| `table_pass` | 表格 pass 单元格 |
| `table_skip` | 表格 skip 单元格(fail 不另立色,走 `error`) |
| `list_bullet_user` | 列表项目符·user 档(如 user 层记忆) |
| `list_bullet_project` | 列表项目符·project 档 |
| `key_hint` | 列表行尾键提示/短注(如 "(global)"、快捷键注) |
| `row_selected_bg` | 常驻选择面板(SessionPicker 等)光标行整行底色;plain 空串不铺底 |

内置三套板(dark/light/plain)各带缺省值;plain 全空串。主题加载侧只认
名字,老名字的配置文件直接兼容——不存在盘上主题文件,新增字段的缺省值
在 `theme.cpp` 的三套板里。

## 批次索引

- 批 0:基件 API + 主题字段 + 约定文档,`tests/unit/cli/test_frame_helpers.cpp`
  钉形状与降级合同。
- 批 1-8:按单子"分期落地"逐批套用,批间不越次序——基件不到位时上层
  改完会回到拼字符串。

## 批 1 落地时补的三条裁量(后续批次照办)

`/memory` 全套(批 1,`src/app/commands/memory_commands.cpp`)落地时,
"i18n 键不新增、cpp 不硬编码新文案"的合同与"表格要有表头/命令输出要
有标题框"之间有三处空隙,裁量如下:

1. **表头与档位标识用数据 schema 名**。表格列头(job/state/op/layer/
   wait/worker、id/score/hard/terms/bytes/result)与档位标注(weak、
   [warn])一律用数据字段的英文名——它们是 schema 标识符,不是待译
   文案,对中英文用户一视同仁。真正需要翻译的句子(表标题、提示、
   原因短句)仍走既有 `tr()/trf()` 键。
2. **一句既有文案拆键值对("SentenceField" 模式)**。命令输出里大量
   既有文案自带 "标签: 值" 句式(中英两套冒号都是半角),渲染段按第
   一个冒号拆成 `Field{key, value}` 进键值对助手——拆的是既有文案,
   不添不改一个字;没有冒号的整句进 value。一键塞多对的
   ("已入库条目: {0}；待写任务: {1}"),首对进 key、余下整段进 value
   (分号全半角中英不一,拆了脆)。
3. **长正文不塞框**。`/memory show` 一类的 markdown 正文在头部键值对
   框之外原样跟出——塞框会被列帽截断劈行,保终端自然折行。

批 1 还有一处渲染改道:`jobs.title_line/error_line/log_line`、
`show.header` 四枚既有键的**信息**(标题、失败原因、日志路径、所在目录)
并进了新排版,键本身保留在 i18n 表里不删——信息一个不丢,只是不再逐句
成行。

## 批 2 落地时补的裁量(高频资源类,后续批次照办)

批 2(`/hooks`、`lubancode hook validate/test/init`、`/doctor cache
probe/usage`、`/plugin` 一族,2026-09-23)落地时的五条裁量:

1. **硬编码中文的老文件按"不添不改一个字"办**。`hook_commands.cpp` 的
   文案从来是硬编码中文(不走 i18n 表),"i18n 键不新增"合同在此读作:
   既有句子原样进 frame,一字不添不改;SentenceField 拆的还是句内冒号。
   后续批次遇同款老文件照此办理,不趁机补 i18n 化(那是另一笔账)。
2. **流式进度行不进框**。探针类命令(`/doctor cache probe` 的逐轮轮号
   行、`/doctor cache usage` 的"发探针"行)每轮一发 HTTP,进度必须即时
   可见——流式行保持 TermOut 原样,轮**结果**收集进收尾的汇总表。
   框只装结论,不装等待。
3. **引导下文的尾冒号剥掉**。旧平铺排版的 "静态检查:"/"已装载 N 条
   hook 定义(...):" 这类句子自带尾冒号(引导下一行),进 frame 标题
   就是废话——标题化时剥掉;SentenceField 拆列吃冒号是同一条裁量的
   逐句版。
4. **CLI 子命令没有会话主题,按 ManageSession 先例现起**:
   `ResolveTheme(std::string(), DetectConsoleCapability().colors_enabled)`
   ——管道/重定向自然降 plain,测试进程里钉的就是 plain 形状。--json
   机器面只收口输出端口(std::cout -> TermOut),落盘字节级不变。
5. **命令族名勘误**:单子"现状证据"表的 `/workspace`(行号 274/368/
   497/592/618/622)实为 `workspace_commands.cpp` 里的 `/plugin` 一族
   (inspect/doctor/test/trust-untrust/reload/enable-disable)——全仓没有
   /workspace 命令,行号精确对应 /plugin 分支。批 2 按实际命令族办;
   `src/workspace/`(manifest_lock 等)是另一单的领地,一字未碰。
