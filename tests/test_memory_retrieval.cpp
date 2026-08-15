// 检索评测尺子(规格"评测尺子"节):固定语料 + 固定问法,每次改检索器
// 都跑一遍,报 Recall/Precision/注入字节/stale 拦截率,数字进 CI 防长歪。
// 语料与问法在 tests/fixtures/memory_retrieval/corpus.json,中英混写、符号
// 名、路径、camelCase/snake_case、同词不同模块干扰、cwd scope 越区、
// expired/archived/conflict 全都铺到。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
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
    std::vector<memory::MemoryEntry> entries;      // 进检索器的正式条目
    std::vector<memory::MemoryEntry> rank_only;    // archived/conflict 等
    std::vector<FixtureQuery> positive;
    std::vector<FixtureQuery> negative;
};

std::string TrimSpace(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\n')) ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\n')) --end;
    return value.substr(begin, end - begin);
}

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
    const fs::path fixture_path = fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "memory_retrieval" / "corpus.json";
    std::ifstream file(fixture_path);
    REQUIRE(file.is_open());
    nlohmann::json root;
    file >> root;
    REQUIRE(root.is_object());

    Fixture fixture;
    for (const auto& item : root["entries"]) {
        fixture.entries.push_back(ParseFixtureEntry(item));
    }
    if (root.contains("rank_only_entries")) {
        for (const auto& item : root["rank_only_entries"]) {
            fixture.rank_only.push_back(ParseFixtureEntry(item));
        }
    }
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

// RankEntries 语义下"会注入"的候选:过门槛、未拦(expired/scope)。
std::vector<std::string> InjectedIds(const std::vector<memory::MemoryEntry>& entries,
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

double Percentile95(std::vector<std::size_t> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t index = samples.size() >= 20 ? samples.size() * 95 / 100 - 1 : samples.size() - 1;
    return static_cast<double>(samples[index]);
}

}  // namespace

TEST_CASE("检索评测:标识符拆分与中英混排分词") {
    const auto tokens = memory::TokenizeForRetrieval("BuildTurnContext 住在 src/agent/loop.cpp,RankEntries 用 BM25 打分");
    const auto has = [&tokens](const std::string& term) {
        return std::find(tokens.begin(), tokens.end(), term) != tokens.end();
    };
    CHECK(has("build"));
    CHECK(has("turn"));
    CHECK(has("context"));
    CHECK(has("buildturncontext"));  // 整串也保留,精确匹配完整标识符
    CHECK(has("src"));
    CHECK(has("agent"));
    CHECK(has("loop"));
    CHECK(has("cpp"));
    CHECK(has("bm25"));
    CHECK(has("打分"));  // 中文双字片段
    CHECK_FALSE(has("b"));  // 单字符不出词
}

