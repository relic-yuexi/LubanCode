# search 内置 Ripgrep 后端迁移:P0-1 冻结旧合同与差分语料

对应设计单:`todos/SearchTool内置Ripgrep后端迁移设计.todo`(完整背景、
合同、批次划分见那份原始设计文档,本文档只承载 P0-1 这一批的具体产出,
不复述设计单已经写清楚的东西)。

本批**不接 ripgrep,不改 `src/tools/search.cpp` 的生产行为**,纯粹是给
后续 P0-2 起的真迁移打地基:把旧内核(`std::regex` ECMAScript +
`std::filesystem::recursive_directory_iterator`)的行为拍照存证,列清楚
换后端时哪些是绝不能变的产品合同、哪些是允许变但要走批准流程的实现细节。

> **P0-2/P0-3 交接注(后续批次落,本节先立账)**:P0-2(manifest/定位器/
> runner 合同/装配注入口)与 P0-3(argv 纯函数/边界策略)落地时,本文档
> 追加对应小节,保持"每批一账"。P0-1 的 golden 与 bench 是活参照物——
> 后续批次改了 `search.cpp` 任何行为,先跑 `search_golden_driver golden`
> 比对 24 条场景。

## 一、`tests/unit/tools/test_search.cpp` 用例分栏表

现有 17 条 `TEST_CASE` 按"换后端后必须逐字节/逐集合保持一致"(A 类,
本身就是回归红线)与"产品意图不变,但底层实现换了引擎,理论上应该还是
一样,需要 P0-5 用真 rg 跑同一夹具复核"(B 类)分栏。

**没有第三类**——现有 17 条测的全是 `SearchTool::execute()` 的黑盒行为
(输入 JSON → 输出文本/错误),没有一条直接测内部函数(`GlobToRegex`、
`WalkFiles`、`LooksBinary` 都没有独立单测),故没有"纯内部实现测试、换
后端后直接作废删除"这一类;P0-8 删旧内核时,这 17 条要么原样留(A 类),
要么在 P0-5 差分通过后把内部注释更新成"由 ripgrep 提供"(B 类),不需要
新增一类"删除表"。

### A 类:必须保留(SearchTool 自己生产的合同,不依赖搜索引擎细节)

| # | 用例 | 为什么是 A 类 |
|---|---|---|
| 1 | 命中带文件名、行号 | `文件:行号:行内容` 输出格式是 §5.1 明文保留的产品合同,字面量匹配在两套正则引擎下结果一致 |
| 3 | 命中超过上限,截断并注明 | "100 条=命中行数"的口径、截断提示文案都是 SearchTool 自己拼的(§5.1"SearchTool 管产品合同"),不随后端变化;新后端下触发方式从"逐行 break"变成"JSONL 流式收满主动收树"(§6.6),但断言的是产出文本,不是触发机制,测试本身不用改 |
| 4 | 跳过 `build/` 和 `.git/` 目录 | 宿主硬排除表(§5.3/P0-3)明文延续,只是实现从"目录名比对"换成"生成 `-g` 排除项",对外行为不变 |
| 6 | 中文内容命中 | 两套正则引擎对纯字面量子串匹配语义一致 |
| 8 | path 给单个文件,只搜这一个 | 单文件识别是 SearchTool 自己做的分发(§4.2/§6.3),不靠 rg 判断 |
| 9 | 单文件 path 照样吃 glob 过滤,配不上就不搜 | 同上,且"没搜到匹配的内容"文案是 SearchTool 自产 |
| 10 | glob:path 给单个文件,文件名配得上就返回它 | 同 8 |
| 16 | mode 不合法,报错不崩 | 参数校验在 `ParseSearchRequest` 层(§6.1),不涉及后端 |
| 17 | 缺少必填参数,报错不崩 | 同上 |

共 9 条。

### B 类:预期保留,但底层引擎换了,需 P0-5 真机复核

| # | 用例 | 换了什么引擎 | 为什么预期该一样、但不能打包票 |
|---|---|---|---|
| 2 | grep glob 过滤只搜指定扩展名 | glob 匹配:自研 `GlobToRegex`(转成 ECMAScript 正则)→ ripgrep globset(§5.2) | `*.cpp` 这种最基础的 basename 过滤两套实现理论上等价,但 globset 的具体实现细节(比如对 `.` 开头文件、大小写、转义字符的处理)没有逐条对照过 |
| 5 | 二进制文件被跳过 | 二进制探测:自读文件头 8KB 找 `\0` → ripgrep 自带的探测算法 | 两者都是"看头几 KB 有没有 `\0`"这个大方向,但精确的采样字节数、边界判定,todo §10.2 明写"按 rg 默认实测后写入合同,不凭猜测断言" |
| 7 | glob:按文件名通配找文件 | 同 2 | 同 2 |
| 11 | glob:`**/*.md` 根目录与子目录都中 | 自研双星号语义 → ripgrep globset 双星号语义 | §5.2 明文"Glob 语法从现有简版迁到 ripgrep globset 语法";两边对"`**/`在开头等于零层或多层目录"这条规则,本批未做逐机比对 |
| 12 | glob:`*.md` 按文件名匹配任意深度 | 同 11 | 同 11 |
| 13 | glob:`sub/*.md` 只中一层 | 同 11 | 同 11,"只中一层"是 glob 标准语义里比较没有争议的一条,但仍标 B 类而非 A 类,不能想当然 |
| 14 | glob:`docs/**` 任意深度 | 同 11 | 同 11 |
| 15 | grep 的 glob 过滤同样支持 `**/` | 同 11(应用在 grep 场景) | 同 11 |

