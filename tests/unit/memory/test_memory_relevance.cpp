// 相关性分级与包裹式护栏的单测(记忆幻觉根治单《adversarial 桶注入诱导硬答》):
//   1) 判据 GradeRelevance:同句共现(>=2 词组 + 锚同行)判强;散在各行
//      的词面重叠判弱;路径/symbol 硬命中(pinpoint)直接判强;无锚时
//      >=3 词组同现兜底;问题本身不足两个词组时门槛收到 1;
//   2) 注入形状:弱档垫尾 + 段头 [弱相关] 短标,强档在前不标;头部信息
//      行如实报"末 M 条弱相关"(全弱时另有一档话术);段尾总护栏升级;
//   3) trace 报账:weak/cooccur 逐条落盘读回;
//   4) 文案中英成对:en 档出英文标注与护栏(zh-CN 为全局缺省,另有既有
//      断言钉住中文措辞)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "cli/i18n.hpp"
#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

std::string TempStamp() {
    return std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    const fs::path path = fs::temp_directory_path() /
                          ("lubancode-memory-relevance-" + TempStamp() + "-" + name + "-" +
                           std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

memory::RelevanceQuery MakeQuery(std::vector<std::vector<std::string>> groups) {
    memory::RelevanceQuery query;
    for (auto& terms : groups) query.groups.push_back(memory::RelevanceGroup{std::move(terms)});
    return query;
}

void EnqueueFact(memory::ProjectMemory& store, const std::string& id, const std::string& title,
                 const std::string& summary, const std::string& content,
                 const std::vector<std::string>& keywords) {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = id;
    request.title = title;
    request.summary = summary;
    request.content = content;
    request.keywords = keywords;
    request.confidence = "verified";
    REQUIRE(store.EnqueueSave(request, /*user_initiated=*/true).has_value());
}

// 语言是进程级全局状态,用完还原(同 test_tool_text.cpp 的规矩)。
struct LangGuard {
    ~LangGuard() { lubancode::cli::SetLanguage("zh-CN"); }
};

}  // namespace

TEST_CASE("判据:同句共现判强,散行重叠判弱,pinpoint 直接判强") {
    // 两个词组 + 锚同一行:实体-动作-对象一句话说全。
    memory::RelevanceQuery query = MakeQuery({{"琼枝"}, {"诗社"}});
    memory::RelevanceGrade grade = memory::GradeRelevance(
        "琼枝在诗社上念了新诗。", "", query, {"琼枝"}, /*pinpoint_hit=*/false);
    CHECK(grade.strong);
    CHECK(grade.best_line_groups == 2);

    // 词组各自单行,锚不在共现行:词面重叠,弱。
    grade = memory::GradeRelevance("琼枝平日在家读书。\n隔壁诗社另有人办。\n", "", query,
                                   {"琼枝"}, false);
    CHECK_FALSE(grade.strong);
    CHECK(grade.best_line_groups == 1);

    // 锚不在场但三个词组同一行:纯内容匹配的实质共现兜底。
    memory::RelevanceQuery three = MakeQuery({{"琼枝"}, {"诗社"}, {"南京"}});
    grade = memory::GradeRelevance("那年琼枝在南京的诗社里谋生。", "", three, {}, false);
    CHECK(grade.strong);

    // 两个词组同一行但无锚、也不满三词组:仍弱。
    grade = memory::GradeRelevance("南京诗社开张那天。", "", query, {}, false);
    CHECK_FALSE(grade.strong);

    // 路径/symbol 硬命中:定位精确到条,不扫行直接判强。
    grade = memory::GradeRelevance("正文与问题词面全不相干。", "", query, {}, true);
    CHECK(grade.strong);

    // 问题本身只有一个词组:门槛收到 1,锚与唯一词组同行即强
    //(生产常态:"部署节奏是什么"——问题就问这一个实体)。
    memory::RelevanceQuery single = MakeQuery({{"部署节奏"}});
    grade = memory::GradeRelevance("部署节奏:每周三上午走发布流程。", "", single, {"部署节奏"},
                                    false);
    CHECK(grade.strong);
    // 唯一词组不在正文任何一行:弱。
    grade = memory::GradeRelevance("正文只谈别的。", "", single, {"部署节奏"}, false);
    CHECK_FALSE(grade.strong);

    // 摘要算一行:常是唯一把两个实体说进一句话的地方。
    grade = memory::GradeRelevance("正文一行。\n又一行。\n", "琼枝在诗社念诗的纪要", query,
                                   {"琼枝"}, false);
    CHECK(grade.strong);
}

TEST_CASE("注入形状:弱档垫尾带标注,强档在前不标,头部信息行报账") {
    const fs::path root = TempRoot("shape");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    // 强条目:关键词"梅香"锚 + 梅香/当票 两词组同行。
    EnqueueFact(store, "fact.strong-echo", "梅香当票", "梅香当了铺子的票",
                "梅香把当票押在城南柜上。\n", {"梅香"});
    // 弱条目:词组各自行,永不同行共现(标题也不沾问题词面——标题行不算
    // 共现,但别给判据添乱);两个关键词硬命中让排级榜必压过强条目——证
    // 明弱档真的被降权垫尾,不是碰巧排在后面。
    EnqueueFact(store, "fact.weak-scatter", "旧年账目杂记", "旧年账目杂记",
                "梅香常来柜前走动。\n有人拿当票换了钱。\n", {"梅香", "当票"});
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const std::string context =
        store.BuildTurnContext("梅香 当票 的事", repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(context.empty());
    // 弱档段头带 [弱相关] 短标;强档段头干净(不误伤)。
    CHECK(context.find("## 召回: fact.strong-echo\n") != std::string::npos);
    CHECK(context.find("## 召回: fact.weak-scatter [弱相关]") != std::string::npos);
    // 强档在前:即使弱条目排级分更高,拼装序也是强先弱后。
    CHECK(context.find("fact.strong-echo") < context.find("fact.weak-scatter"));
    // 头部信息行:末 M 条弱相关如实报数(1 强 1 弱)。
    CHECK(context.find("以下 2 条召回按相关性排序") != std::string::npos);
    CHECK(context.find("末 1 条只是话题相近的弱相关背景") != std::string::npos);
    // 段尾总护栏(变体 B 措辞,快测定形):必须有直接陈述才可据以回答。
    CHECK(context.find("必须有一条记忆直接陈述问题所问的事实才可据以回答") != std::string::npos);

    // trace:弱档逐条报账(weak + cooccur)。
    const memory::RecallTrace trace = store.LastTrace();
    REQUIRE(trace.valid);
    bool saw_weak = false;
    bool saw_strong = false;
    for (const auto& item : trace.entries) {
        if (item.id == "fact.weak-scatter" && item.injected) {
            saw_weak = true;
            CHECK(item.weak);
            CHECK(item.cooccur < 2);
        }
        if (item.id == "fact.strong-echo" && item.injected) {
            saw_strong = true;
            CHECK_FALSE(item.weak);
            CHECK(item.cooccur >= 2);
        }
    }
    CHECK(saw_weak);
    CHECK(saw_strong);
    CHECK(trace.injected_count == 2);

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("注入形状:全弱时信息行换话术,条数报账如实") {
    const fs::path root = TempRoot("allweak");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    EnqueueFact(store, "fact.only-weak", "桂堂春宴", "春日宴饮的杂记",
                "桂堂那日开了春宴。\n席间有人论起旧画。\n", {"桂堂"});
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const std::string context =
        store.BuildTurnContext("桂堂 旧画 的事", repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(context.empty());
    CHECK(context.find("## 召回: fact.only-weak [弱相关]") != std::string::npos);
    // 全弱:信息行走"均只是弱相关"一档,不说"前 N 条直接相关"。
    CHECK(context.find("以下 1 条召回均与问题只是话题相近的弱相关背景") != std::string::npos);
    CHECK(context.find("按相关性排序：前 ") == std::string::npos);

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("护栏文案:头部话术升级,零命中仍零注入") {
    const fs::path root = TempRoot("guard");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    EnqueueFact(store, "fact.clinic-card", "诊疗卡", "社区诊所的诊疗卡",
                "诊疗卡:社区诊所按卡取号,外地卡不通用。\n", {"诊疗卡"});
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    const std::string context =
        store.BuildTurnContext("诊疗卡 怎么用", repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(context.empty());
    // 头部护栏升级措辞(变体 B,快测定形):直接陈述才是答案依据,否则不知道。
    CHECK(context.find("回答所问必须有一条记忆直接陈述该事实") != std::string::npos);
    CHECK(context.find("就如实回答不知道，不要从线索外推或补全") != std::string::npos);
    // 单实体问法(问题只有"诊疗卡"一个词组)不降档:锚与唯一词组同行即强。
    CHECK(context.find("## 召回: fact.clinic-card\n") != std::string::npos);
    CHECK(context.find("[弱相关]") != std::string::npos);  // 段尾护栏提到标注

    // 零命中:不塞空脚手架(护栏也不进)。
    const std::string nothing =
        store.BuildTurnContext("今天吃什么好呢", repo, memory::QueryOrigin::User);
    CHECK(nothing.empty());

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}

TEST_CASE("文案中英成对:en 档出英文标注、信息行与总护栏") {
    LangGuard guard;
    const fs::path root = TempRoot("i18n");
    const fs::path repo = root / "repo";
    fs::create_directories(repo / ".git");
    auto identity = memory::ResolveProjectIdentity(repo, root / "home");
    REQUIRE(identity.has_value());
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;
    memory::ProjectMemory store(*identity, root / "home", options);

    EnqueueFact(store, "fact.en-weak", "book stall", "market notes",
                "She stopped by the stall.\nSomeone mentioned the bakery.\n", {"stall"});
    REQUIRE(memory::RunPendingMemoryJobs(root / "home").has_value());

    lubancode::cli::SetLanguage("en");
    const std::string context =
        store.BuildTurnContext("stall bakery affair", repo, memory::QueryOrigin::User);
    REQUIRE_FALSE(context.empty());
    CHECK(context.find("## 召回: fact.en-weak [weakly related]") != std::string::npos);
    // 全弱:信息行走 all-weak 一档的英文话术。
    CHECK(context.find("All 1 recalled entries below are weakly related background") !=
          std::string::npos);
    CHECK(context.find("answer honestly that you do not know") != std::string::npos);
    // zh-CN 的标注字样不在英文注入里。
    CHECK(context.find("[弱相关]") == std::string::npos);

    std::error_code remove_ec;
    fs::remove_all(root, remove_ec);
}