TEST_CASE("检索评测:归一化与中文词+二元双路——标点不黏词,虚词降权") {
    memory::MemoryEntry entry;
    entry.id = "fact.release-codename";
    entry.title = "发布代号";
    entry.summary = "本项目发布代号是青瓷-47,对外文案统一用它";
    entry.keywords = {"发布代号", "青瓷-47", "代号"};
    entry.updated_at = "2026-01-01T00:00:00Z";
    entry.last_verified_at = entry.updated_at;

    std::vector<memory::TraceTerm> terms;
    const auto ranked = memory::RankEntries(
        {entry}, "这个项目的发布代号是什么？只回复代号，不要调用工具。", "", {}, &terms);
    REQUIRE_FALSE(terms.empty());
    REQUIRE_FALSE(ranked.empty());

    const auto find_term = [&terms](const std::string& text) {
        return std::find_if(terms.begin(), terms.end(),
                            [&text](const memory::TraceTerm& term) { return term.text == text; });
    };
    // 词典整词保住:关键词/标题里的"发布代号"按整词进词表,满权。
    const auto codename = find_term("发布代号");
    REQUIRE(codename != terms.end());
    CHECK(codename->kind == "word");
    CHECK(codename->weight == 1.0);
    CHECK(codename->source == "query");
    // 标点不进词表:全角问号/逗号/句号以及任何半角标点都不许黏进 term。
    for (const memory::TraceTerm& term : terms) {
        CHECK(term.text.find("？") == std::string::npos);
        CHECK(term.text.find("，") == std::string::npos);
        CHECK(term.text.find("。") == std::string::npos);
        CHECK(term.text.find('?') == std::string::npos);
        CHECK(term.text.find(',') == std::string::npos);
    }
    // 虚词碎片降权:"什么"这类句式片段拿低权重,凑不了门槛。
    const auto what = find_term("什么");
    REQUIRE(what != terms.end());
    CHECK(what->kind == "gram");
    CHECK(what->weight <= 0.25);
    // "个项"这类跨虚词的句式碎片同样低权重。
    const auto fragment = find_term("个项");
    if (fragment != terms.end()) CHECK(fragment->weight <= 0.25);

    // 全半角归一:全角字母数字的关键词照常硬命中。
    memory::MemoryEntry bm;
    bm.id = "fact.bm25";
    bm.title = "BM25 软分";
    bm.keywords = {"BM25"};
    bm.updated_at = entry.updated_at;
    bm.last_verified_at = entry.updated_at;
    const auto fullwidth = memory::RankEntries({bm}, "ＢＭ２５ 打分在哪", "", {});
    REQUIRE_FALSE(fullwidth.empty());
    CHECK(fullwidth[0].hard_hits >= 1);

    // 路径分隔符归一:反斜杠路径与正斜杠路径互相认。
    memory::MemoryEntry path_entry;
    path_entry.id = "fact.loop-path";
    path_entry.title = "主循环文件";
    path_entry.paths = {"src/agent/loop.cpp"};
    path_entry.updated_at = entry.updated_at;
    path_entry.last_verified_at = entry.updated_at;
    const auto backslash = memory::RankEntries({path_entry}, "src\\agent\\loop.cpp 在哪定义", "", {});
    REQUIRE_FALSE(backslash.empty());
    CHECK(backslash[0].hard_hits >= 1);

    // 普通二元片段凑不成硬命中:纯句式问法在带关键词的条目上硬命中为零。
    const auto soft_only = memory::RankEntries({entry}, "那接下来该怎么办呢", "", {});
    for (const auto& hit : soft_only) {
        CHECK(hit.hard_hits == 0);
        CHECK_FALSE(hit.qualifies);
    }
}

TEST_CASE("检索评测:archived/conflict 不参与,expired 与 scope 越区不注入") {
    const Fixture fixture = LoadFixture();
    std::vector<memory::MemoryEntry> all = fixture.entries;
    all.insert(all.end(), fixture.rank_only.begin(), fixture.rank_only.end());

    // archived/conflict:直接从排级里消失。
    const auto legacy = memory::RankEntries(all, "legacy old entry 归档条目", "");
    for (const auto& hit : legacy) {
        CHECK(hit.entry->id != "fact.archived-old-entry");
        CHECK(hit.entry->id != "fact.conflict-entry");
    }
    const auto conflict = memory::RankEntries(all, "conflict merge 冲突条目", "");
    for (const auto& hit : conflict) {
        CHECK(hit.entry->id != "fact.conflict-entry");
    }

    // expired:临时分支关键词双硬命中,但过期了,不准注入。
    const auto expired = memory::RankEntries(all, "临时分支 前缀", "");
    bool found_expired = false;
    for (const auto& hit : expired) {
        if (hit.entry->id == "fact.temp-branch-policy") {
            found_expired = true;
            CHECK(hit.expired);
        }
    }
    CHECK(found_expired);

    // scope 越区:关键词硬命中但 cwd 不在 subtree 内,标记 scope_blocked。
    const auto blocked = memory::RankEntries(all, "vite build 入口", "");
    bool found_blocked = false;
    for (const auto& hit : blocked) {
        if (hit.entry->id == "fact.frontend-build") {
            found_blocked = true;
            CHECK(hit.scope_blocked);
        }
    }
    CHECK(found_blocked);
    // 同一问法落在 web 子树内就不拦。
    const auto inside = memory::RankEntries(all, "vite build 入口", "web");
    bool found_inside = false;
    for (const auto& hit : inside) {
        if (hit.entry->id == "fact.frontend-build") {
            found_inside = true;
            CHECK_FALSE(hit.scope_blocked);
        }
    }
    CHECK(found_inside);
}

