# compact 评测主基准备料:Microsoft Lost in Conversation(LiC)

Q2 单《工具与上下文治理量化评测》实验 B2 的外部主基准。本目录是**备料**,不跑实验:数据本体不进仓(单子 §一合同),进仓只有四件——

| 文件 | 作用 |
|---|---|
| `datasets.lock.json` | 钉来源 URL、revision、许可、SHA-256、拉取日期 |
| `fetch_datasets.py` | 按锁取数:校验哈希、解包、幂等可重跑 |
| `smoke_set.json` | 七族各 5 题共 35 题的 smoke 清单(只有 task id 与理由,无数据正文) |
| `README.md` | 本文:盘点、判卷机制、适配器草案 |

数据本体落 `cache/lost_in_conversation/`(gitignore 点名,见仓库 `.gitignore`)。

提醒一句:B1 位置探针借的 *Lost in the Middle*(Liu et al., TACL 2024)是另一码事,只借采样法;本文的 LiC 借任务集。两者别混名。

- 上游:https://github.com/microsoft/lost_in_conversation
- 论文:*LLMs Get Lost in Multi-Turn Conversation*(arXiv:2505.06120,ICLR 2026,据仓内 `.msr/project.md` 记载获 Outstanding Paper Award)
- 锁定 revision:`c865793fe34a929d316119b0451d01bd9183bcfd`(2026-06-09)

---

## 一、盘点:todo 记载 vs 实测

todo B2 段写"600 条分片指令 + reference/tests,MIT"。实测(以解包文件为证)有三处要校正:

| # | todo 记载 | 实测 | 证据 |
|---|---|---|---|
| 1 | 共 600 条 | 主数据文件 `data/sharded_instructions_600.json` 实有 **627 条**;另有一个单独文件 `data/sharded_translation.json` 存 **30 条** translation 族;合计 **657 条** | 两个 JSON 逐一数过 |
| 2 | 七族(code/database/actions/math/data2text/summary/translation) | 族名全对,但 **translation 不在主文件里**,单独存放、单独加载;主文件只有六族。论文主体口径也是六族:仓内 `.msr/project.md` 写 "6 tasks, 600 sharded instructions",README §Intended Use 也写 "Six tasks"——translation 是后补的第七族(README §Repository Contents 与数据 schema 里都有它) | `data/sharded_translation.json`、`tasks/translation/task_translation.py`、`.msr/project.md` |
| 3 | MIT | **对**。`LICENSE` 头两行:"MIT License / Copyright (c) Microsoft Corporation.";GitHub API license 字段也是 MIT(2026-09-04 查) | `LICENSE` |

"600"这个数与文件名一致但与内容不符——文件名叫 600,里面 627。上游论文口径 6 任务 600 条(仓内 `.msr/project.md`),大概后来补样没改名。下文一律用实测数。

另有两处上游 README 自身的小错,适配时别照抄:

- LiC README §Dataset Contents 写分片内层字段是 `shard_text`,实际数据里是 **`shard`**(summary 族还多一个 `doc_idxs`)。全量核对:主文件 3742 个分片、translation 文件 146 个分片,内层键只有 `("shard_id","shard")` 与 `("shard_id","shard","doc_idxs")` 两种。
- `tasks/math/task_math.py` 的 `get_dataset_file()` 返回 `data/sharded_math.json`——此文件不存在,是死路径。模拟主流程不受影响(`run_simulations.py` 直接读 `--dataset_file` 参数再按 `task` 字段过滤),但 `RecapSimulator`(`simulator_recap.py`)与 `tasks.py` 的 `__main__` 走 `get_samples()` 会踩空。

## 二、任务族全景

七族全部源自公开单轮基准,做法一致:把一条完整单轮指令拆成若干"分片"(shard),逐轮喂给模型,每片补一点信息。各族底数与分片统计(实测):

