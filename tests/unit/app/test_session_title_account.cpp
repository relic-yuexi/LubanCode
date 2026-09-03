// 两层会话标题的"账"(骨架拆解反弹·问题 2)单测:本地起名的三道门、
// 一场只试一次、落账成功才占标题、resume 补名、/clear 重开自动起名、
// 代数弃迟到的精炼结果。判定逻辑原先埋在 TerminalSessionController 的
// 四个方法里,起一只完整控制器才能测;现在这只小类配一只假账本
//(标题事件落账成功与否由 FixtureLedger 按开关回)就够。
//
// P0-6:旧 SessionStore 参数已删——标题真账唯一走 control.title.changed
//(TrajectorySessionLedger);这里的假账本按同语义回 bool。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
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

// ---- 通知时序缺陷单·账与回归:真线程精修落账的数账辅助 ----

// 可控假后端:延迟 delay_ms 后回一枚短标题(带真 usage);fail=true 时回
// "空回"——流正常收场、provider 照报 usage,但一个字没吐,清洗后标题为
// 空(失败)。这是"精修失败仍记真实 usage"的实测形状:cheap 档空回/超时
// 截断,token 是真花的。与生产起飞同款:Inputs.trajectory 指 fixture 的
// 真账本,worker 线程自铸旁路桥(purpose=title_refine)把 request/usage/
// output 落进 main.jsonl。
struct FakeRefineBackend final : lubancode::api::Backend {
    int delay_ms = 0;
    bool fail = false;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel != nullptr && cancel->load()) {
                return std::unexpected(
                    lubancode::api::Error{lubancode::api::ErrorKind::Cancelled, "cancelled", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        lubancode::api::Usage usage;
        usage.input_tokens = 492;
        usage.output_tokens = fail ? 2 : 10;
        if (!fail) {
            on_event(lubancode::api::TextDelta{"精修后的短标题"});
            on_event(lubancode::api::ContentBlockDone{0});
        }
        on_event(lubancode::api::MessageDone{"end_turn", usage});
        return {};
    }
};

// 生产起飞同款 Inputs:trajectory 指 fixture 真账本,旁路桥 identity 材料
// (wire/provider)任意但非空,worker 线程落 purpose=title_refine 的一套。
lubancode::app::SessionTitleRefiner::Inputs MakeRefineInputs(
    lubancode::runtime::TrajectorySessionLedger* ledger, std::uint64_t generation, bool fail,
    int delay_ms) {
    lubancode::app::SessionTitleRefiner::Inputs inputs;
    auto backend = std::make_unique<FakeRefineBackend>();
    backend->delay_ms = delay_ms;
    backend->fail = fail;
    inputs.backend = std::move(backend);
    inputs.model = "cheap-m";
    inputs.effort = "low";
    inputs.first_query = "做一个图书管理系统";
    inputs.generation = generation;
    inputs.trajectory = ledger;
    inputs.trajectory_wire = "anthropic";
    inputs.provider = "fake";
    return inputs;
}

// 有界等 Ready(模拟空闲 composer 的 100ms 拍,只问不取)。
bool AwaitReady(lubancode::app::SessionTitleRefiner& refiner, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (refiner.Ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return refiner.Ready();
}

// main.jsonl 逐行解析后的数账器:按 kind 计数,并按 request_id 关联出
// purpose=title_refine 的请求再数它的 usage/output 事件。
struct LedgerCount {
    std::map<std::string, int> by_kind;
    std::set<std::string> refine_request_ids;
    int refine_usage = 0;      // title_refine 请求上的 model.usage.recorded
    int refine_output = 0;     // title_refine 请求上的 output 终态(completed/failed/cancelled)
    int title_changed = 0;     // control.title.changed
    int turn_overlap = 0;      // state.turn_overlap(不许出现)

    explicit LedgerCount(const lubancode::runtime::TrajectorySessionLedger& ledger) {
        std::ifstream file(ledger.session_dir() / "main.jsonl", std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }
            nlohmann::json envelope = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
            if (!envelope.is_object() || !envelope.contains("kind")) {
                continue;
            }
            const std::string kind = envelope.value("kind", "");
            by_kind[kind]++;
            if (kind == "control.title.changed") {
                title_changed++;
            }
            if (kind == "state.turn_overlap") {
                turn_overlap++;
            }
            const bool has_request = envelope.contains("request_id");
            if (kind == "model.request.prepared" && has_request &&
                envelope.value("payload", nlohmann::json::object()).value("purpose", "") ==
                    "title_refine") {
                refine_request_ids.insert(envelope.value("request_id", ""));
            }
            if ((kind == "model.usage.recorded") && has_request &&
                refine_request_ids.count(envelope.value("request_id", "")) > 0) {
                refine_usage++;
            }
            if ((kind == "model.output.completed" || kind == "model.output.failed" ||
                 kind == "model.output.cancelled") &&
                has_request && refine_request_ids.count(envelope.value("request_id", "")) > 0) {
                refine_output++;
            }
        }
    }
};

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

TEST_CASE("首问本地起名:路径只取文件主题,不把盘符和提交尾巴当标题") {
    TitleFixture fx;
    CHECK(fx.account->BeginLocalTitle(
              "D:\\lubancode\\todos\\终端思考活动条重画与输入框光标跳动.todo commit一下这个吧") ==
          LocalResult::Set);
    CHECK(fx.title == "终端思考活动条重画与输入框光标跳动");

    TitleFixture quoted;
    CHECK(quoted.account->BeginLocalTitle("\"D:\\work dir\\修复登录超时.todo\" 请提交") ==
          LocalResult::Set);
    CHECK(quoted.title == "修复登录超时");
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

// 通知时序缺陷单·账与回归一:一次成功精修只记一套旁路事件——空闲唤醒
// 与主循环双收货点(圈顶+ReadLine 后)连问连取,不得造成重复 Take、重复
// usage 或重复 control.title.changed。真线程假后端 + 真 Trajectory 账本,
// worker 侧旁路桥与主线程标题事件同盘串行,数 main.jsonl 定谳。
TEST_CASE("账与回归:成功精修只记一套旁路事件,重复收货不重复记账") {
    TitleFixture fx;
    REQUIRE(fx.account->BeginLocalTitle("本地标题起点") == LocalResult::Set);

    // 起飞(生产同款 Inputs,worker 线程自铸旁路桥)。
    auto& refiner = fx.account->refiner();
    CHECK(refiner.Start(MakeRefineInputs(&*fx.ledger, fx.account->generation(),
                                         /*fail=*/false, /*delay_ms=*/80)));

    // 模拟主循环:唤醒拍先问 Ready(只读),翻真才 Take。
    REQUIRE(AwaitReady(refiner, 2000));
    const auto first = refiner.TakeFinished();
    REQUIRE(first.has_value());
    CHECK(first->ok);
    CHECK(fx.account->AdoptRefined(*first) == AdoptResult::Adopted);
    CHECK(fx.title == "精修后的短标题");

    // 第二收货点(ReadLine 后)再问再取:槽已复位,什么都拿不到,不再采纳。
    // 这是双收货点(圈顶+ReadLine 后)与空闲唤醒三路并行的防重复闸——
    // Take 单次出清,Ready 翻假,后续拍不再醒。
    CHECK_FALSE(refiner.Ready());
    CHECK_FALSE(refiner.Busy());
    CHECK_FALSE(refiner.TakeFinished().has_value());
    CHECK(fx.title == "精修后的短标题");  // 没有第二枚 outcome 可喂,标题不动

    // 数账:本地+采纳恰两条 title.changed;title_refine 恰一枚 prepared、
    // 一枚 usage、一枚 output 终态;无 turn_overlap。
    const LedgerCount count(*fx.ledger);
    CHECK(count.title_changed == 2);
    CHECK(count.refine_request_ids.size() == 1);
    CHECK(count.refine_usage == 1);
    CHECK(count.refine_output == 1);
    CHECK(count.turn_overlap == 0);
}

// 通知时序缺陷单·账与回归二:精修失败仍记真实 usage(旁路桥的半截账),
// 本地标题保住,不走 WriteFailed 车轮的触发条件(失败路 AdoptRefined 回
// Ignored,只有落盘失败才 WriteFailed——那才是打车轮的分支)。
TEST_CASE("账与回归:失败精修照记 usage,本地标题保住,不触发写失败车轮") {
    TitleFixture fx;
    REQUIRE(fx.account->BeginLocalTitle("本地标题起点") == LocalResult::Set);

    auto& refiner = fx.account->refiner();
    CHECK(refiner.Start(MakeRefineInputs(&*fx.ledger, fx.account->generation(),
                                         /*fail=*/true, /*delay_ms=*/40)));
    REQUIRE(AwaitReady(refiner, 2000));  // 失败结局也完工待收:不醒就丢账
    const auto outcome = refiner.TakeFinished();
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);                       // 采样失败
    CHECK(outcome->title.empty());                  // 无标题可换
    CHECK(outcome->accounting.usage_reported);      // 半截也出账(真花的 token)
    CHECK(outcome->accounting.usage.input_tokens == 492);
    CHECK(outcome->accounting.usage.output_tokens == 2);  // 失败路按实报(空回 2)

    // 采纳判定:失败走 Ignored(不是 WriteFailed——车轮只在 WriteFailed 打)。
    CHECK(fx.account->AdoptRefined(*outcome) == AdoptResult::Ignored);
    CHECK(fx.title == "本地标题起点");  // 本地标题保住,不重试

    // 数账:空回的旁路请求照记一枚 prepared + 一枚 usage + 一枚 output
    // 终态(流正常收场走 completed,失败语义在 outcome.ok=false——Sanitize
    // 清洗后为空);title.changed 只有本地那一条;无 turn_overlap。
    const LedgerCount count(*fx.ledger);
    CHECK(count.title_changed == 1);
    CHECK(count.refine_request_ids.size() == 1);
    CHECK(count.refine_usage == 1);
    CHECK(count.refine_output == 1);
    CHECK(count.turn_overlap == 0);
}
