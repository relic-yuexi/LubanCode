// 中文检索瘦身评测尺子(规格"记忆召回的中文检索瘦身与合成事件隔离"):
// 100 条中文项目记忆 + 127 条固定问句(88 应命中 / 39 不应命中),报召回率、
// 误命中率、注入字节 P50/P95。语料在 tests/fixtures/memory_retrieval/
// corpus_zh.json,改检索器前后各跑一遍,数字进 CI 防长歪。
//
// 与 test_memory_retrieval.cpp(中英混排尺子)的分工:那边钉标识符拆分与
// stale/scope/expired 拦截;这边专钉中文——全角标点不进词表、虚词不拿高分、
// 同内容重复对去重后注入字节下降。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "tests/fixtures"
#endif

struct FixtureQuery {
    std::string query;
    std::vector<std::string> expect;
    std::string cwd;  // 空 = 项目根
};

struct Fixture {
    std::vector<memory::MemoryEntry> entries;
    std::vector<FixtureQuery> positive;
    std::vector<FixtureQuery> negative;
};

memory::MemoryEntry ParseFixtureEntry(const nlohmann::json& item) {
    memory::MemoryEntry entry;
    entry.id = item.value("id", std::string());
    entry.kind = item.value("kind", std::string()) == "preference" ? memory::MemoryKind::Preference
                                                                   : memory::MemoryKind::Fact;
    entry.title = item.value("title", std::string());
    entry.summary = item.value("summary", std::string());
    entry.status = item.value("status", std::string("active"));
    entry.confidence = item.value("confidence", std::string("verified"));
    entry.updated_at = "2026-01-01T00:00:00Z";
    entry.last_verified_at = entry.updated_at;
    if (item.contains("keywords") && item["keywords"].is_array()) {
        for (const auto& keyword : item["keywords"]) {
            if (keyword.is_string()) entry.keywords.push_back(keyword.get<std::string>());
        }
    }
    if (item.contains("paths") && item["paths"].is_array()) {
        for (const auto& path : item["paths"]) {
            if (path.is_string()) entry.paths.push_back(path.get<std::string>());
        }
    }
    if (item.contains("scope") && item["scope"].is_object()) {
        entry.scope.kind = item["scope"].value("kind", std::string("project"));
        entry.scope.value = item["scope"].value("value", std::string());
    }
    if (item.contains("expires_at") && item["expires_at"].is_string()) {
        entry.expires_at = item["expires_at"].get<std::string>();
    }
    return entry;
}

Fixture LoadFixture() {
    const fs::path fixture_path = fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "memory_retrieval" / "corpus_zh.json";
    std::ifstream file(fixture_path);
    REQUIRE(file.is_open());
    nlohmann::json root;
    file >> root;
    REQUIRE(root.is_object());

    Fixture fixture;
    for (const auto& item : root["entries"]) fixture.entries.push_back(ParseFixtureEntry(item));
    for (const auto& item : root["queries"]) {
        FixtureQuery query;
        query.query = item.value("query", std::string());
        query.cwd = item.value("cwd", std::string());
        if (item.contains("expect") && item["expect"].is_array()) {
            for (const auto& expect : item["expect"]) {
                if (expect.is_string()) query.expect.push_back(expect.get<std::string>());
            }
        }
        if (query.expect.empty()) {
            fixture.negative.push_back(std::move(query));
        } else {
            fixture.positive.push_back(std::move(query));
        }
    }
    return fixture;
}

// RankEntries 语义下"会注入"的候选:过门槛、未拦(expired/scope)。与正式
// BuildTurnContext 的差异是预算/去重在后面那层,这里先量排级本身。
std::vector<std::string> RankedInjectIds(const std::vector<memory::MemoryEntry>& entries,
                                         const FixtureQuery& query) {
    const auto ranked = memory::RankEntries(entries, query.query, query.cwd);
    std::vector<std::string> ids;
    for (const auto& hit : ranked) {
        if (ids.size() >= 3) break;
        if (hit.expired || hit.scope_blocked) continue;
        if (!hit.qualifies) continue;
        ids.push_back(hit.entry->id);
    }
    return ids;
}

