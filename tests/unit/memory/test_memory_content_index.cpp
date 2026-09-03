// content 进索引与幻觉护栏的单测(LoCoMo 改进单《记忆检索与注入改进》):
//   1) 正文词进检索——纯正文命中的条目排得上、过得了门槛(content 现切与
//      content_index 词袋两条路同一结局);
//   2) catalog 正门 roundtrip——EnqueueSave→worker→BuildTurnContext,词袋
//      落盘读回,注入载荷带摘要行与正文相关段;
//   3) 护栏话术——有召回时头尾都有"线索不足就说不知道";沾边但不含答案
//      的候选只作线索注入,低分候选整条拦下;零命中不塞脚手架;
//   4) 预算选条规则——按分装填、同分按 topic id 稳定排序、单条截断保
//      summary 完整、预算丢弃带理由不静默。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-memory-content-" + std::to_string(run_id) + "-" + name + "-" +
                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

memory::MemoryEntry MakeEntry(const std::string& id, const std::string& title,
                              const std::string& summary, const std::string& content) {
    memory::MemoryEntry entry;
    entry.id = id;
    entry.title = title;
    entry.summary = summary;
    entry.content = content;
    entry.confidence = "verified";
    entry.updated_at = "2026-01-01T00:00:00Z";
    entry.last_verified_at = entry.updated_at;
    return entry;
}

// 干扰条目:语料垫背,把 idf 抬到真库的量级,词面与目标条目不沾。
std::vector<memory::MemoryEntry> FillerEntries(std::size_t count) {
    std::vector<memory::MemoryEntry> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(MakeEntry("fact.filler-" + std::to_string(i),
                                "例行事项 " + std::to_string(i),
                                "例会纪要第 " + std::to_string(i) + " 回",
                                "例会纪要:第 " + std::to_string(i) +
                                    " 回讨论了例程事项,与会者确认既有安排不变,散会。"));
    }
    return out;
}

}  // namespace

TEST_CASE("content 进索引:纯正文命中的条目过门槛且排第一") {
    const std::string content =
        "压测记录:TrajeanHub 走 QuartzRelay 通道,nightly-773 批次全绿,时延曲线平稳。";
    const std::string query = "TrajeanHub QuartzRelay nightly-773 压测记录在哪";

    SUBCASE("content 现切段(无词袋)") {
        std::vector<memory::MemoryEntry> entries = FillerEntries(8);
        entries.push_back(MakeEntry("fact.target", "某夜班运维纪要", "夜班例行运维",
                                    content));
        const auto ranked = memory::RankEntries(entries, query, "");
        REQUIRE_FALSE(ranked.empty());
        CHECK(ranked.front().entry->id == "fact.target");
        CHECK(ranked.front().qualifies);
        CHECK(ranked.front().content_hits >= 2);
        bool filler_qualified = false;
        for (const auto& hit : ranked) {
            if (hit.entry->id != "fact.target" && hit.qualifies) filler_qualified = true;
        }
        CHECK_FALSE(filler_qualified);
    }

    SUBCASE("content_index 词袋段(catalog 路)") {
        // 词袋直接按格式拼(与 BuildContentIndexBag 同款产物):词面:计数。
        std::vector<memory::MemoryEntry> entries = FillerEntries(8);
        memory::MemoryEntry target =
            MakeEntry("fact.target", "某夜班运维纪要", "夜班例行运维", "");
        target.content_index =
            "trajeanhub:1 quartzrelay:1 nightly:1 773:1 压测:1 测记:1 记录:1 通道:1 批次:1 全绿:1";
        entries.push_back(std::move(target));
        const auto ranked = memory::RankEntries(entries, query, "");
        REQUIRE_FALSE(ranked.empty());
        CHECK(ranked.front().entry->id == "fact.target");
        CHECK(ranked.front().qualifies);
        CHECK(ranked.front().content_hits >= 2);
    }
}

