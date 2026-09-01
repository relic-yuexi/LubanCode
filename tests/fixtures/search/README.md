# search 迁移单夹具:tests/fixtures/search/

给《SearchTool 内置 Ripgrep 后端迁移设计》(`todos/SearchTool内置Ripgrep后端迁移设计.todo`)
用的照相语料。P0-1 造这批夹具是为了给旧内核(`std::regex` +
`recursive_directory_iterator`,P0-5 已整段删除)拍照做差分;差分过门后
(P0-8),corpus/ 留着继续服务 `search_golden_driver` 的 golden/bench——
golden 给当前后端(随包 ripgrep)拍快照,bench 是 P0-7 性能门的驱动件。

设计与结果的完整叙述(用例分栏表、ECMAScript/Rust regex 迁移表、
fixed_strings schema 决定、基准结论)在
`docs/development/search-ripgrep-migration.md`,这里只讲"这批文件是什么、
怎么复现"。

## 目录结构

```text
tests/fixtures/search/
  README.md                       本文件
  build_corpus.sh                 一键(重)生成 corpus/ 的脚本,幂等
  corpus/                         静态夹具语料(除 excluded_dirs/ 外均入 git)
    chinese/chinese.txt
    crlf/crlf.txt
    empty/empty.txt
    binary/blob.bin
    long_line/long_line.txt
    many_hits/many_hits.txt
    hidden/.hidden_file.txt, .hidden_dir/inside.txt, visible.txt
    ignore/.gitignore, .ignore, .rgignore, ignored_by_*.txt, kept.txt
    excluded_dirs/                不入 git,见下方专门说明
    nested_glob/src/**、docs/**、top.md
    regex_only_ecmascript/regex_only_ecmascript.txt
    illegal_utf8_content/bad_utf8.txt
  posix_only/
    make_illegal_utf8_filename.sh 非法 UTF-8 文件名,仅 POSIX 现造,不进 git
  bench/
    old_kernel_bench_src.json     旧内核对本仓 src/ 的基准原始数据(P0-1 采集,
                                   冻结的历史基线——旧内核已删,没法重跑,这是
                                   P0-7 性能表"旧内核"一列的唯一数据源,故留)
```

`golden/old_kernel_golden.json`(旧内核 24 场景冻结基线)已随迁移单 P0-8
删除:它的唯一消费者是 P0-5 的 diff 差分门,门已过(全等 17/批准迁移 7/
未批准 0,账在迁移文档 §十),旧内核源码也没了,基线留着只会引诱人拿
新后端跟一段死语义对表。

## 各夹具为什么这么造

| 夹具 | 覆盖的 todo 要求 | 构造要点 |
|---|---|---|
| `chinese/` | 中文 | 三行文本,命中行含中文关键词,验证 UTF-8 子串匹配与输出不乱码 |
| `crlf/` | CRLF | `\r\n` 行结尾,验证旧内核 `getline` 后手动去 `\r`,命中行不带残留 `\r` |
| `empty/` | 空文件 | 零字节文件,验证不崩、报"没搜到" |
| `binary/` | 二进制 | 含 `\0` 字节,且字节流里刻意藏一段"看着像命中"的字面量,验证二进制探测优先于正则生效,不会被当命中读进去 |
| `long_line/` | 超长行 | 单行约 20 万字符,命中锚点在中间,验证不因单行过长崩溃或截断丢锚点(旧内核目前无单行长度上限,新后端 §6.5 有 16 KiB/行的合同,这条正是留给 P0-4 用的基线) |
| `many_hits/` | 100+ 命中 | 150 行同一字面量,验证 100 条截断与"截断"提示 |
| `hidden/` | 隐藏文件 | 点号开头的文件与目录,验证旧内核默认不特殊处理隐藏文件/目录(只按 4 个具名目录跳过,不认"隐藏"这个概念) |
| `ignore/` | ignored 文件(`.gitignore`/`.ignore`/`.rgignore`) | 三个 ignore 文件各自点名一个"该被忽略"的文件,`kept.txt` 三家都不管。旧内核压根不读这三种 ignore 文件,golden 里能看到全部 4 个文件都被命中——这正是 todo §5.3 明写的"有意迁移"要变的行为,不是本批的 bug |
| `excluded_dirs/` | `.git`/`build`/`node_modules`/`.evidence` | 见下方专门说明,不静态入 git |
| `nested_glob/` | 嵌套 glob | `src/`、`src/sub/`、`src/sub/deeper/`、`docs/`、`docs/sub/`、根目录 `top.md`,覆盖 `*.ext`(任意深度按文件名)、`**/*.ext`(任意深度)、`dir/*.ext`(仅一层)、`dir/**`(该目录下任意深度)四种既有语义 |
| `regex_only_ecmascript/` | ECMAScript/Rust regex 迁移证据 | 含可被 backreference(`(\w+)_\1_repeat`)与 lookahead(`look(?=ahead_marker)`)命中的行——旧内核(`std::regex` ECMAScript)认得这两种语法,Rust regex 默认引擎设计上不支持,留给迁移表当实证,不是"必须保留"的合同用例 |
| `illegal_utf8_content/` | 非法 UTF-8 正文(POSIX 要求里"文件名/正文"的正文那一半) | 文件名合法,内容里混了孤立延续字节、`0xFF`、过长编码等非法 UTF-8 序列,外加一段合法 ASCII 命中锚点,验证 grep 不会因为正文非法 UTF-8 而崩或吞异常 |
| `posix_only/make_illegal_utf8_filename.sh` | 非法 UTF-8 文件名(POSIX 要求里的"文件名"那一半) | 见下方"Windows 下为什么不可测"专门说明 |

