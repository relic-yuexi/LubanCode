// 自进化闭环阶段 3:评测与基线(确定性优先)。
//
// 契约(docs/features/evolution/README.md"评测五道门")的落地口径:
//   - 静态门:复用 Package doctor(package::AnalyzePackage)做 schema/组件/
//     引用诊断,不另写一套;本模块只补密钥扫描与绝对路径扫描——发现即
//     error,记入评测账。
//   - 来源回放/留出:走确定性检查(file_exists/json_parses/file_contains/
//     command 一类可执行验收,§8.4 清单),不起真模型。没起模型、没测真实
//     服务之处,一律写 unverified,不冒充测过。
//   - 基线对照:父版或裸 Agent 的确定性指标账(baseline fixture JSON),
//     七项指标照契约记:成功率/验收通过、tool calls、tokens、墙钟、用户
//     确认次数、工作区写入。误报漏报首版判不了,明记 unverified。
//   - 独立 Evaluator 只在确定性证据之后:首版 Evaluator 是结构化的
//     "确定性结果汇总 + 未测之处清单",不接真模型;判词权重永远低于
//     测试与产物(所以行里的 verdict 字段留给后续的模型判词,这里不填)。
//
// 写盘规矩:本模块只产结果行,不落笔。落 eval-results.jsonl(只追加)与
// 状态迁移的唯一写口仍是 EvolutionCoordinator(coordinator.hpp)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::evolution {

// ---------------------------------------------------------------------------
// 验收检查项。acceptance 数组的元素两种形态:
//   - 纯字符串:人话描述,判不了,记 manual(skipped + unverified);
//   - 对象:{"kind": "...", ...} 可执行检查,确定性判 pass/fail。
//     kind 取 file_exists / json_parses / file_contains / command。
//     file_* 的 path 相对任务 workspace;command 整串按空白拆 argv,
//     platform::RunProcess 直接起进程(不经 shell 拼串,安全铁律),
//     cwd=workspace,超时吃计划的 budget.timeout_ms。
// ---------------------------------------------------------------------------

enum class AcceptanceCheckKind { Manual, FileExists, JsonParses, FileContains, Command };

std::string ToString(AcceptanceCheckKind kind);
std::optional<AcceptanceCheckKind> ParseAcceptanceCheckKind(const std::string& text);

struct AcceptanceCheck {
    AcceptanceCheckKind kind = AcceptanceCheckKind::Manual;
    std::string raw;      // 原文回显:manual 的描述 / 对象检查的一句话
    std::string path;     // file_exists / json_parses / file_contains 用
    std::string text;     // file_contains 用
    std::string command;  // command 用(整串)
};

// ---------------------------------------------------------------------------
// eval-plan.json(schema 1)。frozen 契约的 replay[]/holdout[]/baseline/
// budget 四节全收;阶段 3 的可执行口径另认两个可选扩展(README 评测节有
// 记):任务可带 workspace(相对候选目录的夹具目录);baseline 可带
// fixture(基线确定性指标账 JSON 的相对路径)。
// ---------------------------------------------------------------------------

struct EvalTask {
    std::string task_id;     // replay 用 source_id,holdout 用 task_id;解析时归一
    std::string task;        // 任务描述
    std::string workspace;   // 相对候选目录;空 = 无夹具(纯人工验收)
    std::vector<AcceptanceCheck> acceptance;
};

struct EvalPlan {
    int schema = 1;
    std::string candidate_id;
    std::string content_hash;
    std::vector<EvalTask> replay;
    std::vector<EvalTask> holdout;
    std::string baseline_kind = "bare-agent";          // parent / bare-agent
    std::string baseline_ref = "default-agent";
    std::vector<std::string> baseline_metrics;
    std::string baseline_fixture;                      // 相对候选目录;空 = 没附
    std::int64_t budget_max_tool_calls = 0;            // 0 = 不设帽
    std::int64_t budget_max_tokens = 0;
    std::int64_t budget_timeout_ms = 0;
};

// 坏计划给错误文本(CI 要拿它说话),不做静默回落。
std::expected<EvalPlan, std::string> ParseEvalPlan(const std::string& text);

// ---------------------------------------------------------------------------
// 指标账(行 schema 1 的 metrics,七项全字段)
// ---------------------------------------------------------------------------

