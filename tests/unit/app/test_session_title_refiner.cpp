// 会话标题异步精炼器(实测问题 7)的单测:Start 不阻塞、TakeFinished
// 非阻塞收货、单飞不叠发、失败半截也出账、取消生效、代数原样带回、
// 析构不挂。假后端可控延迟,不发一个网络包。
// 通知时序缺陷单补:Ready 只读查询的真值表、失败结局也要醒、主线程见
// Ready 后收货数据全齐、IdleWakeCoordinator 挂 Ready 的唤醒源形制
// (running 不醒/finished 醒/收完不再醒)。
// 2026-09-22 报明单补:失败死因进 Outcome.error——超时(local_deadline
// 带预算数)、网络错(发送失败原样透传)、流内错、空回各归各位;超时
// 预算走 Inputs.timeout_secs 注入口,不为断言真等 30 秒。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"  // RequestPurpose(发车即起飞册:主/旁路请求账)
#include "agent/loop.hpp"          // RequestPreparedContext
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/session_title.hpp"  // kTitleRefineMaxTokens(单子预算钉)
#include "app/session_title_account.hpp"  // SessionTitleAccount(发车即起飞册)
#include "app/session_title_refiner.hpp"
#include "runtime/idle_wake.hpp"  // IdleWakeCoordinator(唤醒源形制钉)
#include "runtime/trajectory_session.hpp"  // TrajectorySessionLedger(真账本)
#include "workspace/identity.hpp"          // MakeFallbackIdentity(测试场身份)

namespace {

using lubancode::app::SessionTitleRefiner;

// 请求摘要:记进测试栈上的账本(backend 本体随线程闭包在 join 时销毁,
// 账不能记在它身上——join 之后读它就是 use-after-free)。
struct CapturedCall {
    std::string model;
    int max_tokens = -1;
    bool has_tools = false;
    std::string user_text;
};

// 假后端:延迟 delay_ms 后回一枚短标题(带 usage);cancel 非空且已被拉起
// 时回取消错误。请求摘要写给 calls 指的外部账本,供断言只喂首问截段。
struct FakeTitleBackend final : lubancode::api::Backend {
    int delay_ms = 0;
    bool honor_cancel = true;
    std::vector<CapturedCall>* calls = nullptr;  // 可空 = 不记

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        if (calls != nullptr) {
            CapturedCall call;
            call.model = request.model;
            if (request.max_tokens.has_value()) {
                call.max_tokens = *request.max_tokens;
            }
            call.has_tools = !request.tools.empty();
            for (const auto& message : request.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                        call.user_text += text->text;
                    }
                }
            }
            calls->push_back(std::move(call));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (honor_cancel && cancel != nullptr && cancel->load()) {
                return std::unexpected(
                    lubancode::api::Error{lubancode::api::ErrorKind::Cancelled, "cancelled", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        on_event(lubancode::api::TextDelta{"实现图书系统"});
        on_event(lubancode::api::ContentBlockDone{0});
        lubancode::api::Usage usage;
        usage.input_tokens = 492;
        usage.output_tokens = 10;
        on_event(lubancode::api::MessageDone{"end_turn", usage});
        return {};
    }
};

SessionTitleRefiner::Inputs MakeInputs(std::unique_ptr<lubancode::api::Backend> backend,
                                       std::uint64_t generation = 1) {
    SessionTitleRefiner::Inputs inputs;
    inputs.backend = std::move(backend);
    inputs.model = "cheap-m";
    inputs.effort = "low";
    inputs.first_query = "做一个图书管理系统,node 前端";
    inputs.generation = generation;
    return inputs;
}

std::unique_ptr<FakeTitleBackend> MakeRecordingBackend(std::vector<CapturedCall>* calls, int delay_ms) {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = delay_ms;
    backend->calls = calls;
    return backend;
}

// 有界等完工:最多等 wait_ms,轮询 TakeFinished(它自己绝不等待)。
std::optional<SessionTitleRefiner::Outcome> AwaitFinished(SessionTitleRefiner& refiner, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = refiner.TakeFinished()) {
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return refiner.TakeFinished();
}

}  // namespace

TEST_CASE("精炼器:Start 立即返回,结果非阻塞收货,usage 与代数带回") {
    std::vector<CapturedCall> calls;  // 账本住测试栈上:join 销毁闭包后仍可读
    SessionTitleRefiner refiner;
    const auto started_at = std::chrono::steady_clock::now();
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/300))));
    const auto start_elapsed = std::chrono::steady_clock::now() - started_at;
    CHECK(start_elapsed < std::chrono::milliseconds(200));  // 起飞不等网络
    CHECK(refiner.Busy());
    // 还没完工:收货给空,绝不等待。
    CHECK_FALSE(refiner.TakeFinished().has_value());

    const auto outcome = AwaitFinished(refiner, /*wait_ms=*/2000);
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);
    CHECK(outcome->title == "实现图书系统");
    CHECK(outcome->model == "cheap-m");
    CHECK(outcome->generation == 1);
    // usage 带回给调用方记 cheap 账(半截/成功一个口径)。
    CHECK(outcome->accounting.usage.input_tokens == 492);
    CHECK(outcome->accounting.usage.output_tokens == 10);
    CHECK(outcome->accounting.usage_reported);
    // 只喂首问:一次调用、输出上限收紧、不带工具、正文是首问截段。
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].max_tokens == lubancode::app::kTitleRefineMaxTokens);
    CHECK_FALSE(calls[0].has_tools);
    CHECK(calls[0].user_text.find("做一个图书管理系统") != std::string::npos);

    // 收货后复位:不忙了,可再起飞。
    CHECK_FALSE(refiner.Busy());
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/0), /*generation=*/2)));
    CHECK(AwaitFinished(refiner, 2000)->generation == 2);
    CHECK(calls.size() == 2);
}