| 族 | 底数来源 | 题数 | 分片数(最小/中位/最大) | 每题自带的判卷料 |
|---|---|---|---|---|
| code | HumanEval 45 + LiveCodeBench 55(lcb_easy 22 / lcb_medium 33) | 100 | 4 / 7 / 10 | `public_test_cases`(+`private_test_cases`,LCB 题带 zlib+base64 压缩)、`metadata.func_name`、`prompt`/`question_content`、`starter_code` |
| database | Spider(dev/val) | 107 | 3 / 4 / 7 | `reference_sql`、`schema_sql`、`db_id`、`spider_difficulty`(medium 69 / hard 26 / extra 12) |
| actions | BFCL(Gorilla) | 105 | 4 / 6 / 10 | `function`(函数表)、`reference_answer`、`test_category`(全部 105 条都是 `parallel`)、`language`(全部 Python) |
| math | GSM8K | 103 | 4 / 5 / 12 | `answer`(带 `####` 终答)、`question` |
| data2text | ToTTo | 120 | 6 / 6 / 7 | `references`(多条参考句)、`table_html`、`table_highlighted_html`、`fewshot_descriptions`、`metadata` |
| summary | SummHay(Salesforce) | 92 | 6 / 7 / 10 | `insights`(要点清单)、`insightid2ref_citations`、`documents`(含 `document_index`)、`query`、`domain`(conv/news) |
| translation | WMT news 德→英 | 30 | 3 / 5 / 5 | `document_de`、`document_en`(参考译文) |

合计 657 题、3888 个分片。难度与长度错得开:database 3 片就完事,math 拖到 12 片;够布"距截点 turn 数"的分桶。

## 三、分片格式:任务怎么拆、约束怎么补

数据结构(实测,以 math 题为例):

```json
{
  "task_id": "sharded-GSM8K/1246",
  "task": "math",
  "shards": [
    {"shard_id": 1, "shard": "what's Ara's total basketball score over four years?"},
    {"shard_id": 2, "shard": "Ara has been on the school basketball team for four years now"},
    {"shard_id": 3, "shard": "Ara plays 40 games per year on average"},
    {"shard_id": 4, "shard": "she scores 21 points in each game"}
  ],
  "question": "Ara joined the school basketball team four years ago. ...(完整单轮题面)",
  "answer": "... #### 3360"
}
```

拆法一眼见底:**第一片是笼统目标,后面每片补一条事实或约束**。逐字再摘三族:

**database**(`sharded-spider-val-422-hard`,3 片,完整题面 "What is the name of the museum that had no visitor yet?"):

```
shard 1: unpopular museum analysis
shard 2: focus on museums that have not had any visitors at all
shard 3: return the name of the museum that meets the criteria
```

**actions**(`sharded-BFCL/parallel_46`,4 片,参考答案是四个 `calculate_winning_percentage` 调用):

```
shard 1: Find the winning percentage of the Lakers
shard 2: Also find the winning percentage of the Bulls
shard 3: Focus on the NBA season 2018
shard 4: Include results from the NBA season 2020 as well
```

**code**(`sharded-HumanEval/105`,8 片,题面是 `by_length` 函数补全):

```
shard 1: Turn digits into names in a list
shard 2: Starting with a list of numbers
shard 3: Sort numbers if they're between 1 and 9
shard 4: Then flip the list around
shard 5: Use the words for numbers: One to Nine
shard 6: If the list is empty, just give back an empty list
shard 7: Ignore any weird numbers that aren't between 1 and 9
shard 8: For instance, from [1, -1, 55] you'll get ['One']
```

data2text、summary、translation 三族的分片不是短句,是**整块材料逐轮投喂**:

- **data2text**(ToTTo):第 1 片给原始表格并立规矩("The description should be at most 30 words ... only respond with a single sentence");第 2 片给 10 条示范句;第 3 片给加了高亮的表格("must now focus on the highlighted cells");第 4 片起给元数据。约束逐步收紧,每轮都要重答。
- **summary**(SummHay):分片只装 `doc_idxs`(文档编号批次),正文在 `documents` 里。第 1 轮按 `prompts/summary/summary_full_prompt_{conv,news}.txt` 起头带第一批文档,之后每轮 user 话术固定:"I have found a few additional documents, please rewrite the summary considering all documents so far"(`tasks/summary/task_summary.py` 的 `populate_sharded_prompt`)。
- **translation**(WMT):德文文档切块逐轮给,每轮要求**重译全文至今为止的全部内容**,可改前译(`tasks/translation/task_translation.py` 的 `populate_sharded_prompt`)。