struct EvalMetrics {
    double success_rate = 0.0;
    double acceptance_rate = 0.0;
    std::int64_t tool_calls = 0;
    std::int64_t tokens = 0;
    std::int64_t wall_clock_ms = 0;
    std::int64_t permission_prompts = 0;
    std::int64_t workspace_writes = 0;

    nlohmann::json ToJson() const;
    static EvalMetrics FromJson(const nlohmann::json& json);
};

// ---------------------------------------------------------------------------
// 检查与扫描的逐项账(行 schema 的扩展字段 checks[]/findings[];必填字段
// 一概不动,扩展只增不改)
// ---------------------------------------------------------------------------

struct CheckResult {
    std::string kind;      // file_exists / json_parses / file_contains / command / manual / baseline-static / baseline-metrics
    std::string detail;    // 路径 / 命令 / 描述
    bool pass = false;
    bool skipped = false;  // manual 或夹具缺失:没测,不冒充失败也不冒充通过
    std::string note;      // 失败/跳过原因(人话)

    nlohmann::json ToJson() const;
    static std::optional<CheckResult> FromJson(const nlohmann::json& json);
};

struct ScanFinding {
    std::string kind;  // secret / absolute-path / malicious-script / dependency-poisoning /
                       // path-escape / network-overreach(阶段 6 四类见下)
    std::string path;  // 包内相对路径
    int line = 0;      // 1 起;判不出行给 0
    std::string detail;  // 命中的关键词/路径样子;不回显密钥原文

    nlohmann::json ToJson() const;
};

// ---------------------------------------------------------------------------
// eval-results.jsonl(行 schema 1,只追加)
// ---------------------------------------------------------------------------

// 复杂度代价(阶段 5):组合包比最小 Skill 包多出的组件数与维护面。
// 计法:components = 包内 skill + workflow + agent 件数;最小可行包 = 1 件
// (一份 Skill);files 按包内全部文件对"清单 + 一份 SKILL.md"多出的数。
// 评测账的静态门行带它,批准页照实亮——不是组件越多越容易晋升。
// 阶段 6 增补(只增不改):shape 另认 "code-draft",plugins/<id>/plugin.json
// 计一件组件(has_plugin),维护面含 runner 与依赖清单。
struct ComplexityCost {
    std::string shape;          // "combination" / "skill-only" / "code-draft"
    bool has_workflow = false;
    bool has_agent = false;
    bool has_plugin = false;    // 阶段 6:process Plugin 草稿一件
    int components = 0;         // skills + workflows + agents
    int minimal_components = 1; // 最小可行包:一份 Skill
    int extra_components = 0;   // components - minimal
    int files = 0;              // 包内全部文件(含 package.yaml)
    int minimal_files = 2;      // package.yaml + SKILL.md
    int extra_files = 0;        // files - minimal_files

    nlohmann::json ToJson() const;
    static std::optional<ComplexityCost> FromJson(const nlohmann::json& json);
    // 一行人话(批准页/判词用):组合包亮代价,Skill-only 亮"最小包"。
    std::string SummaryLine() const;
};

// 从候选 package/ 目录盘点复杂度代价(只读;目录读不动给 shape 空串)。
ComplexityCost ComputeComplexityCost(const std::filesystem::path& package_dir);

struct EvalResultLine {
    int schema = 1;
    std::int64_t seq = 0;          // 账内递增,由落账方(Coordinator)编
    std::string gate;              // static / replay / holdout / baseline
    std::string task_id;
    std::string candidate_id;
    std::string content_hash;
    std::string outcome;           // pass / fail / skipped
    EvalMetrics metrics;
    std::string baseline_ref;      // 仅 gate=baseline 必填
    std::vector<std::string> unverified;
    std::string verdict;           // 模型判词留位;确定性评测不填(权重低于测试与产物)
    std::string recorded_at;       // ISO 8601
    std::vector<CheckResult> checks;    // 扩展:逐项检查账
    std::vector<ScanFinding> findings;  // 扩展:静态门的密钥/绝对路径发现
    std::vector<std::string> notes;     // 扩展:预算越帽/夹具缺失一类的人话
    // 扩展(阶段 5):复杂度代价——组合包比最小 Skill 包多出的组件数与
    // 维护面。静态门行携带;批准页与 CI JSON 照实亮。
    std::optional<ComplexityCost> complexity;