TEST_CASE("检索评测:固定语料 Recall/Precision 与空召回正确率") {
    const Fixture fixture = LoadFixture();
    REQUIRE(fixture.entries.size() >= 30);
    REQUIRE(fixture.positive.size() >= 30);
    REQUIRE(fixture.negative.size() >= 30);

    std::size_t recall1 = 0;
    std::size_t recall3 = 0;
    std::size_t precision1_total = 0;
    std::size_t precision1_hit = 0;
    std::size_t precision3_total = 0;
    std::size_t precision3_hit = 0;
    for (const FixtureQuery& query : fixture.positive) {
        const std::vector<std::string> injected = InjectedIds(fixture.entries, query);
        if (!injected.empty() && std::find(injected.begin(), injected.end(), query.expect[0]) != injected.end()) {
            ++recall1;
        }
        std::size_t matched = 0;
        for (const std::string& id : injected) {
            if (std::find(query.expect.begin(), query.expect.end(), id) != query.expect.end()) ++matched;
        }
        if (matched >= 1) ++recall3;
        if (!injected.empty()) {
            ++precision1_total;
            if (std::find(query.expect.begin(), query.expect.end(), injected[0]) != query.expect.end()) {
                ++precision1_hit;
            }
        }
        precision3_total += injected.size();
        precision3_hit += matched;
        if (matched < injected.size()) {
            std::cout << "[memory-retrieval-eval] over-injected: \"" << query.query << "\" ->";
            for (const std::string& id : injected) {
                if (std::find(query.expect.begin(), query.expect.end(), id) == query.expect.end()) {
                    std::cout << " " << id;
                }
            }
            std::cout << "\n";
        }
    }

    std::size_t empty_correct = 0;
    for (const FixtureQuery& query : fixture.negative) {
        const std::vector<std::string> injected = InjectedIds(fixture.entries, query);
        if (injected.empty()) {
            ++empty_correct;
            continue;
        }
        std::cout << "[memory-retrieval-eval] false-positive: \"" << query.query << "\" ->";
        for (const std::string& id : injected) std::cout << " " << id;
        std::cout << "\n";
    }

    const double recall_at_1 = static_cast<double>(recall1) / fixture.positive.size();
    const double recall_at_3 = static_cast<double>(recall3) / fixture.positive.size();
    const double precision_at_1 =
        precision1_total == 0 ? 1.0 : static_cast<double>(precision1_hit) / precision1_total;
    const double precision_at_3 =
        precision3_total == 0 ? 1.0 : static_cast<double>(precision3_hit) / precision3_total;
    const double empty_rate = static_cast<double>(empty_correct) / fixture.negative.size();

    std::cout << "[memory-retrieval-eval] entries=" << fixture.entries.size()
              << " positive=" << fixture.positive.size() << " negative=" << fixture.negative.size() << "\n";
    std::cout << "[memory-retrieval-eval] Recall@1=" << recall_at_1 << " Recall@3=" << recall_at_3 << "\n";
    std::cout << "[memory-retrieval-eval] Precision@1=" << precision_at_1 << " Precision@3=" << precision_at_3
              << "\n";
    std::cout << "[memory-retrieval-eval] empty-recall-accuracy=" << empty_rate << "\n";

    // CI 防线:改检索器不许把这些数字做低。
    CHECK(recall_at_3 >= 0.90);
    CHECK(recall_at_1 >= 0.80);
    CHECK(precision_at_3 >= 0.90);
    CHECK(precision_at_1 >= 0.85);
    CHECK(empty_rate >= 0.95);
}