这三族的 user 轮是脚本直出的;其余四族(code/database/actions/math)的 user 轮由**模拟用户代理**(LLM)生成,下一节说。

## 四、会话机制:原仓怎么把分片跑成多轮

原仓 `run_simulations.py` + `simulator_sharded.py` 一套,三种角色全是 LLM(`model_openai.py`,OpenAI 兼容口):

1. **assistant**:被测模型。system prompt 由各族 `generate_system_prompt` 出(database 族嵌 schema,actions 族嵌函数表)。
2. **user agent**(模拟用户,`user_agent.py` + `prompts/user_agent.txt`):只管 code/database/actions/math 四族。规矩写在提示词里——每次至多揭露一个未揭露的分片,**必须换口气改写成口语**("Rephrase Shards ... Do not copy the shard verbatim"),但信息一条不能丢("you must make sure to include *all the information in the shard*");模型问无关问题就装傻("I don't know")。第 1 轮例外:分片 1 原文直发。
3. **system agent**(裁判,`system_agent.py`):每轮给 assistant 回复分类(answer_attempt / clarification / discussion),是 answer_attempt 才抽答案判卷。summary/translation/data2text 三族免分类——它们每轮都被要求直接作答。

判对了会话就提前收工;全部分片喂完还没对,循环到分片穷尽为止(每轮答得不对就继续,所以**判卷是逐轮发生的**,日志里每轮一条 `answer-evaluation`)。

对本研究最有用的旁证是 `simulator_recap.py`:论文的干预实验——多轮会话跑完后追加一条 user 消息,把完整题面(recap-full)或全部分片拼接(recap-concat)塞回去,让模型重答。`.msr/project.md` 记的结论:recap/snowball 这类干预**只救回约 15–20%**。这对 compact 是条硬基准:无损重述尚且救不全,有损摘要要过的门槛更高。

另两种单轮对照:`simulator_full.py` 的 **full**(完整题面一发入魂)与 **concat**(全部分片拼成一条消息)——正是 FULL/compact 对照里 FULL 一侧的原生形态。

## 五、判卷机制逐族(决定我们怎么自动判卷)

逐族读的 evaluator 源码,结论分三档:

| 族 | 判卷方式 | 档位 | 依据 |
|---|---|---|---|
| code | **可执行测试**:抽最后一个函数定义,跑 public+private 测试用例,6 秒超时,全过才算对 | 确定性,二值 | `tasks/code/task_code.py::evaluator_function` → `tasks/code/eval_code.py::check_correctness` |
| database | **执行匹配**:预测 SQL 与参考 SQL 各自在真实 Spider SQLite 库上执行,比对结果集(`plug_value=True`) | 确定性,二值 | `tasks/database/task_database.py` → `eval_spider_exec.eval_exec_match` |
| math | **归一化精确匹配**:终答取 `####` 之后,抽数字、去 `$`/千分位逗号/句点,字符串相等才对 | 确定性,二值 | `tasks/math/task_math.py::evaluator_function`(GSM8K/lm-evaluation-harness 同款归一化) |
| actions | **AST 校验**:解析函数调用序列,对参考答案查函数名+参数类型与取值(可接受值列表);无 LLM 参与 | 确定性,二值 | `tasks/actions/eval_bfcl.py::ast_checker`(105 条全是 parallel 类 → `parallel_function_checker_no_order`;`model_name` 参数只管函数名下划线/点号归一,不调模型) |
| data2text | **BLEU**:对 ToTTo 多参考句算 sacrebleu,分数 = BLEU/100,连续 | 确定性,连续 | `tasks/data2text/task_data2text.py::evaluator_function` |
| translation | **BLEU**:对单条参考译文算 sacrebleu,分数 = BLEU/100,连续 | 确定性,连续 | `tasks/translation/task_translation.py::evaluator_function` |
| summary | **LLM judge**:gpt-4o 逐条判 92 题的 insight 是否被覆盖(`prompts/summary/summhay_evaluation.txt`),再算 coverage/attribution/joint,主分数取 `joint_score`;`USE_TRAPI=1` 时换内部端点 t-gpt-4o | **需 LLM judge** | `tasks/summary/task_summary.py::evaluator_function` → `eval_summhay.py::evaluate_insights / compute_single_sample_results` |