## `excluded_dirs/` 为什么不静态入 git

这组夹具要验证 `ShouldSkipDir` 硬排除表(`.git`/`build`/`node_modules`/
`.evidence`),必须真有一个**字面量叫 `.git` 的子目录**,可 git 把 `.git`
当仓库边界保留字,没法把"内容树里有个子目录恰好叫 `.git`"这种东西当常规
文件提交(会被当成潜在的嵌入式仓库/边界标记,`git add` 根本不收)。这一层
下的 `build/` 也会被仓库根 `.gitignore` 的 `build/` 规则连坐忽略。

处理办法:

1. 仓库 `.gitignore` 加了一条 `tests/fixtures/search/corpus/excluded_dirs/`,
   本地生成了也不会被 git 追踪。
2. `build_corpus.sh` 仍会生成这组目录(供本地手动核对),但正牌来源是
   `tests/manual/search_golden_driver.cpp` 里的
   `MaterializeExcludedDirsFixture()`——golden 驱动跑之前会自己现造这五个
   文件,不依赖盘上是否已经跑过 `build_corpus.sh`。
3. 未来 P0-5 的真 rg 差分工具照抄同一份现造逻辑(或直接调用
   `build_corpus.sh`),不要指望这组文件能从 git 签出直接拿到。

## Windows 下为什么不可测:非法 UTF-8 文件名

`posix_only/make_illegal_utf8_filename.sh` 只在 POSIX 上有意义。原因,以及
在本 worktree(Windows)上实测到的现象:

- Windows 的路径本质是 UTF-16(Win32 API 层面),没有"文件名是一段任意
  字节、可以不是合法 UTF-8"这个概念;POSIX 文件名则是"除 `/` 和 `\0`
  外任意字节序列",两边的路径模型不是同一件事。
- 实测:在这台 Windows worktree 上用 Git Bash(MSYS)跑这个脚本,**没有
  报错**,但 `ls -b` 显示出来的文件名是
  `lone_continuation_<替换字符>\200.txt` 这类——说明 MSYS 运行时把无法
  编码进 UTF-16 的字节,换成了它自己的私有代理转义方案(为了在自己的
  运行时内部往返),磁盘上真正落下的 NTFS 文件名并不是"原始那个非法
  字节 `0x80`",而是 MSYS 生造的另一套编码。这不是"能测,只是麻烦",
  而是"测出来的东西名不副实,冒充不了 POSIX 语义",故如实标注跳过,
  不伪造一份看着像但其实不是的夹具。
- 因此 P0-1 只交付这份脚本本身(POSIX 上执行才有意义),不在这个
  Windows worktree 里生成、也不静态提交任何文件名夹具。P0-4/P0-5 到了
  Linux/macOS CI 再用它现造、现测、现丢弃。

## golden 复现方式

```powershell
cmake --build --preset debug --target search_golden_driver
build\debug\tests\Debug\search_golden_driver.exe golden <输出JSON路径>
```

golden JSON 是数组,每个元素是一条场景的固化结果,字段:

| 字段 | 含义 |
|---|---|
| `id` | 场景名,快照比对按它对齐 |
| `mode` / `pattern` / `path` / `glob` | 喂给 `SearchTool::execute()` 的入参(`path` 是相对 `corpus/` 的路径,跨机器可移植,不落绝对路径) |
| `is_error` | `Tool::Result::is_error` |
| `hit_count` | 命中行数(grep)或命中文件数(glob),已排除"没搜到"哨兵文案与截断提示行 |
| `truncated` | 输出里是否带"……(结果超过 100 条,已截断…)"提示 |
| `notice_present` / `notice_over_threshold` | 是否带观察边界读取提示(§7.2 那一行),**只存布尔标记,不存提示文本本身**——提示文本含绝对路径,为了 golden 跨机器可移植特意剥掉 |
| `hits_sorted` | 命中行/命中路径,**排过序**——遍历顺序不是合同的一部分(旧内核的 `recursive_directory_iterator`、ripgrep 并行 walker 都一样),比较时按集合比,不比原始行序 |
| `hits_sha256` | `hits_sorted` 拼接(每条后跟 `\n`)后的 sha256,给比对一个快速对比锚点 |

当前后端(随包 ripgrep)的两次快照之间比较,按 `id` 对齐后比
`is_error`/`hit_count`/`truncated`/`hits_sorted`(集合意义上)/`hits_sha256`。
SearchTool 走默认构造(生产定位路 `ExecutableDir()/libexec/rg`),所以
跑之前得让驱动 exe 旁边有 `libexec/rg.exe`——把驱动拷进发行包或安装位
里跑,顺手就验了包内布局。

## bench 复现方式

```powershell
cmake --build --preset debug --target search_golden_driver
build\debug\tests\Debug\search_golden_driver.exe bench <输出JSON路径> [语料目录]
```

第二个参数是要跑基准的目录(默认本仓 `src/`,即 todo 说的"中/大语料"里的
中档)。JSON 里每条 query 记 7 轮 wall time 原始样本、P50/P95、首轮单独
拎出来(穷人版"冷热盘分开",不是 P0-7 要求的正式冷热盘门槛)、命中行数、
是否出错。数据解读见迁移文档。`bench/old_kernel_bench_src.json` 是 P0-1
用旧内核采的冻结基线,别往它上面写——新基准另起文件,旧文件是 P0-7
"旧内核"一列的唯一数据源。

## 幂等重建

改了 `build_corpus.sh` 或怀疑本地 `corpus/` 被污染,直接重跑:

```bash
bash tests/fixtures/search/build_corpus.sh
```

脚本会先 `rm -rf corpus/` 再重建,不用手动清。