TEST_CASE("content 进索引:catalog 正门 roundtrip,注入带摘要行与正文相关段") {
    const fs::path root = TempRoot("roundtrip");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    const fs::path home = root / "home";
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, home, options);

    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.night-run";
    request.title = "夜班压测纪要";
    request.summary = "夜班例行压测,批次全绿";
    request.content =
        "压测记录第一行:例行准备完成。\n"
        "压测记录:TrajeanHub 走 QuartzRelay 通道,nightly-773 批次全绿。\n"
        "收尾:值班同学确认无误后下班。\n";
    request.confidence = "verified";
    REQUIRE(store.EnqueueSave(request, /*user_initiated=*/true).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());

    // catalog 里落了词袋。
    const std::string catalog = [&] {
        std::ifstream file(store.memory_dir() / ".state" / "catalog.json", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }();
    CHECK(catalog.find("content_index") != std::string::npos);
    bool bag_loaded = false;
    for (const auto& entry : store.ListEntries()) {
        if (entry.id == "fact.night-run") bag_loaded = !entry.content_index.empty();
    }
    CHECK(bag_loaded);

    // 正文词命中的召回:载荷带摘要行,且优先给命中段而不是开头段。
    const std::string context =
        store.BuildTurnContext("TrajeanHub QuartzRelay nightly-773 压测记录", repo,
                                memory::QueryOrigin::User);
    CHECK(context.find("## 召回: fact.night-run") != std::string::npos);
    CHECK(context.find("摘要: 夜班例行压测,批次全绿") != std::string::npos);
    CHECK(context.find("QuartzRelay 通道") != std::string::npos);

    const memory::RecallTrace trace = store.LastTrace();
    REQUIRE(trace.valid);
    bool traced_hit = false;
    for (const auto& item : trace.entries) {
        if (item.id != "fact.night-run") continue;
        traced_hit = item.injected;
        CHECK(item.content_hits >= 2);
    }
    CHECK(traced_hit);

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("护栏话术:注入段头尾都写明,零命中不塞脚手架") {
    const fs::path root = TempRoot("guardrail");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    const fs::path home = root / "home";
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, home, options);

    // 沾边但不含答案的候选:关键词硬命中,注入只是线索——护栏话术必须在场。
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.deploy-cadence";
    request.title = "部署节奏";
    request.summary = "项目每逢周三发版";
    request.content = "部署节奏:每周三上午走发布流程,回滚预案随包附带。";
    request.keywords = {"部署节奏"};
    request.confidence = "user-stated";
    REQUIRE(store.EnqueueSave(request, /*user_initiated=*/true).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());

    const std::string context =
        store.BuildTurnContext("部署节奏是什么", repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(context.empty());
    CHECK(context.find("## 召回: fact.deploy-cadence") != std::string::npos);
    // 头部护栏 + 尾部护栏:线索不足以确定答案就如实说不知道。
    CHECK(context.find("不足以确定答案") != std::string::npos);
    CHECK(context.find("如实回答不知道") != std::string::npos);
    CHECK(context.find("不要从线索外推") != std::string::npos);

    // 低分拦截:与库面全不相干的问法,词项凑不满门槛,零注入零脚手架。
    const std::string nothing = store.BuildTurnContext("今天天气如何适合散步吗", repo,
                                                        memory::QueryOrigin::User);
    CHECK(nothing.empty());
    const memory::RecallTrace trace = store.LastTrace();
    REQUIRE(trace.valid);
    CHECK(trace.injected_count == 0);
    for (const auto& item : trace.entries) {
        CHECK_FALSE(item.injected);
        if (!item.injected) CHECK(item.below_threshold);
    }

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("预算选条:按分装填、同分按 id 稳定、截断保 summary、丢弃带理由") {
    const fs::path root = TempRoot("budget");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    const fs::path home = root / "home";
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    options.max_results = 2;
    options.max_retrieval_bytes = 2048;  // 单条上限 1024:截断路径有得测
    memory::ProjectMemory store(*identity, home, options);

    // 三条同分条目:同标题同关键词,正文词频同构(只换一个等长且不进查询
    // 的词),分数完全并列——装填序只看 topic id。
    const auto make_request = [](const std::string& id, const std::string& who) {
        memory::SaveRequest request;
        request.kind = memory::MemoryKind::Fact;
        request.id = id;
        request.title = "第一班产线纪要";
        request.summary = "第一班产线读数与开工时间";
        std::string content = "摘要行占位。\n";
        for (int i = 0; i < 12; ++i) {
            content += who + "第一班产线记录仪 07:00 开工,读数正常,交接完毕。\n";
        }
        request.content = content;
        request.keywords = {"产线纪要"};
        request.confidence = "verified";
        return request;
    };
    REQUIRE(store.EnqueueSave(make_request("fact.line-a", "甲字"), true).has_value());
    REQUIRE(store.EnqueueSave(make_request("fact.line-b", "乙字"), true).has_value());
    REQUIRE(store.EnqueueSave(make_request("fact.line-c", "丙字"), true).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());

    const std::string query = "第一班产线纪要 07:00 开工";
    const std::string first = store.BuildTurnContext(query, repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(first.empty());

    // 装填按排级序(trace.entries 即排级序):前两条注入,第三条预算丢弃
    // 且带理由。注入的两段都在 prompt 里,丢弃的不在。
    const memory::RecallTrace trace = store.LastTrace();
    REQUIRE(trace.valid);
    REQUIRE(trace.entries.size() >= 3);
    REQUIRE(trace.entries[0].injected);
    REQUIRE(trace.entries[1].injected);
    CHECK_FALSE(trace.entries[2].injected);
    CHECK(trace.entries[2].budget_dropped);
    CHECK(trace.entries[2].drop_reason == "max_results");
    CHECK(first.find("## 召回: " + trace.entries[0].id) != std::string::npos);
    CHECK(first.find("## 召回: " + trace.entries[1].id) != std::string::npos);
    CHECK(first.find("## 召回: " + trace.entries[2].id) == std::string::npos);

    // 单条截断保 summary 完整:每段都带完整摘要行,正文被削。
    CHECK(first.find("摘要: 第一班产线读数与开工时间") != std::string::npos);
    CHECK(trace.injected_count == 2);
    CHECK(trace.injected_bytes <= 2048);
    bool truncated_flag = false;
    for (const auto& item : trace.entries) {
        if (item.injected) {
            CHECK(item.bytes <= 1024);
            if (item.content_truncated) truncated_flag = true;
        }
    }
    CHECK(truncated_flag);

    // 复算稳定:同问重跑,产出逐字节一致。
    const std::string second = store.BuildTurnContext(query, repo, memory::QueryOrigin::User);
    CHECK(first == second);

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("预算选条:同分条目的排级序按 topic id 定,可复算") {
    // 排级层直测:三条分数完全并列(同标题同关键词同摘要,正文等长同构,
    // 时间戳一致),末位裁决只剩 topic id。
    std::vector<memory::MemoryEntry> entries;
    const auto make = [](const std::string& id, const std::string& who) {
        memory::MemoryEntry entry = MakeEntry(id, "第一班产线纪要", "第一班产线读数与开工时间",
                                              "");
        entry.keywords = {"产线纪要"};
        std::string content;
        for (int i = 0; i < 12; ++i) {
            content += who + "第一班产线记录仪 07:00 开工,读数正常,交接完毕。\n";
        }
        entry.content = content;
        return entry;
    };
    entries.push_back(make("fact.line-b", "乙字"));
    entries.push_back(make("fact.line-c", "丙字"));
    entries.push_back(make("fact.line-a", "甲字"));
    const auto ranked = memory::RankEntries(entries, "第一班产线纪要 07:00 开工", "");
    REQUIRE(ranked.size() == 3);
    CHECK(ranked[0].score == ranked[1].score);
    CHECK(ranked[1].score == ranked[2].score);
    CHECK(ranked[0].entry->id == "fact.line-a");
    CHECK(ranked[1].entry->id == "fact.line-b");
    CHECK(ranked[2].entry->id == "fact.line-c");
    // 再算一遍,序不动(可复算)。
    const auto again = memory::RankEntries(entries, "第一班产线纪要 07:00 开工", "");
    REQUIRE(again.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(again[i].entry->id == ranked[i].entry->id);
    }
}