判卷输入是"抽出的答案"不是原始回复。抽取策略四类(`system_agent.py`):code 走 AST 抽最后一个函数(规则化);summary/translation/data2text 取末轮全文(规则化);database 走 `prefix_suffix`、math 走 `gen`——这两类原仓用 LLM 抽,但 math 的 evaluator 自带正则抽取兜底,database 的 ```sql 代码块剥离也是纯规则,适配时都能规则化替代(见 §六)。

**两个环境硬约束**:

- **code 族判卷只在 Unix 跑得动**:`eval_code.py` 用 `signal.alarm` 做超时(L160/L226),Windows 没有 SIGALRM;LiC README §Setup 原话 "Simulation of the `code` task does not work on Windows"。LubanCode 主仓在 Windows——code 族判卷须挪 WSL 侧做(仓里已有 WSL 复现场惯例)。
- **database 族要另下 Spider 库**:`data/spider/readme.txt` 指路 test-suite-sql-eval 的 Google Drive,解压后须有 `data/spider/databases/<db_id>/<db_id>.sqlite`。量级 <5GB,属第二层外部数据,主 lock 不含,要跑 database 主报告时另锁。

## 六、LubanCode 适配器草案

### 6.1 分片 → 会话轮次

每个 LiC 样本 → 一场 LubanCode 会话:

- **system 轮**:照抄各族 `generate_system_prompt`(database 嵌 `schema_sql`,actions 嵌 `function` 函数表,其余用各族 system prompt 原文,`prompts/<族>/` 下都有)。
- **user 轮**:分片逐轮投喂。第一版走**脚本化重放**——分片原文按 `shard_id` 顺序直发,像 translation/summary/data2text 三族的原生做法;data2text/summary/translation 连话术都现成(各族 `populate_sharded_prompt`)。
- **assistant 轮**:被测模型自由作答,每轮都判(原仓同款:判对了提前收工,判不对喂下一片)。

**未决问题 A:要不要接原仓的 user-agent LLM 改写?** 论文口径的四族(code/database/actions/math)user 轮是 LLM 口语化改写的,不是分片原文。改写版更近真实会话,但多一只三方 LLM、引入改写噪声、复现要钉 user 模型与温度。建议:第一版(FULL/compact 主对照)用脚本化重放,保确定性;改写版作后续扩展臂。此裁决留给实验 B2 开工前定。

**未决问题 B:答案抽取的规则化边界。** database 的 SQL 抽取原仓用 LLM(`prefix_suffix`),规则化取末个 ```sql 块可能有少量抽取误差。建议 smoke 阶段双路并行(LLM 抽取 vs 规则抽取)对账一次,差小于 1 个点就固定规则化;summary 族 judge 无法规化,见下。

### 6.2 compact 切入点

分片逐轮补约束,早轮的目标与约束会被后续轮次推离窗口尾部——正是 compact manifest 要守住的东西。建议:

- 切点取**分片中后段**:默认 `ceil(0.6 × n_shards)` 轮后触发一次 compact;另扫 `{0.4, 0.6, 0.8}` 三档,与 B1 位置曲线互为指导(B1 中段掉多少,B2 就把探针布多密)。
- FULL 对照臂不压缩,其余一切钉死:同一样本、同一分片序列、同一截点、同一温度/seed/工具表/系统提示/输出预算,每组 ≥5 次(todo B2 原话)。
- 配对记分:同题同截点 FULL vs compact 的分数差;二值族报准确率差,连续族报 BLEU/joint 差;外加 token 与轮数两本账。

