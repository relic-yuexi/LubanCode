// LoCoMo-MC10 检索层评测 driver(记忆系统评测单 §E1)。不进 ctest,手动跑:
//   locomo_retrieval_driver <perturbed.jsonl> <out.json> [--keep]
//       [--extract <prompts_dir>] [--effort <档>] [--convs a,b]
//
// 每场对话:
//   1) 临时 home(<out 同级>/locomo_eval_home/<conv>/.lubancode)与临时
//      project 目录,ResolveProjectIdentity 走真四级裁决(首仓 manifest
//      原子写);
//   2) 逐 session 走正门 EnqueueSave 灌一条 fact 主题(title=日期+说话人,
//      summary=官方摘要截 500B,content=对话正文转写截 8KiB,keywords=
//      说话人名),RunPendingMemoryJobs 落盘——与生产写入同一只
//      ProcessJob(upsert 校验/指纹/catalog/index 重建全套);
//   3) 每题 BuildTurnContext(改写后 question, QueryOrigin::User)——
//      生产同款召回路(门槛/预算/去重),LastTrace() 读逐条排级账;
//   4) 逐题落 ranked id 序、injected 集合、注入字节、零命中与拦截计数。
//
// --extract <prompts_dir>(摘要质量双库对照,记忆写入侧改进单 §1.2):
//   灌库不走官方摘要,改走真抽取路——RunMemoryExtraction(cheap 路由,
//   LoadFromEnv 的真配置,USERPROFILE 指临时 home)按所给提示词目录产
//   候选,首条 fact 候选的 title/summary/content/keywords 进 EnqueueSave;
//   occurred_at 两库一律用 session datetime(材料自带,隔离"摘要质量"这
//   一个变量)。抽取失败重试一次,再失败落兜底主题(官方形状,计数进
//   extract.fallback,汇总侧如实报)。token 账按库汇总进 extract 段,
//   供"cheap 路由成本前后对照"。<prompts_dir> 指到覆盖目录(其下有
//   features/);传空串 = 用内嵌新提示词。
//
// Recall@k 的 evidence session 定位与五类分桶在 python 汇总侧
// (scripts/eval_locomo_retrieval_report.py)做——driver 只报事实,不掺判据。
//
// Options 用生产默认(max_results=3, max_retrieval_bytes=8KiB):量现状,
// 不为分数调参。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/model_router.hpp"  // TaskKind/BackgroundCallAccounting
#include "api/backend.hpp"
#include "app/backend_stack.hpp"  // BuildBackend
#include "app/memory_extract.hpp"
#include "app/model_router.hpp"  // ModelRouterService(真 cheap 路由)
#include "config/config.hpp"
#include "memory/project_memory.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace lubancode;

