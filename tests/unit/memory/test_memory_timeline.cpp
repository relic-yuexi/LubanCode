// 时间线锚点单(记忆写入侧改进)的端到端单测:occurred_at 写入正文锚行、
// 注入按时间排成一条线、时间行可见不重复、无字段旧条目混排稳定。
// 全部走真路:EnqueueSave → RunPendingMemoryJobs → BuildTurnContext。

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

std::string TempStamp() {
    return std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// 一条带时间的 fact:keywords 决定谁能被查到。
void EnqueueFact(memory::ProjectMemory& store, const std::string& id,
                 const std::string& title, const std::string& body,
                 const std::vector<std::string>& keywords, const std::string& occurred_at) {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = id;
    request.title = title;
    request.summary = title + " 的摘要";
    request.content = body;
    request.keywords = keywords;
    request.confidence = "verified";
    request.occurred_at = occurred_at;
    REQUIRE(store.EnqueueSave(request).has_value());
}

// 注入正文里"## 召回: "段的 id 顺序。
std::vector<std::string> SectionOrder(const std::string& context) {
    std::vector<std::string> order;
    std::size_t pos = 0;
    while ((pos = context.find("## 召回: ", pos)) != std::string::npos) {
        const std::size_t begin = pos + strlen("## 召回: ");
        const std::size_t end = context.find('\n', begin);
        order.push_back(context.substr(begin, end - begin));
        pos = end;
    }
    return order;
}

}  // namespace

TEST_CASE("时间线锚点: 写入侧正文锚行与 frontmatter 字段") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-timeline-write-" + TempStamp());
    std::error_code ec;
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git", ec);
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    EnqueueFact(store, "fact.event-may", "五月事件", "五月发生的事。", {"mayday"}, "2023-05-08");
    EnqueueFact(store, "fact.event-none", "无时间事件", "没有时间的事。", {"notime"}, "");
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const fs::path memory_dir = identity->workspace_dir / "memory";
    // 带时间:frontmatter 有 occurred_at,正文头部一行【日期】锚。
    const std::string may = Read(memory_dir / "facts" / "event-may.md");
    CHECK(may.find("occurred_at: 2023-05-08") != std::string::npos);
    const std::size_t anchor = may.find("【2023-05-08】");
    REQUIRE(anchor != std::string::npos);
    CHECK(may.find("【2023-05-08】\n\n五月发生的事。") != std::string::npos);
    CHECK(may.find("【2023-05-08】", anchor + 1) == std::string::npos);  // 锚只落一行
    // 无时间:occurred_at: ~(yaml-cpp 的 Null 定型),正文无锚。
    const std::string none = Read(memory_dir / "facts" / "event-none.md");
    CHECK(none.find("occurred_at: ~") != std::string::npos);
    CHECK(none.find("【") == std::string::npos);

    // 条目读回带 occurred_at;重复 upsert(带旧正文回来)不翻倍。
    auto entries = store.ListEntries();
    REQUIRE(entries.size() == 2);
    bool saw_may = false;
    for (const auto& entry : entries) {
        if (entry.id == "fact.event-may") {
            saw_may = true;
            CHECK(entry.occurred_at == "2023-05-08");
        } else {
            CHECK(entry.occurred_at.empty());
        }
    }
    CHECK(saw_may);

    {
        memory::SaveRequest again;
        again.kind = memory::MemoryKind::Fact;
        again.id = "fact.event-may";
        again.title = "五月事件";
        again.summary = "五月事件 的摘要";
        again.content = "【2023-05-08】\n\n五月发生的事,补一句。";
        again.keywords = {"mayday"};
        again.confidence = "verified";
        again.occurred_at = "2023-05-08";
        REQUIRE(store.EnqueueSave(again).has_value());
    }
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());
    const std::string rewritten = Read(memory_dir / "facts" / "event-may.md");
    CHECK(rewritten.find("补一句") != std::string::npos);
    const std::size_t first_anchor = rewritten.find("【2023-05-08】");
    REQUIRE(first_anchor != std::string::npos);
    CHECK(rewritten.find("【2023-05-08】", first_anchor + 1) == std::string::npos);  // 只一处

    // 更新不带 occurred_at:旧日期保住,不丢。
    {
        memory::SaveRequest no_date;
        no_date.kind = memory::MemoryKind::Fact;
        no_date.id = "fact.event-may";
        no_date.title = "五月事件";
        no_date.summary = "五月事件 的摘要";
        no_date.content = "五月发生的事,又改一版。";
        no_date.keywords = {"mayday"};
        no_date.confidence = "verified";
        REQUIRE(store.EnqueueSave(no_date).has_value());
    }
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());
    const std::string kept = Read(memory_dir / "facts" / "event-may.md");
    CHECK(kept.find("occurred_at: 2023-05-08") != std::string::npos);
    CHECK(kept.find("【2023-05-08】\n\n五月发生的事,又改一版。") != std::string::npos);

    // 形状不像日期的 occurred_at 在正门被拒。
    memory::SaveRequest sloppy;
    sloppy.kind = memory::MemoryKind::Fact;
    sloppy.id = "fact.event-sloppy";
    sloppy.title = "草率日期";
    sloppy.summary = "s";
    sloppy.content = "正文。";
    sloppy.confidence = "verified";
    sloppy.occurred_at = "上周三";
    CHECK_FALSE(store.EnqueueSave(sloppy).has_value());

    fs::remove_all(root, ec);
}

