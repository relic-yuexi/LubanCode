# 实验 B1:compact 位置探针(首仗,装置已立;2026-09-05 改问答判卷)

单子《工具与上下文治理量化评测》§二 B1。出处 *Lost in the Middle*(Liu et al., TACL 2024)——只借采样法:K 条关键事实(needle)按受控深度({0,5,…,95}% 共 20 档)插进仿真实会话的长上下文,看三处理各把"位置-命中曲线"压成什么样。与 B2 主基准 Microsoft Lost in Conversation(`../compaction_benefit/`)是两码事,别混名。

本目录是**装置阶段**:假后端全链跑通,问答判卷确定性;真模型真跑见文末 `--real` 预留,验收不含真跑。

## 判卷三铁律(2026-09-05 工头令,单子 §二 B1 记账段)

1. **问答驱动判卷**:needle 答案设计为短规范值(名字-号码形,如 `青梧-1009`/`Qingwu-1009`),判卷只看**模型答没答对**——归一化(ASCII 小写、剥空白/全角/零宽)后比对模型回答与期望值/冲突旧值,不再扫视图找原词。工头令原判:摘要本分就是换说法,"视图内找原词"的子串判卷把好摘要冤枉死(LoCoMo 纠偏案同族病),禁它充当 compact 臂成绩。
2. **触发条件贴生产**:三处理的热冷区切分与阈值一律用产品默认,驱动不自设新数——microcompact 走 `StructuralCompressionOptions{}` 默认(long_result_bytes=8192 / preview_bytes=256 / min_compressible_bytes=512);compact 走 `CompactOptions{}` 默认 + `kDefaultHotZoneTokens`=12000。预算窗口收口见下文"窗口口径"。
3. **needle 分层**:金账带 `layer` 字段,`contract`(合同类)与 `evidence`(证据类)各半分开记账——"设计如此"(contract 按产品设计该进 manifest 逐字收编)与"意外丢失"(evidence 本分是被摘要)不混锅。

## 目录件

| 文件 | 作用 |
|---|---|
| `generate.py` | 造稿器:底稿(段 + user 消息)与金账(带 layer/question),seed 可复现,自带断言与 `--self-check` |
| `eval_driver.cpp` | 三处理驱动 + 问答判卷:底稿 → FULL/microcompact/compact 三版请求视图 → grounded 假问答 → 判卷落 JSONL;进程内自检(判卷单测+失败注入)先行 |
| `pipeline.py` | 全链编排(ctest 入口 `eval.compact_position.pipeline`):造稿 → 驱动 → collect,链上断言 |
| `collect.py` | 出四件表:位置-命中曲线(分层)、中段存活率、compact 反救中段、折叠分桶(吃 `../collect_common.py`,csv+md 恒出) |
| `results/` | gitignored:底稿 `drafts/`、金账 `needle_gold.jsonl`、原始账 `raw_position_probe.jsonl`、三处理视图 `views/`、collect 产物 |

## 底稿与金账

- **形状**:96 段仿 read_file 的 tool result(中英混排,长度三档:short<1280B / medium / long>8192B,分布 30/45/25)+ 24 条 user 文本消息(开工 1 + 每 4 段续读 1),驱动拼成真实会话史:user 开工 → 每 4 段一个 turn(assistant tool_use + user tool_result 配对,turn 间 assistant 收口 + user 续读)→ assistant 收口 → user 终问。turn 取小是因为 compact 的 12k-token 热区要装得下尾部两三 turn,位置轴才有区分度。
- **needle 每档四枚(两类各半,铁律 3)**:
  - `evidence/recall`——一段一句事实,落段中段(插入带 25%~75%,long 段保证离头尾各 ≥384B,比折叠预览 256B 更远);
  - `evidence/conflict`——同一配置项旧值在前、新值在后,**同 path 再读**触发折叠路 NewVersion 分支;
  - `contract/recall`——用户约定,落 user 消息正文("由用户定死为 X,全程照此执行");
  - `contract/conflict`——用户改约,旧约在前 user 消息、新约在后 user 消息("已从旧改定为新,以本条约定为准")。
- **金账**一行一 needle:`fact_id / layer / probe_kind / carrier(tool_result|user_turn)/ position_pct(设计档)/ actual_position_pct / lang / key / question(题面)/ expected_value / old_value / source 引用(段:seg_index、path、tool_use_id、offset;user 消息:slot、old_slot)/ seg_length_class / seed`。
- **值形四族**(互不串号,填充数字一律 ≤999 或带小数点):evidence recall `青梧-1009..2738`/`Qingwu-…`、evidence conflict 旧 `玄序-3100..4107` 新 `玄序-6100..7373`、contract recall `承影-4400..5293`/`Chengying-…`、contract conflict 旧 `白鹭-7600..8417` 新 `白鹭-8600..9721`。zh/en 两套措辞同 fact_id 两形,防判卷吃措辞。
- **参数化**:`--segments --repeats --langs --positions --needles-per-position --seed-base --long-result-bytes`。档 0 的 conflict 没有更早的空间(旧值落 0 位、新值挪后,设计档照记、实际档进金账),evidence 与 contract 同规则。
- **断言**(每次生成必跑):值唯一性(recall 值 1 次;conflict 旧值恰 2 次——旧声明一次+新声明里提一次,全文含 user 消息)、插入带、旧声明严格在新声明之前(段下标或消息序位)、档表全覆盖、carrier 与 source 字段自洽、同 seed 逐字节一致。