namespace {

constexpr std::size_t kMaxSummary = 500;   // ValidateSaveRequest 的摘要上限
constexpr std::size_t kMaxContent = 8192;  // 正文上限(8 KiB)

std::string TrimUtf8Safe(std::string s, std::size_t limit) {
    if (s.size() <= limit) return s;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

// 把字节位收到 UTF-8 字符边界上(截 title 头时用——劈开多字节字符会造出
// 坏 UTF-8,job.dump 见了要抛)。
std::size_t Utf8Boundary(const std::string& s, std::size_t cut) {
    if (cut >= s.size()) return s.size();
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return cut;
}

std::vector<std::string> SplitLines(const std::string& path) {    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

struct SessionInput {
    int no = 0;
    std::string datetime;
    std::string summary;
    std::vector<std::string> lines;  // 每条 "[NAME]: text"
};

struct QuestionInput {
    std::string qid;
    std::string category;
    std::string question;
};

// ---- --extract 模式:真抽取路的材料(摘要质量双库对照) ----

struct ExtractEngine {
    // LoadFromEnv 出的真配置 + 真 cheap 路由 + 独占 backend。config_result
    // 必须活着——router 拿的是它的引用。
    std::optional<config::ConfigResult> config_result;
    std::unique_ptr<api::Backend> main_backend;
    std::unique_ptr<app::ModelRouterService> router;
    app::ModelRouterService::DetachedRouted route;
    std::string prompts_dir;
    std::string effort_override;  // 空 = 用路由档位
    bool ok = false;
    std::string error;
};

struct ExtractUsage {
    int requests = 0;
    int failures = 0;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_reasoning_tokens = 0;
};

ExtractEngine MakeExtractEngine(const std::string& prompts_dir, const std::string& effort) {
    ExtractEngine engine;
    engine.prompts_dir = prompts_dir;
    engine.effort_override = effort;
    auto loaded = config::LoadFromEnv();
    if (!loaded.has_value()) {
        engine.error = "LoadFromEnv 失败: " + loaded.error();
        return engine;
    }
    engine.config_result = std::move(*loaded);
    engine.main_backend = app::BuildBackend(engine.config_result->config);
    const auto current_model = std::make_shared<std::string>(engine.config_result->config.model);
    engine.router = std::make_unique<app::ModelRouterService>(
        *engine.config_result, *engine.main_backend, current_model,
        engine.config_result->config.active_provider);
    engine.route = engine.router->RouteDetached(agent::TaskKind::MemoryExtract);
    if (engine.route.backend == nullptr) {
        engine.error = "cheap 路由找不到 provider: " + engine.route.route.provider;
        return engine;
    }
    engine.ok = true;
    return engine;
}

// 一场 session 的转写(单条正文,截 24KiB——与生产 BuildTurnTranscript 同
// 一量级;不造时间头,日期从材料侧给)。
std::string SessionTranscript(const std::vector<std::string>& lines, std::size_t max_bytes) {
    std::string out;
    for (const std::string& line : lines) {
        if (out.size() + line.size() + 1 > max_bytes) break;
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <perturbed.jsonl> <out.json> [--keep] [--extract <prompts_dir>] "
                     "[--effort <档>] [--convs a,b]\n",
                     argv[0]);
        return 2;
    }
    const std::string input_path = argv[1];
    const std::string out_path = argv[2];
    bool keep = false;
    bool extract_mode = false;
    std::string extract_prompts;   // --extract 的目录(空串=内嵌新提示词)
    std::string effort_override;   // 空 = 路由档位
    std::string convs_filter;      // 空 = 全部
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--keep") keep = true;
        else if (arg == "--extract" && i + 1 < argc) extract_mode = true, extract_prompts = argv[++i];
        else if (arg == "--extract") extract_mode = true;  // 值省略 = 内嵌
        else if (arg == "--effort" && i + 1 < argc) effort_override = argv[++i];
        else if (arg == "--convs" && i + 1 < argc) convs_filter = argv[++i];
    }
    std::vector<std::string> only_convs;
    if (!convs_filter.empty()) {
        std::stringstream ss(convs_filter);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) only_convs.push_back(item);
        }
    }

    ExtractEngine engine;
    if (extract_mode) {
        engine = MakeExtractEngine(extract_prompts, effort_override);
        if (!engine.ok) {
            std::fprintf(stderr, "--extract 起不来: %s\n", engine.error.c_str());
            return 1;
        }
        std::fprintf(stderr, "extract 路由: %s/%s (effort=%s, fell_back=%d) prompts=%s\n",
                     engine.route.route.provider.c_str(), engine.route.route.model.c_str(),
                     engine.effort_override.empty() ? engine.route.route.effort.c_str()
                                                    : engine.effort_override.c_str(),
                     engine.route.route.fell_back_to_normal ? 1 : 0,
                     engine.prompts_dir.empty() ? "<embedded>" : engine.prompts_dir.c_str());
    }
    ExtractUsage extract_usage;

    const fs::path work_root = fs::path(out_path).parent_path() / "locomo_eval_run";
    std::error_code ec;
    fs::remove_all(work_root, ec);

    json out = json::array();
    for (const std::string& line : SplitLines(input_path)) {
        const json conv = json::parse(line);
        const std::string cid = conv.at("conv_id").get<std::string>();
        if (!only_convs.empty() &&
            std::find(only_convs.begin(), only_convs.end(), cid) == only_convs.end()) {
            continue;
        }

        // 说话人名从第一条正文的前缀抓([NAME]:),keywords 用。
        std::vector<std::string> speakers;
        for (const auto& raw : conv.at("sessions")) {
            for (const auto& l : raw.at("lines")) {
                const std::string t = l.get<std::string>();
                const auto close = t.find("]:");
                const auto open = t.find('[');
                if (open == 0 && close != std::string::npos) {
                    std::string name = t.substr(1, close - 1);
                    std::string nice;
                    for (char c : name) {
                        if (nice.empty())
                            nice.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
                        else
                            nice.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
                    }
                    bool seen = false;
                    for (const auto& s : speakers)
                        if (s == nice) seen = true;
                    if (!seen) speakers.push_back(nice);
                }
            }
            if (speakers.size() >= 2) break;
        }

        // 临时 home 与 project 目录:每场独立 workspace,记忆互不沾。
        const fs::path home_luban = work_root / ("home-" + cid) / ".lubancode";
        const fs::path proj_dir = work_root / ("proj-" + cid);
        fs::create_directories(home_luban, ec);
        fs::create_directories(proj_dir, ec);

        auto identity = memory::ResolveProjectIdentity(proj_dir, home_luban);
        if (!identity.has_value()) {
            std::fprintf(stderr, "%s: 身份裁决失败: %s\n", cid.c_str(), identity.error().c_str());
            return 1;
        }
        memory::Options options;
        options.global_allowed = true;   // 评测语境:显式授权(写入+召回)
        options.enabled = true;
        options.use = true;
        options.learn = memory::LearnMode::Auto;          // EnqueueSave 需要非 Off
        options.learn_ceiling = memory::LearnMode::Auto;
        // max_results/max_retrieval_bytes 用生产默认(3/8KiB)。
        memory::ProjectMemory memory(std::move(*identity), home_luban, options);
        memory.set_source_session("locomo-eval-" + cid);

        // ---- 灌入:逐 session 一条 fact 主题,走正门 EnqueueSave ----
        std::size_t n_topics = 0;
        std::size_t extract_ok = 0;
        std::size_t extract_fallback = 0;
        std::size_t from_candidate = 0;
        std::size_t from_summary = 0;
        for (const auto& sess : conv.at("sessions")) {
            const int no = sess.at("no").get<int>();
            const std::string occurred = sess.at("datetime").get<std::string>().substr(0, 10);
            std::string content;
            for (const auto& l : sess.at("lines")) {
                content += l.get<std::string>();
                content += "\n";
                if (content.size() >= kMaxContent) break;
            }
            content = TrimUtf8Safe(content, kMaxContent);

            memory::SaveRequest req;
            req.id = "fact.locomo-" + cid + "-s" + std::to_string(no);
            req.kind = memory::MemoryKind::Fact;
            req.confidence = "verified";
            req.source_session = "locomo-eval-" + cid;
            // 时间线锚点:session datetime 是材料自带的时间,两库(旧/新抽取
            // prompt)一致,隔离"摘要质量"这一个变量。
            req.occurred_at = occurred;

            bool have_extracted = false;
            if (extract_mode) {
                // 真抽取路:cheap 路由发一枚,候选取首条 fact;失败重试一次。
                const std::vector<std::string> lines = [sess]() {
                    std::vector<std::string> out_lines;
                    for (const auto& l : sess.at("lines")) out_lines.push_back(l.get<std::string>());
                    return out_lines;
                }();
                const std::string transcript = SessionTranscript(lines, 24 * 1024);
                const std::string task_type = app::ClassifyTaskType(lines.empty() ? "" : lines.front(), {});
                const std::string system_prompt =
                    app::BuildExtractionSystemPrompt(engine.prompts_dir, task_type);
                const std::string effort = !engine.effort_override.empty() ? engine.effort_override
                                                                           : engine.route.route.effort;
                // 注意:std::expected 默认构造即有值,拿 has_value() 当循环闸
                // 会一进门就短路——这里用显式旗标。
                bool extracted = false;
                app::MemoryExtraction extraction;
                std::string extract_error;
                for (int attempt = 0; attempt < 2 && !extracted; ++attempt) {
                    agent::BackgroundCallAccounting accounting;
                    auto result = app::RunMemoryExtraction(*engine.route.backend,
                                                           engine.route.route.model, system_prompt,
                                                           transcript, /*timeout_secs=*/120, effort,
                                                           &accounting);
                    ++extract_usage.requests;
                    if (result.has_value()) {
                        extraction = std::move(*result);
                        extracted = true;
                    } else {
                        ++extract_usage.failures;
                        extract_error = result.error();
                    }
                    extract_usage.input_tokens += accounting.usage.input_tokens;
                    extract_usage.output_tokens += accounting.usage.output_tokens;
                    extract_usage.cache_read_tokens += accounting.usage.cache_read_tokens;
                    extract_usage.cache_creation_tokens += accounting.usage.cache_creation_tokens;
                    extract_usage.output_reasoning_tokens += accounting.usage.output_reasoning_tokens;
                    if (!extracted && attempt == 0) {
                        std::fprintf(stderr, "%s s%d: 抽取失败(%s),重试一次\n", cid.c_str(), no,
                                     extract_error.c_str());
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                }
                if (extracted) {
                    std::string kinds;
                    for (const auto& candidate : extraction.candidates) {
                        if (!kinds.empty()) kinds += ",";
                        kinds += candidate.kind;
                    }
                    std::fprintf(stderr, "%s s%d: 抽取 task=%s 候选[%s] 总结头: %.80s\n",
                                 cid.c_str(), no, extraction.task_type.c_str(), kinds.c_str(),
                                 extraction.summary.c_str());
                    // 映射一(生产正路):有 fact 候选就整条用候选。
                    for (const auto& candidate : extraction.candidates) {
                        if (candidate.kind != "fact" || candidate.title.empty() ||
                            candidate.content.empty()) {
                            continue;
                        }
                        req.title = TrimUtf8Safe(candidate.title, 200);
                        req.summary = TrimUtf8Safe(candidate.summary, kMaxSummary);
                        req.content = TrimUtf8Safe(candidate.content, kMaxContent);
                        if (!candidate.keywords.empty()) {
                            for (const std::string& keyword : candidate.keywords) {
                                if (req.keywords.size() >= 16) break;
                                req.keywords.push_back(keyword);
                            }
                        } else {
                            req.keywords = speakers;
                        }
                        have_extracted = true;
                        ++from_candidate;
                        break;
                    }
                    // 映射二(本材料的真产物):候选被"不收清单"正确拦空时,
                    // worker 真正抽出来的是回合 summary 与 retrieval_terms——
                    // title 由 summary 头部派生(确定性截断,两库同规则),
                    // content 一律用 session 转写,隔离出"摘要质量"单变量。
                    if (!have_extracted && !extraction.summary.empty()) {
                        const std::string summary = TrimUtf8Safe(extraction.summary, kMaxSummary);
                        std::size_t cut = 0;
                        for (const std::string_view stop : {"。", ";", "；", ":", "："}) {
                            const std::size_t hit = summary.find(stop);
                            if (hit != std::string::npos && hit > 3 && (cut == 0 || hit < cut)) {
                                cut = hit + stop.size();
                            }
                        }
                        if (cut == 0 || cut > 120) cut = summary.size() < 120 ? summary.size() : 120;
                        req.title = TrimUtf8Safe(summary.substr(0, Utf8Boundary(summary, cut)), 200);
                        req.summary = summary;
                        req.content = content;
                        for (const std::string& term : extraction.retrieval_terms) {
                            if (req.keywords.size() >= 16) break;
                            req.keywords.push_back(term);
                        }
                        if (req.keywords.empty()) req.keywords = speakers;
                        have_extracted = true;
                        ++from_summary;
                    }
                }
            }
            if (!have_extracted) {
                // 兜底(官方形状,无官方摘要——别把对照变量掺进来):
                // 抽取失败或没给 fact 候选时落转写本体,计数如实报。
                if (extract_mode) ++extract_fallback;
                req.title = TrimUtf8Safe("Chat session " + std::to_string(no) + " (" + occurred +
                                             ") " + (speakers.size() == 2
                                                         ? speakers[0] + " & " + speakers[1]
                                                         : std::string("chat")),
                                         200);
                req.summary.clear();
                req.content = content;
                req.keywords = speakers;
            } else {
                ++extract_ok;
            }
            auto saved = memory.EnqueueSave(req, /*user_initiated=*/true);
            if (!saved.has_value()) {
                std::fprintf(stderr, "%s: session %d 灌入失败: %s\n", cid.c_str(), no,
                             saved.error().c_str());
                return 1;
            }
            ++n_topics;
        }
        // worker 落盘(生产同款 job 处理)。
        auto flushed = memory::RunPendingMemoryJobs(home_luban);
        if (!flushed.has_value()) {
            std::fprintf(stderr, "%s: worker 失败: %s\n", cid.c_str(), flushed.error().c_str());
            return 1;
        }
        // 索引体积对账:catalog.json 落盘字节(content 进索引前后各记一笔,
        // 汇总侧出对照表)。
        std::error_code size_ec;
        const auto catalog_bytes =
            fs::file_size(memory.memory_dir() / ".state" / "catalog.json", size_ec);
        const std::size_t catalog_size = size_ec ? 0 : static_cast<std::size_t>(catalog_bytes);

        // ---- 逐题召回:BuildTurnContext + LastTrace ----
        json q_out = json::array();
        for (const auto& q : conv.at("questions")) {
            const std::string question = q.at("question").get<std::string>();
            const auto t0 = std::chrono::steady_clock::now();
            (void)memory.BuildTurnContext(question, proj_dir, memory::QueryOrigin::User);
            const auto t1 = std::chrono::steady_clock::now();
            const memory::RecallTrace trace = memory.LastTrace();
            json item;
            item["qid"] = q.at("qid");
            item["category"] = q.at("category");
            item["recall_us"] = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            json ranked = json::array();
            json injected = json::array();
            int below = 0, budget = 0, stale = 0, dup = 0, truncated = 0;
            for (const auto& e : trace.entries) {
                ranked.push_back(e.id);
                if (e.injected) injected.push_back(e.id);
                if (e.below_threshold) ++below;
                if (e.budget_dropped) ++budget;
                if (e.stale_blocked) ++stale;
                if (e.duplicate_dropped) ++dup;
                if (e.content_truncated) ++truncated;
            }
            item["ranked"] = ranked;
            item["injected"] = injected;
            item["injected_bytes"] = trace.injected_bytes;
            item["injected_count"] = trace.injected_count;
            item["below_threshold"] = below;
            item["budget_dropped"] = budget;
            item["stale_blocked"] = stale;
            item["duplicate_dropped"] = dup;
            item["content_truncated"] = truncated;
            item["zero_recall"] = trace.entries.empty();
            q_out.push_back(std::move(item));
        }

        json conv_result;
        conv_result["conv"] = cid;
        conv_result["n_topics"] = n_topics;
        conv_result["jobs_flushed"] = flushed.value();
        conv_result["catalog_bytes"] = catalog_size;
        if (extract_mode) {
            conv_result["extract"] = {{"ok", extract_ok},
                                      {"fallback", extract_fallback},
                                      {"from_candidate", from_candidate},
                                      {"from_summary", from_summary}};
        }
        conv_result["questions"] = std::move(q_out);
        out.push_back(std::move(conv_result));
        std::fprintf(stderr, "%s: topics=%zu questions=%zu\n", cid.c_str(), n_topics,
                     conv.at("questions").size());
    }

    std::ofstream of(out_path, std::ios::binary);
    of << out.dump(1) << "\n";
    if (!keep) fs::remove_all(work_root, ec);
    if (extract_mode) {
        json usage{{"requests", extract_usage.requests},
                   {"failures", extract_usage.failures},
                   {"input_tokens", extract_usage.input_tokens},
                   {"output_tokens", extract_usage.output_tokens},
                   {"cache_read_tokens", extract_usage.cache_read_tokens},
                   {"cache_creation_tokens", extract_usage.cache_creation_tokens},
                   {"output_reasoning_tokens", extract_usage.output_reasoning_tokens}};
        const fs::path usage_path = fs::path(out_path).replace_extension(".usage.json");
        std::ofstream uf(usage_path, std::ios::binary);
        uf << usage.dump(2) << "\n";
        std::fprintf(stderr, "extract usage: %s\n", usage.dump().c_str());
    }
    std::fprintf(stderr, "written: %s\n", out_path.c_str());
    return 0;
}