    nlohmann::json ToJson() const;
    static std::optional<EvalResultLine> FromJson(const nlohmann::json& json);
};

std::string SerializeEvalResultLine(const EvalResultLine& line);
std::optional<EvalResultLine> ParseEvalResultLine(const std::string& text);

// 整读一份 eval-results.jsonl。坏行/半截行跳过,不废整账(恢复规矩同观察账)。
std::vector<EvalResultLine> LoadEvalResults(const std::filesystem::path& results_file);

// ---------------------------------------------------------------------------
// 静态门:Package doctor + 密钥扫描 + 绝对路径扫描(候选包全文,含 SKILL 正文)
// ---------------------------------------------------------------------------

struct StaticGateResult {
    bool doctor_valid = false;        // AnalyzePackage 的整包结论
    int diagnostics_errors = 0;       // 盘点诊断的 Error 级条数
    int diagnostics_warnings = 0;
    int components_total = 0;
    int components_ok = 0;
    std::vector<std::string> errors;  // doctor 的 error 级人话(诊断 + 坏件),至多 20 条
    std::vector<ScanFinding> findings;

    bool pass() const { return doctor_valid && diagnostics_errors == 0 && findings.empty(); }
};

StaticGateResult RunStaticGate(const std::filesystem::path& package_dir);

// 密钥扫描与绝对路径扫描的纯函数(单测直接喂文本)。secret 命中只报关键词
// 与行号,不回显值。
std::vector<ScanFinding> ScanTextForSecrets(const std::string& text);
std::vector<ScanFinding> ScanTextForAbsolutePaths(const std::string& text);
// 一个文件该不该按文本扫:头 512 字节里有 NUL 视为二进制,跳过。
bool LooksBinary(const std::string& text);

// ---------------------------------------------------------------------------
// 阶段 6 四类安全夹具(代码候选的静态门,发现即 error,与密钥/绝对路径
// 同一道门)。都是"生成草稿可以,带病草稿就地拦下"的保守扫描:
//   - 恶意脚本:毁盘/远程拉码执行/反弹 shell 一类的形状命中(注释里出现
//     也拦——草稿里不该有这些字样,人工审查线自会明辨);
//   - 依赖投毒:依赖清单(requirements*.txt / pyproject.toml / package.json)
//     里出现非注册表直链(git+/http/ftp/file)与改信任源的 pip 开关;
//   - 路径逃逸:路径段里的 ..(含 ${plugin_dir}/.. 形态)——草稿不许伸出包根;
//   - 网络越权:代码用了网络原语而清单未许,或布尔放行(network: true 的
//     宽授权)。前两类与路径逃逸是纯文本;网络越权要拿清单与代码对账,
//     走 ScanPackageNetworkOverreach(包级)。
// 夹具规矩:测试里的"恶意样张"只写无害的形状(注释/死串),绝不放真
// 密钥、真命令。
// ---------------------------------------------------------------------------
std::vector<ScanFinding> ScanTextForMaliciousScript(const std::string& text);
std::vector<ScanFinding> ScanTextForDependencyPoisoning(const std::string& rel_path,
                                                        const std::string& text);
std::vector<ScanFinding> ScanTextForPathEscape(const std::string& text);

// 代码文件里出现的网络原语(python/node/lua/shell 常见的取网姿势)。
// 纯文本;命中只说明"代码想用网",准不准要拿清单对账。
std::vector<ScanFinding> ScanCodeForNetworkUse(const std::string& text);

// 一只包的网络权限对账(包级,RunStaticGate 调):读包内全部 plugin.json /
// mcp.yaml 的网络声明,与包内代码文件的网络原语碰一碰——
//   代码用网 + 清单未许(或包内没有清单)   -> network-overreach(越权用网);
//   清单布尔放行(network: true 宽授权)   -> network-overreach(宽授权,
//     草稿须落精确声明,交人工审查);
//   代码带明文 http:// 取数               -> network-overreach(明文外联)。
// 返回的 finding 已带包内相对路径。
std::vector<ScanFinding> ScanPackageNetworkOverreach(const std::filesystem::path& package_dir);

