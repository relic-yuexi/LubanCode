# 实验 B1:compact 位置探针(首仗,装置已立)

单子《工具与上下文治理量化评测》§二 B1。出处 *Lost in the Middle*(Liu et al., TACL 2024)——只借采样法:K 条关键事实(needle)按受控深度({0,5,…,95}% 共 20 档)插进仿真实会话的长上下文,看三处理各把"位置-命中曲线"压成什么样。与 B2 主基准 Microsoft Lost in Conversation(`../compaction_benefit/`)是两码事,别混名。

本目录是**装置阶段**:假后端全链跑通,判卷确定性;真模型(cmmoon/gpt-5.6-luna)跑法见文末预留,验收不含真跑。

## 目录件

| 文件 | 作用 |
|---|---|
| `generate.py` | 造稿器:底稿(N 段填充 + needle 按档插入)与金账,seed 可复现,自带断言与 `--self-check` |
| `eval_driver.cpp` | 三处理驱动 + 确定性判卷:底稿 → FULL/microcompact/compact 三版请求视图 → 判卷落 JSONL;进程内自检(判卷单测)先行 |
| `pipeline.py` | 全链编排(ctest 入口 `eval.compact_position.pipeline`):造稿 → 驱动 → collect,链上断言 |
| `collect.py` | 出三件表:位置-命中曲线、中段存活率、折叠分桶(吃 `../collect_common.py`,csv+md 恒出) |
| `results/` | gitignored:底稿 `drafts/`、金账 `needle_gold.jsonl`、原始账 `raw_position_probe.jsonl`、三处理视图 `views/`、collect 产物 |

## 底稿与金账

- **形状**:96 段仿 read_file 的 tool result(中英混排,长度三档:short<1280B / medium / long>8192B,分布 30/45/25),驱动拼成真实会话史:user 开工 → 每 4 段一个 turn(assistant tool_use + user tool_result 配对,turn 间 assistant 收口 + user 续读)→ user 终问。turn 取小是因为 compact 的 12k-token 热区要装得下尾部两三 turn,位置轴才有区分度。
- **needle**:每档 2 枚——recall(直接召回,一段一句事实)+ conflict(更新冲突,同一配置项旧值在前、新值在后,**同 path 再读**触发折叠路 NewVersion 分支)。needle 落段中段(插入带 25%~75%,long 段保证离头尾各 ≥384B——比折叠预览 256B 更远)。
- **措辞两形**:zh/en 两套底稿同 fact_id 两措辞,值各一形(青梧-XXXX / Qingwu-XXXX),防判卷吃措辞。
- **金账**一行一 needle:`fact_id / position_pct(设计档) / actual_position_pct / lang / probe_kind / expected_value / old_value / seg_index / seg_length_class / offset_pct_in_seg / path / tool_use_id / old_seg_index / seed`。
- **参数化**:`--segments --repeats --langs --positions --needles-per-position --seed-base --long-result-bytes`。档 0 的 conflict 没有更早的空间:旧值落 0 段、新值挪第 4 段,设计档照记、实际档进金账。
- **断言**(每次生成必跑):值唯一性(recall 值 1 次;conflict 旧值恰 2 次)、插入带、旧段严格在新段前、档表全覆盖、同 seed 逐字节一致。

## 三处理怎么接

同一份底稿(同一 `api::Message` 历史),只换压缩路:

| 处理 | 接法 | 模型 |
|---|---|---|
| FULL | 原文直发,零处理对照基线 | 无 |
| microcompact | `agent::CompressWorkingView`(产品无损结构压缩折叠路,in-process,默认参数 long_result_bytes=8192 / preview_bytes=256 / min_compressible_bytes=512;fresh memo 一次性定形,不带 artifact 仓) | **无**——判卷是真语义 |
| compact | `agent::Compact`(六栏摘要路)+ `BuildCompactedHistory`(热区 kDefaultHotZoneTokens=12000) | 假后端替身:固定形态六栏摘要 + ```json manifest,过三道验收(≥40 码点、manifest 可解析、goal 非空);**摘要内容是假的,只验管道,语义待真跑**,账里 `summary_fake=true`。热区保留是真实行为,该臂存活只可能来自热区 |

窗口口径:compact 臂 `budget.window_tokens = nullopt`(装置未按窗口校验,如实记录)。

## 判卷规则(确定性,独立自检)

归一化(ASCII 小写、剥空白/全角空格 U+3000/不换行空格/零宽空格)后子串匹配:

- **recall**:期望值在视图 → `hit`;不在 → `lost`。
- **conflict**(更新冲突):新值在 → `hit`;新值不在而旧值在 → `stale`(**superseded 旧值必须不作答**——拿旧值答与答不出同记 miss,stale 单列);两值都不在 → `lost`。新值声明里会提及旧值("已由 X 变更为 Y"),新值在场时旧值串跟着在场属正常,verdict 只看新值;新值不在时旧值串只可能来自旧声明,stale 判定不虚。

单测两处:驱动进程内 `RunSelfChecks`(归一化、三类判卷、微型管道——长段中段 needle 必折/短段必活/同 path 必触发 NewVersion/确定性逐字节、FULL 全存活),自检不过 exit 1;`collect.py` doctest 钉中段区间(30~70 含端点)与计数。

## 跑法

```sh
# ctest(先编目标):
cmake -B build/eval -G "Visual Studio 17 2022" -DLUBANCODE_BUILD_EVAL=ON .
cmake --build build/eval --target eval_compact_position --config Release
ctest --test-dir build/eval -C Release -L eval.compact_position