TEST_CASE("检索评测:端到端注入字节、stale 拦截与 expired 不进 prompt") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-memory-eval-" +
                                                       std::to_string(std::chrono::high_resolution_clock::now()
                                                                          .time_since_epoch()
                                                                          .count()));
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    const fs::path home = root / "home";
    // 问法里的 cwd 都要真实存在,relative() 才算得出前缀。
    fs::create_directories(repo / "web" / "src");
    fs::create_directories(repo / "src" / "api");

    const Fixture fixture = LoadFixture();
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(*identity, home, options);

    // 正式条目全部入库(走真 worker 路径);paths 指到的文件造出来,指纹
    // 才有得算。stale 用例的文件之后再改。
    for (const auto& item : fixture.entries) {
        memory::SaveRequest request;
        request.kind = item.kind;
        request.id = item.id;
        request.title = item.title;
        request.summary = item.summary;
        request.content = "正文:" + item.title + "。检索评测固定正文,占一段字节数。";
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
    REQUIRE(store.ListEntries().size() == fixture.entries.size());

    // 注入字节:正负问法各跑一遍,cwd 按问法指定。
    std::vector<std::size_t> byte_samples;
    std::size_t no_hit_bytes = 0;
    for (const auto* list : {&fixture.positive, &fixture.negative}) {
        for (const FixtureQuery& query : *list) {
            const std::string context = store.BuildTurnContext(query.query, repo / fs::path(query.cwd));
            // 注入字节按召回段计,不算固定能力说明头。
            std::size_t injected_bytes = 0;
            std::size_t hits = 0;
            for (std::size_t pos = context.find("## 召回: ", 0); pos != std::string::npos;
                 pos = context.find("## 召回: ", pos + 1)) {
                const std::size_t next = context.find("\n## 召回: ", pos + 1);
                const std::size_t end = next == std::string::npos ? context.size() : next + 1;
                injected_bytes += end - pos;
                ++hits;
            }
            if (hits == 0) {
                no_hit_bytes = context.size();
            } else {
                byte_samples.push_back(injected_bytes);
            }
        }
    }

    // 无命中问法的 suffix 是零——不塞空脚手架,更不含索引正文。
    CHECK(no_hit_bytes == 0);

    double average = 0.0;
    for (const std::size_t sample : byte_samples) average += static_cast<double>(sample);
    average /= byte_samples.empty() ? 1 : static_cast<double>(byte_samples.size());
    const double p95 = Percentile95(byte_samples);
    std::cout << "[memory-retrieval-eval] avg-injected-bytes=" << average << " p95-injected-bytes=" << p95
              << "\n";
    CHECK(average <= 8.0 * 1024);
    CHECK(p95 <= 8.0 * 1024 + 256);  // 标题行/来源行是少量开销

    // expired 不进 prompt。
    const std::string expired_context = store.BuildTurnContext("临时分支 前缀", repo);
    CHECK(expired_context.find("fact.temp-branch-policy") == std::string::npos);

    // stale:改掉 entry 1 的关联文件,强命中也不注正文,只提示核验。
    const fs::path loop_file = repo / "src" / "agent" / "loop.cpp";
    {
        std::ofstream out(loop_file, std::ios::binary | std::ios::app);
        out << "// drifted\n";
    }
    const std::string stale_context = store.BuildTurnContext("AgentLoop::Run 在哪里定义", repo);
    CHECK(stale_context.find("相关文件已变化") != std::string::npos);
    CHECK(stale_context.find("检索评测固定正文") == std::string::npos);

    // stale 清单看得见,verify 之后原 id 复活。(loop.cpp 挂在两条主题上,
    // 漂移后两条都进清单;过期条目也在清单里。只核验其中一条。)
    const auto stale = store.ListStaleEntries();
    REQUIRE(stale.size() == 3);  // 两条 loop.cpp 指纹漂移 + 一条已过期
    bool listed = false;
    for (const auto& item : stale) {
        if (item.entry.id == "fact.agent-loop-request-flow") {
            listed = true;
            CHECK(item.reason == "fingerprint");
        }
        if (item.entry.id == "fact.temp-branch-policy") {
            CHECK(item.reason == "expired");
        }
    }
    CHECK(listed);
    REQUIRE(store.EnqueueVerify("fact.agent-loop-request-flow", /*refresh=*/false).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
    REQUIRE(store.ListStaleEntries().size() == 2);
    const std::string revived = store.BuildTurnContext("AgentLoop::Run 在哪里定义", repo);
    CHECK(revived.find("fact.agent-loop-request-flow") != std::string::npos);
    CHECK(revived.find("检索评测固定正文") != std::string::npos);

    fs::remove_all(root, ec);
}