// ---------------------------------------------------------------------------
// 一次任务门(replay/holdout)的确定性执行
// ---------------------------------------------------------------------------

struct TaskRunResult {
    EvalResultLine line;
    bool fixture_missing = false;  // workspace 给了却不在盘上:没测,不是测砸
};

// 评测 Workflow 与被测 Workflow 分家(阶段 5 钉死):任务门的"执行"只有
// 确定性检查器(file_exists/json_parses/file_contains/command),永远不起
// 候选包里的 workflow 自己跑——workflow 组件只做静态校验与来源回放的
// 夹具,免得自己给自己打分。acceptance 的 kind 白名单里没有(也不许有)
// "拿被测 workflow 跑一遍"这种 kind:认不得的 kind 整份计划拒解析。
TaskRunResult RunEvalTask(const std::string& gate, const EvalTask& task,
                          const std::string& candidate_id, const std::string& content_hash,
                          const std::filesystem::path& candidate_dir, const EvalPlan& plan);

// ---------------------------------------------------------------------------
// 基线夹具:对照的另一边(父版或裸 Agent 在同一夹具上的确定性指标账)
// ---------------------------------------------------------------------------

struct BaselineFixture {
    std::string kind;       // parent / bare-agent
    std::string ref;        // 父包引用 / 裸 Agent 名
    std::string task_id;    // 与候选侧哪条任务对照(可与候选行按 task 对账)
    EvalMetrics metrics;    // 七项指标(能记的记,判不了的来源侧自己写 unverified)
    std::vector<std::string> unverified;  // 基线侧没测到的
};

std::optional<BaselineFixture> ParseBaselineFixture(const std::string& text);

// ---------------------------------------------------------------------------
// 账面汇总:通过几项、没测什么、比基线贵多少
// ---------------------------------------------------------------------------

struct GateTally {
    int pass = 0;
    int fail = 0;
    int skipped = 0;
    int total() const { return pass + fail + skipped; }
};

struct MetricDelta {
    bool has_baseline = false;
    double candidate = 0.0;
    double baseline = 0.0;
    double delta = 0.0;   // candidate - baseline
    int delta_pct = 0;    // 基线非零时按百分比四舍五入;否则 0
};

struct EvalSummary {
    std::size_t line_count = 0;
    GateTally static_gate;
    GateTally replay;
    GateTally holdout;
    GateTally baseline;
    int checks_passed = 0;              // 可执行检查通过数(static 计 1 项)
    int checks_failed = 0;
    int checks_skipped = 0;             // manual / 夹具缺失
    std::vector<std::string> unverified;  // 并集,排序去重
    bool has_holdout = false;           // 无留出任务只可标 experimental
    std::string baseline_kind;
    std::string baseline_ref;
    bool has_baseline_metrics = false;
    MetricDelta success_rate;
    MetricDelta acceptance_rate;
    MetricDelta tool_calls;
    MetricDelta tokens;
    MetricDelta wall_clock_ms;
    MetricDelta permission_prompts;
    MetricDelta workspace_writes;
    std::optional<ComplexityCost> complexity;  // 阶段 5:最近一带账静态行的复杂度代价

    bool any_fail() const {
        return static_gate.fail > 0 || replay.fail > 0 || holdout.fail > 0 || baseline.fail > 0;
    }
    bool any_fixture_missing() const;
};

// 汇总一份账(可以是整账,也可以只是本次追加的行)。代价对照:候选侧取
// replay+holdout 行累计,基线侧取 baseline 行累计;比率取两侧非 skipped 行
// 的均值。账是只追加的,重跑评测数字会累进——要单轮账面,喂本次追加的行。
EvalSummary SummarizeEvalLedger(const std::vector<EvalResultLine>& lines);

// 确定性判词(阶段 3 的"独立 Evaluator"首版:结构化汇总,不接模型)。
// 三样必须说清:通过几项、没测什么、比基线贵多少。
std::string BuildDeterministicVerdict(const EvalSummary& summary);

// CI 退出码:全过 0;有 fail 1;夹具/计划缺失 2。
int EvalExitCode(const EvalSummary& summary, bool plan_loaded, bool fixture_missing_any);

}  // namespace lubancode::evolution