# 手动分步:
python generate.py                          # 底稿+金账(results/)
build/eval/tests/eval/Release/eval_compact_position.exe   # 三处理+判卷
python collect.py --raw results/raw_position_probe.jsonl --out-dir results
python pipeline.py --python python \
    --driver build/eval/tests/eval/Release/eval_compact_position.exe --root .
```

## 产出

`results/` 下(可重生成):`position_curve.{csv,md}`(处理×探针×20 档命中率+stale/lost 细分)、`mid_survival.{csv,md}`(中段=位置 30~70 档)、`fold_survival.{csv,md}`(段长档分桶)、`raw_position_probe.jsonl`(一行一判)、`treatment_stats.jsonl`(视图字节数/折叠账/热区账)、`views/`(三处理请求视图,审计用)。零分母记 `unavailable` 不填 0。

## 假后端全链数字(2026-09-05,commit 见账)

600 案公式(20 档 × 2 措辞 × 3 处理 × 5 重复)× 2 探针类 = **1200 案**;底稿 10 份(2 措辞 × 5 重复),每份 ~380KB。

| 处理 | recall hit | conflict hit | conflict stale | 中段(30~70)hit |
|---|---|---|---|---|
| FULL | 200/200 | 200/200 | 0 | 100%(完整性锚) |
| microcompact | 158/200(79.0%) | 158/200(79.0%) | **37**(18.5%) | 77.78% |
| compact(摘要为假) | 9/200(4.5%,全在 90/95 档热区) | 9/200 | 0 | 0% |

**microcompact 初判(第一笔真语义信号)**:折叠路与上下文位置无关、与 needle 落段长度强相关——fold_survival 表里 short/medium 档 100% 存活,long 档(>8192B)0% 存活:落段中段的事实被换成头尾 256B 预览即真丢,20 个位置档全都一样(位置曲线平的,波动只是各档段长分布)。更要紧的是 **stale 陷阱**:37/200 conflict 案旧值声明活着(落短/中段)、新值被折掉(落长段)——模型照旧值作答就是错。视图体积 380KB→137KB(省 ~64%)。**B2 指向**(单子 B1→B2):ToolEvidence 探针优先打折叠区长段,重点布"同键改版"的更新冲突。

compact 臂管道全通(采样→验收→archive→热区),命中只来自 12k-token 热区尾部(90/95 档);摘要为假,语义命中率待真模型。

## 真跑路预留(只备不点炮)

装置验收**不含真跑**:不发真请求、不读真钥匙、不写真 `~/.lubancode`。真跑三条命令的样子:

```sh
# 1) 钥匙进临时 USERPROFILE(拷真 config.json 的既有章法;Windows 下 config
#    在 %USERPROFILE%\.lubancode\config.json,别拿真家做实验场):
set USERPROFILE=D:\tmp\lubancode-eval-profile
#    (该目录下 .lubancode\config.json 含 provider ccmoon 的 base_url/api_key)

# 2) 真模型跑 compact 处理(模型钉死 ccmoon/gpt-5.6-luna,单子 §二 B):
#    驱动加 --real 后,FakeStreamingBackend 换 backend_stack.cpp 的真后端工厂
#    (provider→wire 路由),compact 采样走真 SampleRequest;账记真模型 id。
eval_compact_position --drafts results/drafts --results results_real ^
    --real --provider ccmoon --model gpt-5.6-luna

# 3) 同一 collect 入口出表(raw 指向真跑账):
python collect.py --raw results_real/raw_position_probe.jsonl --out-dir results_real
```

`--real` 尚未接线(装置阶段不点炮);接线时驱动里假后端替身换成真后端工厂、`summary_fake` 置 false,FULL/microcompact 两臂不变(它们本就不需要模型)。真跑另还欠一步"模型在视图上作答"的问答驱动(位置-命中的 U 形是模型现象,视图判卷只是第一层)——届时立小单。

## 边界

装置全走本目录 + `../CMakeLists.txt` 注册段;`src/` 一行不动(三处理全吃产品导出的纯函数/公开接口)。量出要改产品的(如长段折叠保事实、摘要提示词),另立单走 src,本装置只量不修。