TEST_CASE("精炼器:单飞——上一枚没收走之前不叠发") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 200;
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    CHECK_FALSE(refiner.Start(MakeInputs(std::make_unique<FakeTitleBackend>())));
    CHECK(AwaitFinished(refiner, 2000).has_value());
    // 结果待收也算忙:还没 TakeFinished 之前再 Start 照样拒。
    auto backend2 = std::make_unique<FakeTitleBackend>();
    backend2->delay_ms = 0;
    CHECK(refiner.Start(MakeInputs(std::move(backend2))));
    CHECK(refiner.Busy());
    CHECK_FALSE(refiner.Start(MakeInputs(std::make_unique<FakeTitleBackend>())));
    CHECK(AwaitFinished(refiner, 2000).has_value());
}

TEST_CASE("精炼器:失败带回 ok=false 与死因,账照出;本地标题由调用方保留") {
    // 网络错:发送失败原样透传(kind 保留),错误串进 Outcome.error。
    struct NetworkErrorBackend final : lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>*) override {
            return std::unexpected(lubancode::api::Error{
                lubancode::api::ErrorKind::Network, "connection refused", 0});
        }
    };
    {
        SessionTitleRefiner refiner;
        CHECK(refiner.Start(MakeInputs(std::make_unique<NetworkErrorBackend>())));
        const auto outcome = AwaitFinished(refiner, 2000);
        REQUIRE(outcome.has_value());
        CHECK_FALSE(outcome->ok);
        CHECK(outcome->title.empty());
        CHECK(outcome->error == "connection refused");  // 死因归位,报明行填 {0}
    }
    // 流内错(服务端 error 事件折 Api 分型):同样原样报明。
    struct ErrorBackend final : lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
            const std::atomic<bool>*) override {
            on_event(lubancode::api::TextDelta{"半"});
            on_event(lubancode::api::StreamError{"cheap 不可用"});
            return {};
        }
    };
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::make_unique<ErrorBackend>())));
    const auto outcome = AwaitFinished(refiner, 2000);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);
    CHECK(outcome->title.empty());
    CHECK(outcome->error == "cheap 不可用");
}

TEST_CASE("精炼器:RequestCancel 打断在飞请求,不重试") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;  // 不取消就跑不完
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    refiner.RequestCancel();  // 人工 /title 抢先那一下
    const auto outcome = AwaitFinished(refiner, 3000);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);  // 取消收场:调用方保留本地标题
}

