// 两层会话标题的"账"(骨架拆解反弹·问题 2)单测:本地起名的三道门、
// 一场只试一次、落盘成功才占标题、resume 补名、/clear 重开自动起名、
// 代数弃迟到的精炼结果。判定逻辑原先埋在 TerminalSessionController 的
// 四个方法里,起一只完整控制器才能测;现在这只小类配一只真 SessionStore
// (临时目录)就够。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "app/session_title_account.hpp"
#include "sessions/session_store.hpp"

namespace {

using lubancode::app::SessionTitleAccount;
using LocalResult = SessionTitleAccount::LocalResult;
using AdoptResult = SessionTitleAccount::AdoptResult;

// 每例一只临时目录 + 开了档的 SessionStore + 绑好的账。
struct TitleFixture {
    std::filesystem::path dir;
    lubancode::sessions::SessionStore store;
    std::string title;
    bool pending = false;
    bool broken = false;
    std::unique_ptr<SessionTitleAccount> account;

    explicit TitleFixture(bool begin_store = true)
        : dir(std::filesystem::temp_directory_path() /
              ("lubancode-title-account-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)))),
          store(dir.string()) {
        std::filesystem::create_directories(dir);
        if (begin_store) {
            lubancode::sessions::SessionMeta meta;
            meta.wire = "anthropic";
            meta.model = "test-model";
            meta.cwd = dir.string();
            REQUIRE(store.Begin(meta, "test-session"));
            REQUIRE(store.active());
        }
        account = std::make_unique<SessionTitleAccount>(title, pending, store, broken);
    }
    ~TitleFixture() {
        account.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

lubancode::app::SessionTitleRefiner::Outcome RefinedOutcome(std::uint64_t generation,
                                                            const std::string& text) {
    lubancode::app::SessionTitleRefiner::Outcome outcome;
    outcome.ok = true;
    outcome.title = text;
    outcome.model = "cheap-model";
    outcome.generation = generation;
    return outcome;
}

}  // namespace

TEST_CASE("首问本地起名:三道门与一场一次") {
    TitleFixture fx;
    // 首问起名成功:标题落定、/sessions 侧能读到(title 事件行真落了盘)。
    CHECK(fx.account->BeginLocalTitle("帮我把这个函数拆一拆") == LocalResult::Set);
    CHECK(fx.title == "帮我把这个函数拆一拆");
    const auto listed = lubancode::sessions::ListSessions(fx.dir.string(), 1);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].title == fx.title);

    // 一场只试一次:第二次直接 NoNeed,标题不动。
    CHECK(fx.account->BeginLocalTitle("换个问题") == LocalResult::NoNeed);
    CHECK(fx.title == "帮我把这个函数拆一拆");

    // 已有标题(人工 /title 先落):不起。
    TitleFixture fx2;
    fx2.title = "人工标题";
    CHECK(fx2.account->BeginLocalTitle("首问") == LocalResult::NoNeed);

    // 人工标题待建档(pending):不起。
    TitleFixture fx3;
    fx3.pending = true;
    CHECK(fx3.account->BeginLocalTitle("首问") == LocalResult::NoNeed);

    // 全空白首问:没的可起,标题留空。
    TitleFixture fx4;
    CHECK(fx4.account->BeginLocalTitle("   \n  ") == LocalResult::NoUsableText);
    CHECK(fx.title.empty() == false);
    CHECK(fx4.title.empty());
}

TEST_CASE("没建档的场子不起名") {
    TitleFixture fx(/*begin_store=*/false);
    CHECK(!fx.store.active());
    CHECK(fx.account->BeginLocalTitle("首问") == LocalResult::NoNeed);
    CHECK(fx.title.empty());
}

TEST_CASE("resume 补名:有标题不重复,没标题补本地,翻代弃旧精炼") {
    TitleFixture fx;
    // 在飞精炼带着旧代数;resume 善后翻代后落地 -> Ignored(人工优先)。
    const std::uint64_t old_generation = fx.account->generation();
    CHECK(fx.account->BackfillOnResume("老档首条正文") == LocalResult::Set);
    CHECK(fx.account->generation() == old_generation + 1);
    CHECK(fx.title == "老档首条正文");
    CHECK(fx.account->AdoptRefined(RefinedOutcome(old_generation, "迟到的精炼")) ==
          AdoptResult::Ignored);
    CHECK(fx.title == "老档首条正文");  // 迟到结果不落地

    // 已有标题的老档:resume 不重复起名。
    TitleFixture fx2;
    fx2.title = "老档已有标题";
    CHECK(fx2.account->BackfillOnResume("正文") == LocalResult::NoNeed);
    CHECK(fx2.title == "老档已有标题");
}

TEST_CASE("精炼结果采纳:对代才换,失败与空标题保留本地") {
    TitleFixture fx;
    REQUIRE(fx.account->BeginLocalTitle("本地标题起点") == LocalResult::Set);
    const std::uint64_t generation = fx.account->generation();

    SUBCASE("对代成功:换标题") {
        CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "精炼后的短标题")) ==
              AdoptResult::Adopted);
        CHECK(fx.title == "精炼后的短标题");
        const auto listed = lubancode::sessions::ListSessions(fx.dir.string(), 1);
        REQUIRE(listed.size() == 1);
        CHECK(listed[0].title == "精炼后的短标题");
    }
    SUBCASE("失败/空标题:保留本地") {
        lubancode::app::SessionTitleRefiner::Outcome failed;
        failed.ok = false;
        failed.generation = generation;
        CHECK(fx.account->AdoptRefined(failed) == AdoptResult::Ignored);
        CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "")) == AdoptResult::Ignored);
        CHECK(fx.title == "本地标题起点");
    }
    SUBCASE("翻代后迟到:弃") {
        fx.account->BumpGeneration();
        CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "迟到的精炼")) ==
              AdoptResult::Ignored);
        CHECK(fx.title == "本地标题起点");
    }
}

TEST_CASE("/clear 重开:自动起名复位,下一问重走本地起名") {
    TitleFixture fx;
    REQUIRE(fx.account->BeginLocalTitle("第一场标题") == LocalResult::Set);
    const std::uint64_t generation = fx.account->generation();
    fx.title.clear();  // /clear 清场(真场子还会换存档,这里只对账)
    fx.account->ResetForNewSession();
    CHECK(fx.account->generation() == generation + 1);
    CHECK(fx.account->auto_attempted() == false);
    // 旧代数的精炼结果在新场子落地:弃。
    CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "旧场精炼")) == AdoptResult::Ignored);
    // 新场子的首问重新起名。
    CHECK(fx.account->BeginLocalTitle("新场首问") == LocalResult::Set);
    CHECK(fx.title == "新场首问");
}