共 8 条。

**P0-5 交付要求**:这 8 条对着真 rg 逐条重跑,结果与旧内核 golden
(`tests/fixtures/search/golden/old_kernel_golden.json` 里对应场景,尤其
`glob_*`/`grep_glob_filter_nested_cpp` 几条)比对;若全等,直接把测试挪去
backend-neutral 分类,不用改断言;若有出入,按 todo §5.2"迁移语义需在
中英文参数说明与 CHANGELOG 明写"处理,不能悄悄改断言让测试继续绿。

## 二、fixtures 清单与构造理由

完整清单、每条为什么这么造、`excluded_dirs/` 为什么不入 git、非法 UTF-8
文件名为什么 Windows 下不可测(含本机实测现象),写在
`tests/fixtures/search/README.md`,不在本文档里重复贴一遍。这里只列
**覆盖面对照表**,对照 todo §九 P0-1 清单原文要求的夹具类别:

| todo 要求 | 覆盖夹具 | 状态 |
|---|---|---|
| 中文 | `corpus/chinese/` | 完成 |
| CRLF | `corpus/crlf/` | 完成 |
| 空文件 | `corpus/empty/` | 完成 |
| 二进制 | `corpus/binary/` | 完成 |
| 超长行 | `corpus/long_line/`(约 20 万字符) | 完成 |
| 100+ 命中 | `corpus/many_hits/`(150 行) | 完成 |
| 隐藏文件 | `corpus/hidden/` | 完成 |
| ignored 文件(`.gitignore`/`.ignore`/`.rgignore`) | `corpus/ignore/` | 完成 |
| `.git`/`build`/`node_modules`/`.evidence` 各类目录 | `corpus/excluded_dirs/`(运行时现造,见 README) | 完成 |
| 嵌套 glob | `corpus/nested_glob/` | 完成 |
| 非法 UTF-8 正文 | `corpus/illegal_utf8_content/` | 完成(POSIX/Windows 均可) |
| 非法 UTF-8 文件名(POSIX) | `posix_only/make_illegal_utf8_filename.sh` | 脚本已交付,**Windows worktree 下如实标注不可测**(见 README"Windows 下为什么不可测"一节,含本机 MSYS 实测现象:非法字节被转成 MSYS 私有代理转义,不是原始字节,冒充不了 POSIX 语义) |

多造的两个不在 todo 清单原文里、但服务于后续批次的夹具:

- `corpus/regex_only_ecmascript/`:backreference、lookahead 命中锚点,给
  第四节的正则迁移表当实证,不是 todo 点名要求的类别,是为了不让迁移表
  变成纯嘴炮。
- `corpus/hidden/visible.txt`:hidden 夹具里额外放一个不隐藏的同名模式
  文件,用来在 golden 里对照"隐藏与非隐藏文件都命中",不是单独类别。

## 三、golden 产出方式与格式

驱动源码:`tests/manual/search_golden_driver.cpp`(`EXCLUDE_FROM_ALL`,
不进 ctest,手动跑,链 `lubancode_core`,不改 `search.cpp` 一个字节,只是
调用现成的 `SearchTool::execute()` 再做归一化)。

```powershell
cmake --build --preset debug --target search_golden_driver
build\debug\tests\Debug\search_golden_driver.exe golden tests\fixtures\search\golden\old_kernel_golden.json
```

产出:`tests/fixtures/search/golden/old_kernel_golden.json`,24 条场景
(17 条对应 test_search.cpp 现有用例覆盖的行为形态 + 7 条专门为 ignore/
excluded_dirs/regex 差异补的场景)。格式详细字段说明在
`tests/fixtures/search/README.md`"golden 复现方式"一节,核心设计取舍:

- **不钉遍历顺序**:`hits_sorted` 是排过序的数组,`hits_sha256` 是排序后
  拼接的摘要——旧内核的目录遍历顺序、未来 ripgrep 并行 walker 的顺序,
  两者从设计上就不是同一回事,拿原始行序当合同会把"遍历器实现细节"错误
  地提升成"产品合同",这正是 todo 原文强调的"不把 filesystem 遍历顺序
  钉成合同"。
