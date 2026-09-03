// LoCoMo-MC10 检索层评测 driver(记忆系统评测单 §E1)。不进 ctest,手动跑:
//   locomo_retrieval_driver <perturbed.jsonl> <out.json> [--keep]
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
// Recall@k 的 evidence session 定位与五类分桶在 python 汇总侧
// (scripts/eval_locomo_retrieval_report.py)做——driver 只报事实,不掺判据。
//
// Options 用生产默认(max_results=3, max_retrieval_bytes=8KiB):量现状,
// 不为分数调参。

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

std::vector<std::string> SplitLines(const std::string& path) {
    std::vector<std::string> lines;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <perturbed.jsonl> <out.json> [--keep]\n", argv[0]);
        return 2;
    }
    const std::string input_path = argv[1];
    const std::string out_path = argv[2];
    const bool keep = argc > 3 && std::string(argv[3]) == "--keep";

    const fs::path work_root = fs::path(out_path).parent_path() / "locomo_eval_run";
    std::error_code ec;
    fs::remove_all(work_root, ec);

    json out = json::array();
    for (const std::string& line : SplitLines(input_path)) {
        const json conv = json::parse(line);
        const std::string cid = conv.at("conv_id").get<std::string>();

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
        for (const auto& sess : conv.at("sessions")) {
            memory::SaveRequest req;
            req.id = "fact.locomo-" + cid + "-s" + std::to_string(sess.at("no").get<int>());
            req.kind = memory::MemoryKind::Fact;
            req.title = TrimUtf8Safe("Chat session " + std::to_string(sess.at("no").get<int>()) +
                                         " (" + sess.at("datetime").get<std::string>().substr(0, 10) +
                                         ") " + (speakers.size() == 2 ? speakers[0] + " & " + speakers[1]
                                                                     : std::string("chat")),
                                     200);
            req.summary = TrimUtf8Safe(sess.at("summary").get<std::string>(), kMaxSummary);
            std::string content;
            for (const auto& l : sess.at("lines")) {
                content += l.get<std::string>();
                content += "\n";
                if (content.size() >= kMaxContent) break;
            }
            req.content = TrimUtf8Safe(content, kMaxContent);
            req.keywords = speakers;
            req.confidence = "verified";
            req.source_session = "locomo-eval-" + cid;
            auto saved = memory.EnqueueSave(req, /*user_initiated=*/true);
            if (!saved.has_value()) {
                std::fprintf(stderr, "%s: session %d 灌入失败: %s\n", cid.c_str(),
                             sess.at("no").get<int>(), saved.error().c_str());
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
        conv_result["questions"] = std::move(q_out);
        out.push_back(std::move(conv_result));
        std::fprintf(stderr, "%s: topics=%zu questions=%zu\n", cid.c_str(), n_topics,
                     conv.at("questions").size());
    }

    std::ofstream of(out_path, std::ios::binary);
    of << out.dump(1) << "\n";
    if (!keep) fs::remove_all(work_root, ec);
    std::fprintf(stderr, "written: %s\n", out_path.c_str());
    return 0;
}