double Percentile(std::vector<std::size_t> samples, int percent) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t index = samples.size() >= 100 ? samples.size() * percent / 100 - 1
                                                    : samples.size() * percent / 100;
    return static_cast<double>(samples[std::min(index, samples.size() - 1)]);
}

// 从 BuildTurnContext 产出的上下文里量"召回段"字节:## 召回: 起到下一段。
std::size_t RecallBlockBytes(const std::string& context) {
    std::size_t total = 0;
    std::size_t hits = 0;
    for (std::size_t pos = context.find("## 召回: ", 0); pos != std::string::npos;
         pos = context.find("## 召回: ", pos + 1)) {
        const std::size_t next = context.find("\n## 召回: ", pos + 1);
        const std::size_t end = next == std::string::npos ? context.size() : next + 1;
        total += end - pos;
        ++hits;
    }
    return hits == 0 ? 0 : total;
}

}  // namespace

TEST_CASE("中文检索评测:100 条固定语料的召回率与误命中率") {
    const Fixture fixture = LoadFixture();
    REQUIRE(fixture.entries.size() == 100);
    REQUIRE(fixture.positive.size() >= 80);
    REQUIRE(fixture.negative.size() >= 30);

    std::size_t recall1 = 0;
    std::size_t recall3 = 0;
    std::size_t precision3_total = 0;
    std::size_t precision3_hit = 0;
    for (const FixtureQuery& query : fixture.positive) {
        const std::vector<std::string> injected = RankedInjectIds(fixture.entries, query);
        if (!injected.empty() &&
            std::find(injected.begin(), injected.end(), query.expect[0]) != injected.end()) {
            ++recall1;
        }
        std::size_t matched = 0;
        for (const std::string& id : injected) {
            if (std::find(query.expect.begin(), query.expect.end(), id) != query.expect.end()) ++matched;
        }
        if (matched >= 1) ++recall3;
        precision3_total += injected.size();
        precision3_hit += matched;
        const bool top_miss = injected.empty() ||
                              std::find(injected.begin(), injected.end(), query.expect[0]) == injected.end();
        if (injected.empty() || matched < injected.size()) {
            std::cout << "[memory-zh-eval] " << (top_miss ? "MISS" : "over") << ": \"" << query.query
                      << "\" got:";
            for (const std::string& id : injected) std::cout << " " << id;
            std::cout << " want:";
            for (const std::string& id : query.expect) std::cout << " " << id;
            std::cout << "\n";
        }
    }

    std::size_t false_hits = 0;
    for (const FixtureQuery& query : fixture.negative) {
        const std::vector<std::string> injected = RankedInjectIds(fixture.entries, query);
        if (!injected.empty()) {
            ++false_hits;
            std::cout << "[memory-zh-eval] false-positive: \"" << query.query << "\" ->";
            for (const std::string& id : injected) std::cout << " " << id;
            std::cout << "\n";
        }
    }

    const double recall_at_1 = static_cast<double>(recall1) / fixture.positive.size();
    const double recall_at_3 = static_cast<double>(recall3) / fixture.positive.size();
    const double precision_at_3 =
        precision3_total == 0 ? 1.0 : static_cast<double>(precision3_hit) / precision3_total;
    const double false_hit_rate = static_cast<double>(false_hits) / fixture.negative.size();

    std::cout << "[memory-zh-eval] entries=" << fixture.entries.size()
              << " positive=" << fixture.positive.size() << " negative=" << fixture.negative.size() << "\n";
    std::cout << "[memory-zh-eval] Recall@1=" << recall_at_1 << " Recall@3=" << recall_at_3
              << " Precision@3=" << precision_at_3 << " false-hit-rate=" << false_hit_rate << "\n";

    // CI 防线:优化前基线 Recall@1/@3=1.00、Precision@3=0.81、误命中率
    // 5.1%(2/39)。检索瘦身只许把误命中率与精度做上去,召回不许掉。
    CHECK(recall_at_1 >= 0.95);
    CHECK(recall_at_3 >= 0.95);
    CHECK(precision_at_3 >= 0.78);
    CHECK(false_hit_rate <= 0.08);
}