- **不落绝对路径**:`path` 字段存相对 `corpus/` 的路径;观察边界读取提示
  (§7.2 那一行文案含绝对路径)只存 `notice_present`/`notice_over_threshold`
  两个布尔,不存文案原文——golden 要能在别的机器上复现比对,不能夹带
  本机路径。
- **JSON 序列化用 replace 容错**:`illegal_utf8_content` 场景的命中行
  本身就含非法 UTF-8 字节,nlohmann::json 默认 `dump()` 会因此抛异常
  (`invalid UTF-8 byte`);改用
  `dump(2, ' ', false, nlohmann::json::error_handler_t::replace)`,非法
  字节在**人读的** `hits_sorted` 里显示为替换字符,但 `hits_sha256` 是
  在替换之前、对原始字节算的,保真度不丢在哈希里,只在人眼展示层做了
  容错。

### 24 条场景实测结果一览(旧内核,本机跑出)

| id | 结果 | 验证的行为 |
|---|---|---|
| `grep_chinese` | 1 命中 | 中文字面量匹配 |
| `grep_crlf` | 1 命中,行内容不带 `\r` | CRLF 正确剥离 |
| `grep_empty_no_hit` | 0 命中 | 空文件不崩 |
| `grep_binary_skipped` | 0 命中 | 二进制探测生效,藏在里面的"命中"没被读到 |
| `grep_long_line_anchor` | 1 命中 | 20 万字符单行不崩、锚点仍能命中 |
| `grep_many_hits_truncated` | 100 命中,`truncated=true` | 上限与截断提示 |
| `grep_hidden_included` | 3 命中(隐藏文件、隐藏目录下的文件、非隐藏文件全中) | 旧内核不特殊处理"隐藏"这个概念 |
| `grep_ignore_files_not_respected` | 4 命中(3 个"该被 ignore 的"全中 + 1 个不该被 ignore 的) | **实证**:旧内核完全不认 `.gitignore`/`.ignore`/`.rgignore`,这正是 §5.3 要迁移掉的行为 |
| `grep_excluded_dirs_hard_skip` | 1 命中(只有 `real.txt`) | `.git`/`build`/`node_modules`/`.evidence` 硬排除生效 |
| `grep_explicit_evidence_file` | 1 命中 | 显式点名单文件绕开观察边界 |
| `grep_explicit_evidence_dir_root` | 1 命中 | 显式点名目录根绕开观察边界 |
| `grep_illegal_utf8_content` | 1 命中,不崩 | 非法 UTF-8 正文容错 |
| `grep_glob_filter_nested_cpp` | 3 命中 | glob 过滤 + 嵌套目录 |
| `grep_invalid_regex` | `is_error=true` | ECMAScript 语法错误有稳定报错,不崩 |
| `grep_no_match` | 0 命中 | 无命中路径 |
| `grep_ecmascript_backreference` | 1 命中 | **实证**:旧内核认 backreference,见第四节 |
| `grep_ecmascript_lookahead` | 1 命中 | **实证**:旧内核认 lookahead,见第四节 |
| `glob_star_cpp_recursive` | 3 命中 | `*.cpp` 任意深度 |
| `glob_doublestar_md` | 3 命中 | `**/*.md` 根+子目录 |
| `glob_docs_star_one_level` | 1 命中 | `docs/*.md` 只中一层 |
| `glob_docs_doublestar_any_depth` | 2 命中 | `docs/**` 任意深度 |
| `glob_hidden_dotfiles_included` | 3 命中 | glob 模式下隐藏文件同样不特殊处理 |
| `glob_ignore_files_not_respected` | 4 命中 | glob 模式下同样不认 ignore 文件 |
| `glob_excluded_dirs_hard_skip` | 1 命中 | glob 模式下硬排除同样生效 |

全部 24 条与人工核对的预期完全吻合,证明本批产出的固化逻辑没有归一化
层面的偷懒或误判(过程中曾发现并修掉两处驱动自身的 bug:"没搜到匹配的
内容"这句哨兵文案被误记成一条命中的问题、以及非法 UTF-8 字节导致 JSON
序列化抛异常的问题——都是驱动程序的问题,不是 `search.cpp` 的问题,
`search.cpp` 全程未改动一行)。

## 四、ECMAScript(`std::regex`)与 Rust regex(ripgrep)语法差异迁移表