## 三处理怎么接(铁律 2:全用产品默认)

同一份底稿(同一 `api::Message` 历史),只换压缩路:

| 处理 | 接法 | 模型 |
|---|---|---|
| FULL | 原文直发,零处理对照基线 | 无 |
| microcompact | `agent::CompressWorkingView`(产品无损结构压缩折叠路,in-process,默认参数;fresh memo 一次性定形,不带 artifact 仓) | **无**——问答方是 grounded 脚本 |
| compact | `agent::Compact`(六栏摘要路)+ `BuildCompactedHistory`(热区 kDefaultHotZoneTokens=12000) | 假后端替身:固定形态六栏摘要 + ```json manifest,过四道验收;**摘要内容是假的,只验管道**,账里 `summary_fake=true`。热区保留是真实行为 |

**窗口口径(收口)**:compact 臂预算窗口 `budget.window_tokens = nullopt`——这与生产 `BuildCompactOptions` 在"压缩路由与模型目录都查不到窗口"时的形态**一致**(CompactBudget 语义:窗口未知不拦截,但如实记"未按窗口校验");装置 fake 模型无目录条目即此形态,`treatment_stats.jsonl` 里 `budget_window_tokens=null` 落账,不假装核过。`output_reserve_tokens=4096` / `protocol_headroom_tokens=2048` 用 CompactBudget 默认——生产路仅在窗口已知时才按 `BuildContextBudgetPlan` 重算 headroom,窗口未知即默认值,同形。真跑 `--real` 接线时应照生产路由查模型目录窗口(同 `interactive_session_assembly.cpp` 的 BuildCompactOptions)。

## 问答判卷(铁律 1)

流程:每 needle 一问(金账 `question` 字段,conflict 问"以最新为准")→ 问答方读**处理后的视图**作答 → 判卷归一化比对回答:

- **recall**:回答含期望值 → `hit`;不含 → `lost`。
- **conflict**:回答含新值 → `hit`(回答顺带提及旧值不算错——"已由旧变更为新"本就带旧值);不含新值而含旧值 → `stale`(superseded 旧值不得作答,拿旧值答与答不出同记 miss,stale 单列);两值都不含 → `lost`。

与旧子串规则同骨,只是比对对象从"视图文本"换成"模型回答"。

**装置阶段问答方 = `fake-grounded-v1`**(脚本化假后端,零真请求):忠实于所见视图作答——看见新值答新值(整句,带措辞噪声,判卷归一化抽值)、只见旧值答旧值、都不见答"未提及"(句中绝不出现任何值)。这既验管道与判卷逻辑,又保住装置阶段的真语义信号(折叠丢的视图上 grounded 必然答不出,stale 陷阱视图上必然答旧值)。真跑阶段同一只**实验模型**读三版视图答题,判卷器不动。

**视图级子串在场**(`new_in_view`/`old_in_view`)保留,仅作 **FULL 与 microcompact 臂的辅助诊断列**——折叠是机械截断,原词在场确实是好信号;compact 臂成绩只认问答,该臂诊断列记 unavailable。

**负路径自证**(判卷器三型真红过,两处):驱动 `RunSelfChecks` 显式注入失败回答——recall 答不出/答错值判 lost;conflict 答旧值判 stale、答不出/答无关值判 lost,并断言这些判定 `hit=false`;微型管道里造"旧值活短段、新值折长段"的 conflict,grounded 必答旧值、判卷必 stale。主链再实证:驱动收尾断言全链账 stale>0 且 lost>0,否则 exit 1。

单测两处:驱动进程内 `RunSelfChecks`(归一化、问答判卷三型+失败注入、grounded 作答器三路、微型管道、确定性),自检不过 exit 1;`collect.py` doctest 钉中段区间、计数与反救公式。

## 跑法

```sh
# ctest(先编目标):
cmake -B build/eval -G "Visual Studio 17 2022" -DLUBANCODE_BUILD_EVAL=ON .
cmake --build build/eval --target eval_compact_position --config Release
ctest --test-dir build/eval -C Release -L eval.compact_position

# 手动分步:
python generate.py                          # 底稿+金账(results/)
build/eval/tests/eval/Release/eval_compact_position.exe   # 三处理+问答判卷
python collect.py --raw results/raw_position_probe.jsonl --out-dir results
python pipeline.py --python python \
    --driver build/eval/tests/eval/Release/eval_compact_position.exe --root .