TEST_CASE("中文检索评测:端到端注入字节 P50/P95 与零命中不塞脚手架") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-memory-zh-" +
                                                       std::to_string(std::chrono::high_resolution_clock::now()
                                                                          .time_since_epoch()
                                                                          .count()));
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    const fs::path home = root / "home";
    // 问法里的 cwd 要真实存在,relative() 才算得出前缀。
    fs::create_directories(repo / "web");

    const Fixture fixture = LoadFixture();
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(*identity, home, options);

    // 100 条全部入库(走真 worker 路径);paths 指到的文件造出来,指纹才有得算。
    for (const auto& item : fixture.entries) {
        memory::SaveRequest request;
        request.kind = item.kind;
        request.id = item.id;
        request.title = item.title;
        request.summary = item.summary;
        // 正文由标题派生:同标题的重复对派生出同正文,去重路径才有得测。
        request.content = "正文:" + item.title + "。中文评测固定正文,占一段字节数。";
        request.keywords = item.keywords;
        request.paths = item.paths;
        request.scope = item.scope;
        request.expires_at = item.expires_at;
        for (const std::string& path : item.paths) {
            const fs::path target = repo / fs::path(path);
            fs::create_directories(target.parent_path(), ec);
            if (!fs::exists(target, ec)) {
                std::ofstream out(target, std::ios::binary);
                out << "fixture source\n";
            }
        }
        REQUIRE(store.EnqueueSave(request).has_value());
    }
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
    REQUIRE(store.ListEntries().size() == 100);

    // 全部问句各跑一遍,量每问注入字节;零命中问句只记 suffix 总字节。
    std::vector<std::size_t> byte_samples;
    std::size_t zero_hit_suffix_bytes = 0;
    std::size_t zero_hit_count = 0;
    for (const auto* list : {&fixture.positive, &fixture.negative}) {
        for (const FixtureQuery& query : *list) {
            const std::string context = store.BuildTurnContext(query.query, repo / fs::path(query.cwd));
            const std::size_t bytes = RecallBlockBytes(context);
            if (bytes == 0) {
                zero_hit_suffix_bytes = std::max(zero_hit_suffix_bytes, context.size());
                ++zero_hit_count;
            } else {
                byte_samples.push_back(bytes);
            }
        }
    }

    const double p50 = Percentile(byte_samples, 50);
    const double p95 = Percentile(byte_samples, 95);
    const double average = [&] {
        double sum = 0.0;
        for (const std::size_t sample : byte_samples) sum += static_cast<double>(sample);
        return byte_samples.empty() ? 0.0 : sum / static_cast<double>(byte_samples.size());
    }();
    std::cout << "[memory-zh-eval] injected-queries=" << byte_samples.size() << " zero-hit-queries="
              << zero_hit_count << "\n";
    std::cout << "[memory-zh-eval] avg-injected-bytes=" << average << " P50=" << p50 << " P95=" << p95
              << " zero-hit-suffix-bytes=" << zero_hit_suffix_bytes << "\n";

    CHECK(!byte_samples.empty());
    CHECK(p50 <= 2.5 * 1024);
    CHECK(p95 <= 8.0 * 1024 + 256);
    // 零命中不塞空脚手架:suffix 不进任何主题正文,也不进整段使用说明。
    CHECK(zero_hit_suffix_bytes < 1024);

    fs::remove_all(root, ec);
}