### 6.3 判卷复用

- 优先**直接 import 原仓 task 类**(把 `cache/lost_in_conversation` 挂上 `sys.path`,按 `tasks/tasks.py::get_task` 取),判卷逻辑零移植、跟着上游走;
- 判卷输入用 6.1 的答案抽取;code/database/math/actions 四族全自动;data2text/translation 全自动(连续分);
- **summary 族标"需 LLM judge"**:判卷模型、prompt、原始裁决全部留档(Q2 单 B1 同款纪律);FULL 与 compact 两臂必须**同一 judge 批次内**判,防 judge 漂移;
- code 族判卷路由到 WSL;database 族先解决 Spider 库下载并单独 lock(未决问题 C:Spider 库的 lock 条目与取数脚本,跑 database 主报告前补)。

### 6.4 指标口径

- 主指标:`compaction_benefit_ratio`(todo B2 原名)族内配对差;
- 二值族:accuracy 差 + 配对 bootstrap CI;连续族:分数差中位数 + P95;
- 按距截点 turn 数、分片长度(n_shards 分桶)分桶——smoke 集已经把长度极值布进去了;
- 零分母写 `unavailable`,不填 0(todo 纪律)。

## 七、smoke 集选取规则

`smoke_set.json` 35 题(七族各 5),只存 `task_id`/族/`dataset_file`/`n_shards`/理由,无数据正文;正文由 `fetch_datasets.py` 拉到 cache 后凭 id 关联。规则是**确定性的**,可从缓存数据复算:

1. 族内样本按 `task_id` 字典序排序;
2. 先取三席:**最短分片**、**最长分片**、**中位分片**(并列取 task_id 字典序最小)——量分片长度上下界与中位;
3. 再补组覆盖:code 按 `source`(humaneval/lcb_easy/lcb_medium)、database 按 `spider_difficulty`(medium/hard/extra)、summary 按 `domain`(news/conv),缺哪组补哪组的中位分片段;
4. 不足 5 席按四分位分片长度补齐。

选出来的构成:code 双源齐(HumanEval 3 + LCB 2)、database 三档难度齐(medium 3 + extra 1 + hard 1)、summary 双域齐;各族分片长度都摸到极值与中位。translation 族只有 30 题 3–5 片,极值即全覆盖。

## 八、许可与边界

- **MIT**,版权 Microsoft Corporation(`cache/lost_in_conversation/LICENSE`)。可用于评测,评测结果可公开引用与发表;引用时按上游要求 cite 论文(LiC README §Cite the Work 给了 BibTeX:laban2025llms)。
- 上游声明研究用途(README §Out-of-scope Use):不得用于替代人类研究、不得用于高风险决策领域;不得拿模拟会话对人类行为下结论。我们只拿来测 compact,不越界。
- 数据再分发注意:MIT 覆盖本仓代码与数据发布,但我们按自家合同**不把数据本体进仓**,只进 lock 与 id 清单,更干净。
- 第二层数据(用则另锁):Spider 数据库(test-suite-sql-eval,走 Google Drive)、SummHay 原始数据(`task_summary.py` 里的生成脚本会从 salesforce/summary-of-a-haystack 拉)——这两处的许可各自查,跑对应族主报告前另立 lock 条目。

## 九、复现取数

```sh
python tests/eval/compaction_benefit/fetch_datasets.py          # 取数+校验+解包
python tests/eval/compaction_benefit/fetch_datasets.py --check  # 只校验缓存
python tests/eval/compaction_benefit/fetch_datasets.py --force  # 强制重解包
```

直连 codeload 慢(本机实测 ~20KB/s)会超时,脚本自动换 github.com archive + 代理 `http://127.0.0.1:10808`;代理可用环境变量 `LIC_FETCH_PROXY` 覆盖(空串禁用)。断网时脚本报错并给四条排查建议,归档留在 `cache/downloads/` 可离线重解。