TEST_CASE("时间线锚点: 注入按时间排线,时间行可见,锚行不重复") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-timeline-inject-" + TempStamp());
    std::error_code ec;
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git", ec);
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    // 三条带时间的事实。七月那条多吃一个关键词,排级榜上压过五月——注入
    // 仍须按时间升序排成线(证明真的重排了,不是碰巧)。
    EnqueueFact(store, "fact.timeline-may", "五月会面", "五月两人见面聊了搬家。", {"maymeet"},
                "2023-05-08");
    EnqueueFact(store, "fact.timeline-jun", "六月搬家", "六月搬到新区。", {"junmove"},
                "2023-06-19");
    EnqueueFact(store, "fact.timeline-jul", "七月聚会", "七月聚会上又见。", {"julparty", "maymeet"},
                "2023-07-01");
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const std::string context = store.BuildTurnContext("maymeet junmove julparty 时间线", repo);
    REQUIRE(!context.empty());
    const auto order = SectionOrder(context);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "fact.timeline-may");
    CHECK(order[1] == "fact.timeline-jun");
    CHECK(order[2] == "fact.timeline-jul");

    // 每段时间行可见,正文锚行不重复占段(骨架里已带"时间: "行)。
    for (const std::string& date : {"2023-05-08", "2023-06-19", "2023-07-01"}) {
        CHECK(context.find("时间: " + date) != std::string::npos);
        CHECK(context.find("【" + date + "】") == std::string::npos);
    }

    // trace 的排级账不受拼装序影响:榜上七月在前(两个硬命中)。
    const auto trace = store.LastTrace();
    REQUIRE(trace.valid);
    REQUIRE(trace.entries.size() >= 3);
    CHECK(trace.entries.front().id == "fact.timeline-jul");
    CHECK(trace.injected_count == 3);

    fs::remove_all(root, ec);
}

TEST_CASE("时间线锚点: 同分按 topic id,无时间条目按排级序续后") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-timeline-mixed-" + TempStamp());
    std::error_code ec;
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git", ec);
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    // 同日两条:id 升序定先后。另有一条无时间的事实,分数最高(三个硬命
    // 中),拼装时让位给时间线,续在时间线之后。
    EnqueueFact(store, "fact.same-day-b", "同日乙", "同日乙的事。", {"samedayb"}, "2023-05-08");
    EnqueueFact(store, "fact.same-day-a", "同日甲", "同日甲的事。", {"samedaya"}, "2023-05-08");
    EnqueueFact(store, "fact.no-time", "无时间", "没有时间的事。", {"samedaya", "samedayb", "notime"},
                "");
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const std::string context = store.BuildTurnContext("samedaya samedayb notime 时间线", repo);
    const auto order = SectionOrder(context);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "fact.same-day-a");  // 同分按 id
    CHECK(order[1] == "fact.same-day-b");
    CHECK(order[2] == "fact.no-time");  // 无时间不参与时间排序,续在时间线后
    const auto trace = store.LastTrace();
    REQUIRE(trace.valid);
    CHECK(trace.entries.front().id == "fact.no-time");  // 排级榜仍是它第一

    fs::remove_all(root, ec);
}

TEST_CASE("时间线锚点: 全库无时间字段时注入序与从前一致(排级序)") {
    const fs::path root = fs::temp_directory_path() / ("lubancode-timeline-legacy-" + TempStamp());
    std::error_code ec;
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git", ec);
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());
    const fs::path memory_dir = identity->workspace_dir / "memory";

    // 手写三份旧主题:frontmatter 没有 occurred_at 键(旧条目),写入侧
    // 别碰它们,读入照常。
    fs::create_directories(memory_dir / "facts", ec);
    const std::vector<std::pair<std::string, std::string>> legacy = {
        {"fact.old-plain-a", "alpha"}, {"fact.old-plain-b", "beta"}, {"fact.old-plain-c", "gamma"}};
    for (const auto& [id, keyword] : legacy) {
        Write(memory_dir / "facts" / (id.substr(5) + ".md"),
              "---\nname: " + id.substr(5) + "\ndescription: " + id + " 摘要\n"
              "metadata:\n  schema: 3\n  node_type: memory\n  type: fact\n  id: " + id + "\n"
              "  confidence: verified\n  status: active\n"
              "  scope: {level: project, kind: project, value: \"\"}\n"
              "  origin_session_ids: []\n  created: 2026-08-16\n  modified: 2026-08-16\n"
              "  last_verified: 2026-08-16\n  expires: null\n  keywords:\n    - " + keyword + "\n"
              "  evidence: []\n---\n\n# " + id + "\n\n旧正文,没有时间字段。\n");
    }
    REQUIRE(memory::RebuildMemoryIndex(memory_dir).has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    memory::ProjectMemory store(*identity, root / "home", options);
    for (const auto& entry : store.ListEntries()) {
        CHECK(entry.occurred_at.empty());
    }

    // 注入照旧:段序 = 排级序,没有时间行,不炸。
    const std::string context = store.BuildTurnContext("alpha beta gamma 时间线", repo);
    const auto order = SectionOrder(context);
    REQUIRE(order.size() == 3);
    CHECK(context.find("时间: ") == std::string::npos);
    const auto trace = store.LastTrace();
    REQUIRE(trace.valid);
    std::vector<std::string> ranked_injected;
    for (const auto& entry : trace.entries) {
        if (entry.injected) ranked_injected.push_back(entry.id);
    }
    CHECK(order == ranked_injected);  // 无时间字段时拼装序就是排级序

    fs::remove_all(root, ec);
}