**方法论声明**:本批 P0-1 规矩里明写"不联网"(不下载 ripgrep 二进制、
不接真实 rg),所以下表**没有**逐条上网核对 `docs.rs/regex` 最新页面,
而是基于 Rust `regex` crate 长期公开、稳定、架构性的设计事实(不是版本
细节)——该 crate 的核心卖点就是"worst-case 线性时间保证",这个保证从
设计上排除了 backreference 与 look-around,是这个库最广为人知、多年未变
的特性,不是需要联网才能确认的冷门细节。凡是涉及具体语法边界、错误
消息格式这类容易随版本变的细节,标"待 P0-5 真机差分核实",不凭本表
直接下最终结论——P0-5 有真 rg 可用时,应优先拿
`grep_ecmascript_backreference`/`grep_ecmascript_lookahead` 这两条 golden
场景重跑核实,而不是相信本表的猜测。

| 语法类别 | ECMAScript(`std::regex`,旧内核) | Rust regex(默认引擎,ripgrep) | 结论 |
|---|---|---|---|
| Backreference `\1` | 支持(golden `grep_ecmascript_backreference` 已实测:旧内核认) | **不支持**——crate 文档长期以来的公开说明是"故意不做,因为要保证最坏情况线性时间,backreference 匹配在通用情形下是 NP 困难问题" | 旧支持新不支持,需迁移说明。用到 backreference 的用户 pattern 换后端后要么报 `search_pattern_invalid`,要么需要用户自己改写成不含 backreference 的等价形式;§6.3 明确不开 `--pcre2`(PCRE2 才支持 backreference),所以这条不会被隐藏的兼容层悄悄接住 |
| Lookahead `(?=...)`/`(?!...)` | 支持(golden `grep_ecmascript_lookahead` 已实测:旧内核认) | **不支持**——同样是线性时间保证排除的语法(look-around 理论上可做,但会显著拖慢自动机构造与匹配,crate 设计上不做) | 旧支持新不支持,需迁移说明,同 backreference。todo §6.3 原文点名"look-around/backreference 需另开明确模式(即走 `--pcre2`,首版不开)" |
| Lookbehind `(?<=...)`/`(?<!...)` | **C++ 标准的 ECMAScript 语法本身不含 lookbehind**(这是 C++ 标准锁定的早期 ECMAScript 语法版本,晚于该版本才加进 JS 标准的 lookbehind 没有被 C++ 委员会跟进纳入)——待 P0-5 用具体编译器(MSVC/libstdc++)实测确认,不同实现有无扩展支持可能有出入 | 不支持,原因同上两条 | 两边大概率都不支持(旧内核这边不支持是 C++ 标准本身的限制,不是"故意迁移掉"的行为),**这条不算迁移表里的行为变化**,只是提醒:如果哪天旧内核在某个编译器上意外支持了 lookbehind(扩展行为),那反而是需要迁移说明的"旧支持新不支持",标"待 P0-5 真机差分核实"以防万一 |
| Unicode 属性转义 `\p{L}`、`\p{Han}` 等 | **C++ 标准 ECMAScript 语法不含** `\p{}` | 支持(`\p{Category}`、`\p{Script}` 等,crate 长期公开文档写明的核心能力) | 新支持旧不支持,纯增益。用户以后能写 `\p{Han}` 之类的 pattern 精确匹配汉字,旧内核完全做不到 |
| `\w`/`\d`/`\s` 的 Unicode 感知 | **不感知**——`std::regex` 在这份代码里吃的是 `std::string`(UTF-8 字节流),`\w` 只按 ASCII 字节判"是不是字母数字下划线",中文字符的 UTF-8 字节(`0x80` 以上)不会被 `\w` 当"单词字符" | **默认感知 Unicode**——crate 文档长期公开说明:除非显式用字节模式(`(?-u)` 或 `regex::bytes::Regex`),`\w`/`\d`/`\s` 默认按 Unicode 属性判定,中文、日文等文字里的表意文字会被 `\w` 当作单词字符 | **这条是本仓场景下最值得警惕的一条**——本仓大量中文注释/文档,pattern 里用 `\w+` 这类写法去匹配"一个词"时,旧内核下中文字符不算 `\w`(常导致匹配在中文处断开),新后端下中文会被当 `\w` 的一部分(整段连续中文可能被当成一个"单词"匹配)。较高置信度(crate 设计公开且稳定),**但精确到"哪些具体 Unicode 类目被计入、组合字符/变体选择符怎么处理"这类边界,标待 P0-5 真机核实**,不在本表断言到那么细 |
| POSIX 字符类 `[[:alpha:]]` 等 | 支持(C++ 标准 ECMAScript 语法保留了 POSIX 类) | 支持(crate 文档写明支持 POSIX 类) | 两边都支持,不是迁移点 |
| 非贪婪量词 `*?`、`+?`、`??` | 支持 | 支持 | 两边都支持,不是迁移点 |
| 具名捕获组 | 支持(`(?<name>...)`) | 支持(`(?P<name>...)`,也接受 `(?<name>...)` 写法——**具体哪种写法两边都能通用,待 P0-5 真机核实**,不确定这条就不断言) | 大方向一致,写法细节留待核实 |
| 非法模式的报错行为 | 抛 `std::regex_error`,`search.cpp` 现有代码接住转成 `is_error=true`(golden `grep_invalid_regex` 已实测) | 编译失败时 CLI 退出码 2,ripgrep 自己往 stderr 写错误信息;todo §6.6/§7.1 已规划成 `search_pattern_invalid` | 两边都有"编译不过就报错不崩"这个大方向,**具体报错文案、哪些边界情况判"合法"哪些判"非法"两边可能不完全对齐**(比如某个 pattern 在 ECMAScript 语法下非法、换到 Rust regex 语法下反而合法,或反过来),标待 P0-5 真机差分核实,不能只测 `grep_invalid_regex` 这一条(`(unclosed`,两边八成都判非法,不是好的差异探针)就下结论——这也是为什么本批额外造了 backreference/lookahead 两条更有区分度的探针 |
| 转义序列(`\n`、`\t`、`\xHH`、`\uHHHH` 等) | 支持标准 C 风格转义 | crate 文档写明支持类似的转义集合 | 大方向一致,**逐个转义序列是否每个都对齐,待 P0-5 真机差分核实**,本表不逐项断言 |

