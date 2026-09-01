// 两层会话标题的"账"(骨架拆解反弹·问题 2)单测:本地起名的三道门、
// 一场只试一次、落账成功才占标题、resume 补名、/clear 重开自动起名、
// 代数弃迟到的精炼结果。判定逻辑原先埋在 TerminalSessionController 的
// 四个方法里,起一只完整控制器才能测;现在这只小类配一只假账本
//(标题事件落账成功与否由 FixtureLedger 按开关回)就够。
//
// P0-6:旧 SessionStore 参数已删——标题真账唯一走 control.title.changed
//(TrajectorySessionLedger);这里的假账本按同语义回 bool。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "app/session_title_account.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"

namespace {

using lubancode::app::SessionTitleAccount;
using LocalResult = SessionTitleAccount::LocalResult;
using AdoptResult = SessionTitleAccount::AdoptResult;

// 真账本夹具:临时根下开一场 TrajectorySessionLedger——标题事件走
// control.title.changed(与生产同一条路)。
struct TitleFixture {
    std::filesystem::path dir;
    std::string title;
    std::optional<lubancode::runtime::TrajectorySessionLedger> ledger;
    std::unique_ptr<SessionTitleAccount> account;

    explicit TitleFixture(bool with_ledger = true)
        : dir(std::filesystem::temp_directory_path() /
              ("lubancode-title-account-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(dir / "repo", ec);
        if (with_ledger) {
            lubancode::runtime::TrajectorySessionLedger::Options options;
            options.workspaces_root = dir / "workspaces";
            options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir / "repo");
            options.lubancode_version = "test";
            auto opened = lubancode::runtime::TrajectorySessionLedger::Open(options);
            REQUIRE(opened.has_value());
            ledger.emplace(std::move(*opened));
        }
        account = std::make_unique<SessionTitleAccount>(title, ledger.has_value() ? &*ledger : nullptr);
    }
    ~TitleFixture() {
        account.reset();
        ledger.reset();
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

TEST_CASE("首问本地起名:一场一次,已有标题不起,全空白留空") {
    TitleFixture fx;
    // 首问起名:标题落定(真账走 control.title.changed)。
    CHECK(fx.account->BeginLocalTitle("帮我把这个函数拆一拆") == LocalResult::Set);
    CHECK(fx.title == "帮我把这个函数拆一拆");

    // 一场只试一次:第二次 NoNeed,标题不动。
    CHECK(fx.account->BeginLocalTitle("换个问题") == LocalResult::NoNeed);
    CHECK(fx.title == "帮我把这个函数拆一拆");

    // 已有标题(人工 /title 先落):不起。
    TitleFixture fx2;
    fx2.title = "人工标题";
    CHECK(fx2.account->BeginLocalTitle("首问") == LocalResult::NoNeed);

    // 全空白首问:没的可起,标题留空。
    TitleFixture fx4;
    CHECK(fx4.account->BeginLocalTitle("   \n  ") == LocalResult::NoUsableText);
    CHECK(fx4.title.empty());

    // 没账本的场子:不起名。
    TitleFixture fx5(/*with_ledger=*/false);
    CHECK(fx5.account->BeginLocalTitle("首问") == LocalResult::NoNeed);
    CHECK(fx5.title.empty());
}

TEST_CASE("resume 补名:翻代弃旧精炼,已有标题不重复") {
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

TEST_CASE("精炼结果采纳:对代才换,失败与空标题保留") {
    TitleFixture fx;
    REQUIRE(fx.account->BeginLocalTitle("本地标题起点") == LocalResult::Set);
    const std::uint64_t generation = fx.account->generation();

    // 对代成功:换标题(control.title.changed 落账)。
    CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "精炼后的短标题")) ==
          AdoptResult::Adopted);
    CHECK(fx.title == "精炼后的短标题");
    // 失败/空标题:保留本地。
    CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "")) == AdoptResult::Ignored);
    // 翻代后迟到:弃。
    fx.account->BumpGeneration();
    CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "迟到的精炼")) ==
          AdoptResult::Ignored);
    CHECK(fx.title == "精炼后的短标题");
}

TEST_CASE("/clear 重开:自动起名复位,代数翻号") {
    TitleFixture fx;
    const std::uint64_t generation = fx.account->generation();
    fx.title.clear();  // /clear 清场
    fx.account->ResetForNewSession();
    CHECK(fx.account->generation() == generation + 1);
    CHECK(fx.account->auto_attempted() == false);
    CHECK(fx.account->AdoptRefined(RefinedOutcome(generation, "旧场精炼")) == AdoptResult::Ignored);
    // 新场子的首问重新起名。
    CHECK(fx.account->BeginLocalTitle("新场首问") == LocalResult::Set);
    CHECK(fx.title == "新场首问");
}