TEST_CASE("精炼器:看门狗到点打断慢后端,死因带预算数归 local_deadline") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;  // 不打断就跑不完的慢后端
    SessionTitleRefiner::Inputs inputs = MakeInputs(std::move(backend));
    inputs.timeout_secs = 1;  // 注入口打真超时:不为断言真等 30 秒常数
    SessionTitleRefiner refiner;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(refiner.Start(std::move(inputs)));
    const auto outcome = AwaitFinished(refiner, 8'000);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);  // 看门狗拉取消:调用方保留本地标题
    // 死因归位:超时归 local_deadline,文案带预算数(采样层统一话),
    // 报明行拿它填 {0}——超时/网络错/空回各报各的。
    CHECK(outcome->error == "采样超过 1 秒,被本地超时预算停止");
    CHECK(outcome->accounting.duration_ms >= 1000);  // 耗时随死因报明
    // 短预算 + 收尾余量,不许拖到等待窗上限。
    CHECK(elapsed < std::chrono::seconds(6));
}

TEST_CASE("精炼器:析构时在飞也不挂——取消 + 有界收尾 + detach 放行") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;
    backend->honor_cancel = false;  // 最坏现场:取消旗拉了也装死
    const auto t0 = std::chrono::steady_clock::now();
    {
        SessionTitleRefiner refiner;
        CHECK(refiner.Start(MakeInputs(std::move(backend))));
    }  // 析构:取消 → 有界等待(7 秒窗)→ detach 放行,不 terminate
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < std::chrono::seconds(15));
    CHECK(true);  // 到这里就是过了:析构未挂死、未 terminate
}

// 有界等 Ready 翻真:模拟空闲 composer 的 100ms 拍轮询(只问 Ready,
// 绝不 Take——收货是主循环收货点的事)。
bool AwaitReady(SessionTitleRefiner& refiner, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (refiner.Ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return refiner.Ready();
}

TEST_CASE("Ready 真值表:未启动/运行中/完成待取/取走后——Busy 当不了唤醒条件") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 200;
    SessionTitleRefiner refiner;
    // 未启动:两只都 false,空闲拍不空醒。
    CHECK_FALSE(refiner.Ready());
    CHECK_FALSE(refiner.Busy());
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    // 运行中:Ready 仍 false——起飞那一刻不醒,完工才醒。
    CHECK(refiner.Busy());
    CHECK_FALSE(refiner.Ready());
    REQUIRE(AwaitReady(refiner, 2000));
    // 完成待取:Ready 翻真;Busy 也仍 true(槽还占着,单飞防叠发)——
    // 这正是 Busy 当不了唤醒条件的原因:它起飞即真,会空转到收货。
    CHECK(refiner.Ready());
    CHECK(refiner.Busy());
    // 取走后:双双翻假,槽复位可再起飞。
    REQUIRE(refiner.TakeFinished().has_value());
    CHECK_FALSE(refiner.Ready());
    CHECK_FALSE(refiner.Busy());
}

TEST_CASE("Ready 并发可见性:主线程见 Ready 后 Take——标题/usage/代数全齐") {
    std::vector<CapturedCall> calls;
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/150),
                                   /*generation=*/7)));
    // worker 写完 outcome 后才立 done(Start 尾段的手工次序):主线程一旦
    // 见 Ready,Take 拿到的必是整套结果,没有半截可见。
    REQUIRE(AwaitReady(refiner, 2000));
    const auto outcome = refiner.TakeFinished();
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);
    CHECK(outcome->title == "实现图书系统");
    CHECK(outcome->model == "cheap-m");
    CHECK(outcome->generation == 7);
    CHECK(outcome->accounting.usage.input_tokens == 492);
    CHECK(outcome->accounting.usage.output_tokens == 10);
    CHECK(outcome->accounting.usage_reported);
}

TEST_CASE("失败结局也 Ready——失败也要叫醒来收账(usage 是真花的)") {
    // 空回:流正常收场但一个字没吐,usage 照出——收货点记账后安静弃标题。
    struct EmptyTextBackend final : lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
            const std::atomic<bool>*) override {
            lubancode::api::Usage usage;
            usage.input_tokens = 492;
            usage.output_tokens = 10;
            on_event(lubancode::api::MessageDone{"end_turn", usage});
            return {};
        }
    };
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::make_unique<EmptyTextBackend>())));
    REQUIRE(AwaitReady(refiner, 2000));  // 失败结局同样完工待收:不醒就丢账
    const auto outcome = refiner.TakeFinished();
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);
    CHECK(outcome->title.empty());
    CHECK(outcome->error == "标题为空");  // 空回死因归位:清洗后一个字不剩
    CHECK(outcome->accounting.usage.input_tokens == 492);
    CHECK(outcome->accounting.usage_reported);
}