## 五、`fixed_strings` 参数 schema 决定

todo §5.1 给出的建议形状:

```json
{ "fixed_strings": false }
```

- `false`(默认):`pattern` 按 ripgrep 默认 Rust regex 语法解释。
- `true`:传 `--fixed-strings`,`pattern` 按字面量逐字匹配,不解析正则
  元字符。

**核对结论:不需要微调,原建议形状直接采纳。** 理由:

1. 只加一个可选布尔字段,不新增 flags 数组,符合 §5.1"不新增任意 flags
   数组"的合同底线——多一个字段就多一分模型选错的可能,布尔已经是能
   表达"要不要按字面搜"这个二元问题的最小单位。
2. 默认值 `false` 保持"pattern 默认当正则解析"这条现有习惯不变
   (现有 `test_search.cpp` 全部 17 条用例都不传这个参数,默认值不改变
   任何现有调用形状,P0-5 换后端时不用碰这些测试的入参)。
3. 命名 `fixed_strings` 直接对应 ripgrep CLI 的 `--fixed-strings` 参数名,
   模型/开发者若熟悉 ripgrep,不用二次学习映射关系;不熟悉的也能从参数
   名直译"固定字符串"望文生义。
4. 中英文都要过一遍的核对:
   - 中文候选文案:`"pattern 是否按字面量搜索(不解析正则元字符);默认
     false,pattern 按 ripgrep 的 Rust regex 语法解析"`。
   - 英文候选文案:`"Whether pattern is matched literally (no regex
     metacharacters); defaults to false, meaning pattern is parsed as
     ripgrep's Rust regex syntax."`
   - 两版文案都要在 P0-5 落地时同步写进
     `src/prompts/tools/zh-CN/search.md`、`src/prompts/tools/en/search.md`、
     `SearchTool::input_schema()` 里 `pattern`/`fixed_strings` 两个字段的
     `description`,以及 `tests/unit/tools/test_tool_text.cpp` 的断言——
     本批只定文案内容与落点,不落地(§6/P0-5 范围)。
5. 唯一需要 P0-5 留意的边界:`fixed_strings=true` 时 `pattern` 里若带
   `-` 开头字符,argv builder 仍要走参数边界保护(§4.2.4),不能因为是
   "字面量模式"就放松对 pattern 首字符的处理——这条不影响 schema 形状
   本身,记在这里防止 P0-5 实现时漏掉。

## 六、基准:旧内核对本仓 `src/` 的实测数据

驱动:同一个 `search_golden_driver`,`bench` 子命令。

```powershell
build\debug\tests\Debug\search_golden_driver.exe bench tests\fixtures\search\bench\old_kernel_bench_src.json src
```

原始数据(全部样本、P50/P95/min/max、首轮单独字段)在
`tests/fixtures/search/bench/old_kernel_bench_src.json`。语料是本仓
`src/` 目录,`CountFiles` 统计到 **768 个非二进制候选文件**(遍历口径与
`WalkFiles` 一致:跳过 `.git`/`build`/`node_modules`/`.evidence`)。5 类
查询、每类 7 轮,某一次实测结果(数值会因机器负载浮动,重点看相对关系
与结构性发现,不是某个绝对毫秒数):

| 查询 | pattern/glob | 命中(最后一轮输出行数) | P50 | P95 | min | max |
|---|---|---:|---:|---:|---:|---:|
| `literal_common_word` | `SearchTool` | 32 | 5167.6ms | 5560.2ms | 1768.0ms | 5788.9ms |
| `regex_moderate` | `std::[A-Za-z_]+<` | 101(含截断提示行) | 83.5ms | 87.2ms | 79.3ms | 88.1ms |
| `no_match` | `definitely_absent_zzz_token_12345` | 0 | 6327.1ms | 6385.8ms | 6211.5ms | 6596.6ms |
| `high_frequency` | `the` | 101(含截断提示行) | 902.4ms | 925.8ms | 883.4ms | 954.7ms |
| `glob_enum_cpp` | `*.cpp`(glob 模式) | 101(含截断提示行) | 101.1ms | 105.1ms | 86.4ms | 107.3ms |