```

## 产出

`results/` 下(可重生成):`position_curve.{csv,md}`(处理×层×探针×20 档问答命中率+stale/lost 细分)、`mid_survival.{csv,md}`(中段=位置 30~70 档,分层+视图诊断列)、`compact_rescue.{csv,md}`(compact 臂中段命中率 vs FULL 同档,`rescue_delta_pp`=差值百分点——**反救中段观测点**)、`fold_survival.{csv,md}`(evidence 层段长档分桶)、`raw_position_probe.jsonl`(一行一判,含 question/model_answer/verdict 三件)、`treatment_stats.jsonl`(视图字节/折叠账/热区账/预算形态)、`views/`(三处理请求视图,审计用)。零分母记 `unavailable` 不填 0。

## 假后端全链数字(2026-09-05,commit 见账)

2400 案公式:20 档 × 2 措辞 × 3 处理 × 5 重复 × **4 桶**(层 2 × 探针 2)= 600 案/桶 × 4;金账 800 needle(evidence 400 / contract 400 各半),底稿 10 份(2 措辞 × 5 重复),每份 ~380KB。

| 处理 | 层 | 总 hit | 中段(30~70)hit | stale |
|---|---|---|---|---|
| FULL | 全部 | 800/800 | 100%(完整性锚) | 0 |
| microcompact | contract | 400/400(100%) | 100%(user 消息不折叠) | 0 |
| microcompact | evidence | 316/400(79.0%) | 77.78% | **37(中段 18 例)** |
| compact(摘要为假) | evidence | 18/400(4.5%,全在 90/95 档热区) | 0% | 0 |
| compact(摘要为假) | contract | 38/400(9.5%,85/90/95 档热区 user 消息) | 0% | 0 |

`compact_rescue` 装置读数:四桶 `rescue_delta_pp` 全 -100(假摘要不含任何 needle,compact 中段命中只可能来自热区尾部)——这正是"子串判卷全瞎、问答判卷才可见"的反转观测点,真跑后该列就是 compact 是否反救中段的判据。

**分层首笔信号(铁律 3 的价值)**:microcompact 臂两层命运分明——contract 层 100% 存活(user 文本消息折叠路根本不碰),evidence 层长段 0% 存活(段中段事实被换成头尾 256B 预览即真丢)。混在一锅会把"折叠专打工具输出、不碰用户约定"这个设计事实糊掉。evidence conflict 的 **stale 陷阱仍在**:37/400 案旧值声明活着(落短/中段)、新值被折掉(落长段)——grounded 模型照旧值作答即错,问答判卷把它单列出来了。视图体积 380KB→137KB(省 ~64%)。

compact 臂管道全通(采样→验收→archive→热区),命中只来自 12k-token 热区尾部;摘要为假,语义命中率(含分层差距、反救中段)待真模型。

## 真跑路预留(只备不点炮)

装置验收**不含真跑**:不发真请求、不读真钥匙、不写真 `~/.lubancode`。真跑三条命令的样子(**问答模型 = 实验模型 ccmoon/gpt-5.6-luna**,单子 §二 B 钉死;压缩采样与视图问答同一只模型——这正是生产形态,压缩后读上下文的就是下一轮模型):

```sh
# 1) 钥匙进临时 USERPROFILE(拷真 config.json 的既有章法;Windows 下 config
#    在 %USERPROFILE%\.lubancode\config.json,别拿真家做实验场):
set USERPROFILE=D:\tmp\lubancode-eval-profile
#    (该目录下 .lubancode\config.json 含 provider ccmoon 的 base_url/api_key)

# 2) 真模型跑三处理 + 问答(模型钉死 ccmoon/gpt-5.6-luna):
#    驱动加 --real 后:FakeStreamingBackend 换 backend_stack.cpp 的真后端工厂
#    (provider→wire 路由),compact 采样走真 SampleRequest;问答方同模型读三版
#    视图逐 needle 作答(视图为上下文、金账 question 为题面),账记真模型 id
#    与 answering_model;compact 预算窗口照生产路由查模型目录。
eval_compact_position --drafts results/drafts --results results_real ^
    --real --provider ccmoon --model gpt-5.6-luna

# 3) 同一 collect 入口出表(raw 指向真跑账;compact_rescue 的 rescue_delta_pp
#    与分层差距即主读数):
python collect.py --raw results_real/raw_position_probe.jsonl --out-dir results_real
```

`--real` 尚未接线(装置阶段不点炮,驱动对 `--real` 显式报错退 2);接线时驱动里假后端替身换真后端工厂、问答方换实验模型、`summary_fake` 置 false,FULL/microcompact 两臂视图不变(它们不需要压缩模型,问答方换真模型即可)。

## 边界

装置全走本目录 + `../CMakeLists.txt` 注册段;`src/` 一行不动(三处理全吃产品导出的纯函数/公开接口)。量出要改产品的(如长段折叠保事实、摘要提示词、contract 收编 manifest),另立单走 src,本装置只量不修。todo_write 守恒(required_open_items)不在装置范围——装置的 contract 层用"用户约定/偏好"形,活动待办守恒是 B2 三本账主实验的活。