TEST_CASE("唤醒源形制(装配同款):running 不醒、finished 醒、收完不再醒") {
    lubancode::runtime::IdleWakeCoordinator wakes;
    SessionTitleRefiner refiner;
    // 与 interactive_session_assembly 的挂法同款:ready 只问 Ready()。
    const auto token = wakes.AddSource("session_title", [&refiner]() { return refiner.Ready(); });
    CHECK(token.valid());
    CHECK(wakes.SourceNames() == std::vector<std::string>{"session_title"});
    CHECK_FALSE(wakes.AnyReady());  // 未启动:不醒
    std::vector<CapturedCall> calls;
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/150))));
    CHECK(refiner.Busy());
    CHECK_FALSE(wakes.AnyReady());  // 运行中:不空醒(Ready 才是唤醒条件)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!wakes.AnyReady() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(wakes.AnyReady());  // 完工:叫醒主循环收货
    REQUIRE(refiner.TakeFinished().has_value());
    CHECK_FALSE(wakes.AnyReady());  // 收完:不再醒,不空转
}

// ---------------------------------------------------------------------------
// 触发时机提前单(发车即起飞):v3 场首问主回合 BeginTurn 铸号后立即起飞,
// worker 线程的旁路桥与主线程的主 turn 账并行落同一本 v3 流——V3Writer
// 提交全程持锁,盘上串行;title.requested/extracted/applied 三枚事实事件
// 保真(选 a 路:桥材料随身带,起飞即落,不延迟不弃账),主 turn 的请求账
// 一枚不少。旧注释"回合里发必撞车"是 v2 状态机(一 stream 一 open turn)
// 的机理,v3 无轮账互斥,此处用真账本定谳。
// ---------------------------------------------------------------------------
TEST_CASE("发车即起飞: 主 turn open 时起飞,v3 账并行不撞,三枚事实事件保真") {
    // v3 显式开(与本仓其余案同一纪律:显式置值,ctest 注入不猜)。
#ifdef _WIN32
    _putenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1");
#else
    setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1", 1);
#endif
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("lubancode-title-kickoff-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(dir / "repo", ec);
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = dir / "workspaces";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir / "repo");
    options.lubancode_version = "test";
    auto opened = lubancode::runtime::TrajectorySessionLedger::Open(options);
    REQUIRE(opened.has_value());
    auto ledger = std::move(*opened);
    REQUIRE(ledger.v3_main_writer() != nullptr);  // 前提:确是 v3 场
    const std::string session_id = ledger.session_id();
    const std::filesystem::path stream = ledger.session_dir() / (session_id + ".jsonl");

    // 首问主回合:BeginTurn 铸号 + 输入落账(此刻是"发车即起飞"的起飞点,
    // 主模型请求尚未发出)。
    auto turn_bridge = ledger.NewTurnBridge({"fake", "anthropic", "terminal"});
    REQUIRE(turn_bridge != nullptr);
    turn_bridge->BeginTurn("turn-1", "external_user");
    lubancode::api::Message user_message;
    user_message.role = lubancode::api::Role::User;
    user_message.content.push_back(lubancode::api::TextBlock{"做一个图书管理系统"});
    turn_bridge->RecordInput(user_message);

    // 标题账房 + 立即起飞(生产同款 Inputs:trajectory 递 v3 账本,worker
    // 线程自铸旁路桥)。Start 立即回——起飞不等网络,不挡主回合。
    std::string title;
    lubancode::app::SessionTitleAccount account(title, &ledger);
    SessionTitleRefiner::Inputs inputs = MakeInputs(MakeRecordingBackend(nullptr, /*delay_ms=*/80));
    inputs.generation = account.generation();
    inputs.trajectory = &ledger;
    inputs.trajectory_wire = "anthropic";
    inputs.provider = "fake";
    SessionTitleRefiner& refiner = account.refiner();
    CHECK(refiner.Start(std::move(inputs)));
    CHECK(refiner.Busy());
    // 起飞事实(title.requested)在真起飞后记——与 controller 同款次序。
    account.NoteTitleGenerationStarted("cheap-m", "fake");

    // 主 turn 的模型请求此刻才发(与标题采样真并行):worker 在写旁路账,
    // 主线程写主 turn 账,同一本流,V3Writer 的锁管串行。
    std::string main_request_id;
    {
        lubancode::agent::RequestPreparedContext ctx;
        ctx.purpose = lubancode::accounting::RequestPurpose::MainTurn;
        lubancode::api::Request request;
        request.model = "main-m";
        request.system = "SYSTEM-X";
        request.messages.push_back(user_message);
        main_request_id = turn_bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(main_request_id.empty());
        REQUIRE(turn_bridge->OnRequestSent(main_request_id));
        lubancode::api::Usage usage;
        usage.input_tokens = 900;
        usage.output_tokens = 24;
        turn_bridge->OnUsageRecorded(main_request_id, usage, true, "resp-1");
        lubancode::api::Message assistant;
        assistant.role = lubancode::api::Role::Assistant;
        assistant.content.push_back(lubancode::api::TextBlock{"办完了。"});
        REQUIRE(turn_bridge->OnOutputCompleted(main_request_id, assistant, "end_turn", "resp-1"));
    }

    // 等标题采样完工(模拟空闲唤醒拍只问 Ready),收货、记提取与采用。
    REQUIRE(AwaitReady(refiner, 2000));
    const auto outcome = refiner.TakeFinished();
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);
    CHECK(outcome->title == "实现图书系统");
    CHECK(account.AdoptRefined(*outcome) == lubancode::app::SessionTitleAccount::AdoptResult::Adopted);
    CHECK(title == "实现图书系统");
    // 主回合收口。
    turn_bridge->EndTurn(true, false, "");

    // 数账(v3 流逐行):三枚事实事件同号贯穿;title_refine 的消息归首问
    // 主回合 turn-1;主 turn 的 prepared 一枚不少。
    std::ifstream file(stream, std::ios::binary);
    REQUIRE(file.is_open());
    int requested = 0;
    int extracted = 0;
    int applied_generated = 0;
    int applied_local = 0;
    int title_users_on_turn1 = 0;
    int main_prepared = 0;
    std::string generation_id;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const nlohmann::json row = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (row.is_discarded()) {
            continue;
        }
        const std::string kind = row.value("kind", std::string());
        if (kind == "title.requested") {
            ++requested;
            generation_id = row.value("titleGenerationId", std::string());
        } else if (kind == "title.extracted") {
            ++extracted;
            CHECK(row.value("titleGenerationId", std::string()) == generation_id);
        } else if (kind == "session.title.applied") {
            if (row.at("payload").value("source", std::string()) == "generated") {
                ++applied_generated;
                CHECK(row.value("titleGenerationId", std::string()) == generation_id);
            } else {
                ++applied_local;
            }
        } else if (kind == "model.request.prepared") {
            const std::string purpose =
                row.value("payload", nlohmann::json::object()).value("purpose", std::string());
            if (purpose == "title_refine") {
                // title_refine 的请求归首问主回合(a 路:不延迟,起飞即落)。
                CHECK(row.value("turnId", std::string()) == "turn-1");
            } else {
                ++main_prepared;
            }
        } else if (row.value("type", std::string()) == "message" &&
                   row.value("purpose", std::string()) == "session_title" &&
                   row.at("message").value("role", std::string()) == "user") {
            ++title_users_on_turn1;
            CHECK(row.value("turnId", std::string()) == "turn-1");
        }
    }
    CHECK(requested == 1);
    CHECK(extracted == 1);
    CHECK(applied_generated == 1);
    CHECK(applied_local == 0);  // 本案直接走生成流,本地标题不在场
    CHECK(main_prepared == 1);  // 主 turn 的请求账一枚不少
    CHECK(title_users_on_turn1 == 1);

    std::filesystem::remove_all(dir, ec);
#ifdef _WIN32
    _putenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=");
#else
    unsetenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
#endif
}