**结构性发现(不是"慢"这一句空话,是量出来的具体原因)**:命中数落在
"101 行"(即触发了 100 条截断)的三个查询(`regex_moderate`、
`high_frequency`、`glob_enum_cpp`)都在 80~950ms 量级完成;而命中数没到
截断线的两个查询(`literal_common_word` 只有 32 命中、`no_match` 0 命中)
都要 1.7~6.6 **秒**——比截断了的查询慢一到两个数量级。

原因很直白:旧内核的截断判断在 `WalkFiles` 的 `visit` 回调里,一旦
`hit_count >= kMaxResults` 就让 `visit` 返回 `false`,`recursive_directory_
iterator` 立刻停止遍历,不再碰后面的文件;但只要命中数没到 100(哪怕是
0 命中),就必须把 768 个候选文件**逐个**打开、逐行读、逐行跑一遍
`std::regex_search`,慢的不是正则本身(`regex_moderate` 那条正则比字面量
`SearchTool` 复杂得多,却因为命中密集、很快凑够 100 条提前收手,反而是
五条里最快的之一),慢的是"要不要走完整棵树"这件事本身。

这条实测结果直接印证 todo §2.1"没有 ripgrep 的 ignore walker……没有
成熟的 literal extraction、自动机、SIMD 与并行文件扫描"里点的病根,也
印证了 todo §八 P0-7 为什么专门把"无命中"和"100+ 高频命中"分成两类
查询——这两类在旧内核下的成本模型截然不同,不是同一个曲线的两个点。
把 `wall_ms_first_round`(首轮,读数普遍与后续轮次接近,可见"冷盘/热盘"
在这台机器上这次跑不是主导因素,主导因素是命中密度)和
`wall_ms_p50_excluding_first_round` 都留在原始数据里,供 P0-7 需要更严谨
的冷热盘分离时参考,但**这不是 P0-7 要求的正式冷热盘门槛**,本批只是
顺手留了字段,不代表 P0-7 的基准方法论已经跑过了。

**本批未采集的维度**:内存占用。todo 原文"若方便测的话"给了空间——
实测中 Windows `psapi.h`(`K32GetProcessMemoryInfo`)在本仓头文件/工具链
环境下编译报错(`PROCESS_MEMORY_COUNTERS` 结构体解析出一堆"未知重写
说明符"这类无关报错,像是宏或 include 顺序污染),排查成本超出 P0-1
这个诊断批次该花的精力,如实留白,交给 P0-7——那边本就是正式的性能与
稳定门批次,峰值内存是其列明的必测维度之一,到时候可以换
`GetProcessMemoryInfo` + 显式链 `psapi.lib` 的路子重新试。

## 七、CHANGELOG 落点(本批不改,先标清楚)

不在本批实际编辑 `CHANGELOG.md`(遵照任务要求),按行为归属预先标好
未来哪个批次落哪条:

| 变化 | 落点批次 | CHANGELOG 措辞方向(草稿,不是最终文案) |
|---|---|---|
| `search` grep 正则从 ECMAScript 换成 ripgrep/Rust regex | P0-5(切主路) | "search 的 grep 模式正则语法从 ECMAScript 换成 Rust regex(ripgrep 同款):不再支持 backreference、lookahead;新增 Unicode 属性转义 `\p{...}`,`\w`/`\d`/`\s` 默认按 Unicode 而非 ASCII 判定" |
| glob 语法从自研简版换成 ripgrep globset | P0-5 | "search 的 glob 匹配语法换成 ripgrep 同款(globset);常见写法(`*.ext`、`**/*.ext`、`dir/**`)行为不变,边界写法如有出入以 P0-5 差分表为准" |
| ignored 文件默认不再被搜到 | P0-5 | "search 默认遵守 `.gitignore`/`.ignore`/`.rgignore`;要搜到被忽略的文件,把 path 逐字点名到具体文件或目录" |
| 新增 `fixed_strings` 参数 | P0-5 | "search 的 grep 模式新增可选参数 `fixed_strings`:传 true 按字面量搜索,不解析正则元字符" |
| 随包携带 ripgrep 可执行文件、发行包体积变化 | P0-6 | "LubanCode 发行包内置 ripgrep(MIT 协议)……" |
| 旧内核(`std::regex`/自写 glob/自写目录遍历)删除 | P0-8 | "search 的自研搜索内核已删除,统一由内置 ripgrep 驱动" |

## 八、给 P0-2 的交接备注

1. **golden 与 bench 数据是活的参照物,不是一次性快照。** P0-2 起若改了
   `search.cpp` 的任何行为(哪怕只是重构、不打算改语义),先跑一遍
   `search_golden_driver golden`,跟 `tests/fixtures/search/golden/
   old_kernel_golden.json` 比对,confirm 24 条场景依旧全等,再继续往下
   走——这是本批留给后面批次的免费回归网。
2. **`excluded_dirs/` 每次都要现造,不能假设它在盘上。** 见第二节与
   README 的专门说明;P0-5 的差分工具如果要同时跑旧内核和新 rg,两边
   都要在各自的执行前调用同一份 `MaterializeExcludedDirsFixture` 逻辑
   (或直接复用 `build_corpus.sh`),不能只造一次共用。
3. **B 类 8 条用例是 P0-5 的第一验收项,不是可选项。** 换上真 rg 后,
   第一件事就是把这 8 条重新跑一遍(可以直接扩展
   `search_golden_driver`,加一个 `--backend=ripgrep` 之类的开关,复用
   同一份场景表),全等就把测试从"B 类"改注成"A 类"(或者干脆去掉分类
   注释,反正行为验证过了);有出入就必须按 todo §5.2 的"迁移语义写进
   中英文参数说明与 CHANGELOG"处理,不能因为"看起来差不多"就悄悄改断言
   让测试继续绿——那样会把一次语义变化伪装成"没有变化"。
4. **正则迁移表的"较高置信度"部分需要真机核实,不能直接当最终结论引用。**
   尤其 `\w`/`\d`/`\s` 的 Unicode 感知那一条,本仓中文内容占比不低,这条
   一旦证实,值得单独在 P0-5 的 CHANGELOG 条目里加粗提醒用户。
5. **`fixed_strings` 的 schema 已经定案,P0-5 直接抄第五节的字段名和中英
   文文案草稿去用**,不需要重新讨论形状;真要改也只应该是文案措辞上的
   润色,不应该改字段名或语义。
6. **内存基准这块欠账,P0-7 接手时排查 `psapi.h` 编译问题**(或换路子),
   不要假设这是"已经测过、只是数字不好看"——是压根没采集到。

## 九、P0-2/P0-3 交接账(manifest、定位器、runner 合同、argv 与边界策略)

P0-2/P0-3 落地(同一批),生产行为仍一字不动:旧内核照跑,落的清单如下。

### 落了什么

| 件 | 位置 | 说明 |
|---|---|---|
| manifest(三平台真哈希) | `third_party/ripgrep/manifest.json` | Windows/macOS 与上游 `.sha256` 双对;Linux deb 原占位格已按上游 `.sha256` + 本机下载重算填入 |
| MIT license 原文 | `third_party/ripgrep/LICENSE-MIT` | 上游 15.2.0 tag 逐字节(1081 字节) |
| 第三方声明 | `THIRD_PARTY_NOTICES.md`(仓根) | 组件/版本/上游/许可/打包位置五项 + Linux 取 .deb 的取舍 + 构建期依赖不随包的分界 |
| 打包脚本 | `scripts/fetch_ripgrep.py` | 只读 manifest、固定 URL、重算哈希、只解指定 member(ar/tar/zip/deb 四路,deb 的 `./` 前缀剥掉对账)、staging 过门原子换入、跑 `--version` 精确校版本;Windows 资产真机全链路实测过,deb/tar.gz 解成员逻辑本地实测(ELF/Mach-O 魔数对上) |
| 合同头/源 | `src/tools/search_ripgrep.hpp/.cpp` | typed request/policy/result、十枚稳定错误码(`search_*`)、`BundledRipgrepLocator`(只认 `ExecutableDir()/libexec`)、`IRipgrepRunner`/`BundledRipgrepRunner`、`BuildGrepArgv`/`BuildGlobArgv` 纯函数、`BuildSearchPolicy`/`BuildObservationExcludes` |
| 观察边界快照 | `src/tools/observation_filter.hpp/.cpp` | `ExcludedDirsSnapshot()`:线程安全拷贝,只含运行时登记账(`.evidence` 名字口径仍走硬排除表) |
| 注入口 | `src/tools/search.hpp/.cpp` + `src/app/tool_runtime.cpp` | `SearchTool(shared_ptr<IRipgrepRunner>)`;基础表/Explore 表两处装配注默认 runner。**execute 不消费**——P0-5 切主路才接线,故意不写"看后端在不在再选路"的分支(那会开出 rg 缺件静默退回慢内核的禁路) |
| doctor | `src/app/commands/doctor_commands.cpp` | `/doctor search`:真起 `rg --version` 精确校版本(版本记号精确相等;首行实测 `ripgrep 15.2.0 (rev …)`,rev 不钉),不列 PATH 候选 |
| 单测 | `tests/unit/tools/test_search_ripgrep.cpp` | 24 条 TEST_CASE,全注入式(假路径+假探针),不起真进程 |

### 版本精确校验的实测形状

上游 15.2.0 的 `rg --version` 首行是 `ripgrep 15.2.0 (rev e89fff89ac)`——
**不是**裸 `ripgrep 15.2.0`(设计单 §8.2 的措辞按"版本记号精确"执行:
首行程序名记号 == `ripgrep` 且版本记号 == `15.2.0`,rev 段随构建变,不钉)。
`ParseRipgrepVersion` 按此解析,单测含 rev/CRLF/垃圾输出/认不出各路。

### PATH 防劫持的三面证据

1. **定位面**:`BundledRipgrepPath()` 只由 `platform::ExecutablePath()` 拼
   `libexec/rg(.exe)`,源码没有读 PATH/环境变量的分支;PATH 前置假 rg 目录
   后,定位结果不含该目录(单测)。
2. **执行面**:smoke/将来 P0-4 的执行都拿绝对路径给 `RunProcess` 的
   argv[0]——Windows `CreateProcessW`(应用名空,但命令行首 token 含目录
   路径)不搜 PATH,POSIX `execvp` 对含 `/` 的名字不搜 PATH。POSIX 单测造
   一枚会写 marker 的假 rg 脚本放 PATH 最前,smoke 后 marker 不存在。
3. **环境面**:`LUBANCODE_RG_PATH` 之类的口子设计单明令禁止,定位器不读;
   单测设了该变量,定位结果与不设一字不差。

另有**真机四幕**(WSL 下临时驱动链 `search_ripgrep.cpp + process_posix.cpp`
直跑 `RunRipgrepSmoke`,非单测假件):deb 内真 GNU rg → `ready`/版本 15.2.0
精确对上;假版本脚本 → `version_mismatch` + 稳定码;缺件 → `missing` +
稳定码;PATH 前放会写 marker 的假 rg、smoke 真 rg → 照常 `ready` 且
marker 不存在。真探针(`DefaultRipgrepVersionProbe` → `RunProcess` →
`ParseRipgrepVersion` → 精确比对)全链路真机验过。

### 排除账与旧内核的对账表(P0-3)

| 旧内核(search.cpp) | 新策略(P0-3) | 对齐点 |
|---|---|---|
| `SkipDirNames` 无条件跳 `.git/build/node_modules/.evidence`(任意深度) | 硬排除 `**/<名>/**` 四条,无条件 | 同一张表、同语义(ripgrep walker 对整棵被排除目录剪枝) |
| `root_in_boundary=true` 不做边界过滤 | root 在观察边界内不生观察排除(登记账与 `.evidence` 名字口径都算) | 同一分支 |
| 点名 `build/` 等目录照常搜 | 排除 glob 按 root-relative 语义天然不咬根下文件 | 同一行为(测试注释钉死推理) |
| 默认递归时边界内文件/目录不进结果 | `BuildObservationExcludes` 只收落在 root 下的登记目录,逐条 `!<rel>/**` 让 walker 真剪枝 | 从"解析后丢命中"升级成"walker 剪枝"(设计单 §5.4 的要求) |

### argv 基线(逐元素,单测钉死)

```text
grep: --no-config --json --line-buffered --color=never --hidden
      --engine=default --no-multiline [--fixed-strings]
      [-g <用户 glob,首 ! 转义 [!]>] [-g !<排除>]...
      -- <pattern> <scope>
glob: --no-config --files --null --hidden
      [-g <用户正向 glob,首 ! 转义 [!]>] [-g !<排除>]...
      -- <scope>
```

scope:目录 root 恒 `.`;单文件 root 用文件名,cwd=父目录。flag 墙:
`--pre/--pre-glob/--search-zip/--pcre2/--auto-hybrid-regex/--type-add/
-u/--unrestricted/--engine(裸)` 无任何输入面(源码无生成分支,测试扫
argv);`--follow/--text/--no-ignore` 有 policy 开关、生产 policy 恒默认。

### 本批没做的(归后续批次)

- **rg 二进制未随包**:`libexec/` 里没有真 rg,doctor 如实报 missing。脚本
  与哈希已备(真机 fetch Windows 资产全链路通过),入包、安装、Release 门
  是 P0-6。
- **启动 smoke 不做**:P0-4 前不动启动行为,smoke 由 `/doctor search` 明
  触发。
- **`BundledRipgrepRunner` 的流式执行**:前置校验(缺件/不可执行/版本/
  spawn)真做,之后如实回 `search_backend_not_wired`——P0-4 接 ChildProcess
  流式分帧时换掉这个尾巴,P0-5 切主路。
- **README"一份原生二进制"口径不改**:rg 未真入包前改口径反而失实,归
  P0-6(rg 真随包时一并改)。
