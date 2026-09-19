// 只读并行单 P3 宿主验收·Gateway(无人值守 headless 执行器):自动任务/
// 渠道消息一轮的 Agent 档案从 HeadlessExecutor::Options 拿批次执行策略
//(生产装配 gateway_launch/assistant_tasks 从 config 折好递进)。钉:
//   1. ParallelRead + 并发 2:任务批次里两枚读真并发(进门闸);
//   2. 缺省档(Exclusive):同款批次串行——不声明不并行;
//   3. 执行收口:V3 场开出、reply selection 落文件(既有合同不缺环)。
// 渠道回合的逐轮收窄闸(per_turn_tools)在场时整批回退串行,是 loop 级
// 合同(P2 册"Hook 在场"案);headless 侧不另测。断先后用进门闸,不靠
// sleep 猜并发。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "agent/tool_batch_schedule.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/path_utils.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

constexpr auto kGateWait = std::chrono::seconds(3);

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;

    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>& on_event,
                                                const std::atomic<bool>* = nullptr) override {
        if (index_ >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[index_]) {
            on_event(event);
        }
        ++index_;
        return {};
    }

private:
    std::size_t index_ = 0;
};

std::vector<api::StreamEvent> BatchScript(
    const std::vector<std::tuple<std::string, std::string, std::string>>& calls) {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "test-model"});
    for (std::size_t i = 0; i < calls.size(); ++i) {
        events.push_back(
            api::ToolUseStart{static_cast<int>(i), std::get<0>(calls[i]), std::get<1>(calls[i])});
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), std::get<2>(calls[i])});
        events.push_back(api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return events;
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 进门闸 + 峰值账。
struct ConcurrencyGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    std::size_t enter_target = 1;
    int gate_timeouts = 0;
};

struct ToolCounters {
    std::atomic<int> executions{0};
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
};

class GatedRead : public tools::Tool {
public:
    GatedRead(ConcurrencyGate& gate, ToolCounters& counters) : gate_(&gate), counters_(&counters) {}
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "headless 读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
        ++counters_->executions;
        const int now_active = counters_->active.fetch_add(1) + 1;
        int observed = counters_->peak.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !counters_->peak.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
            if (gate_->enter_target > 1 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->entered >= gate_->enter_target; })) {
                ++gate_->gate_timeouts;
            }
        }
        counters_->active.fetch_sub(1);
        return {"headless read ok", false};
    }

private:
    ConcurrencyGate* gate_;
    ToolCounters* counters_;
};

// 一套 headless 现场根。
struct HeadlessFixture {
    std::filesystem::path root;
    std::filesystem::path replies_dir;
    EnvGuard v3pin{"LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1"};

    explicit HeadlessFixture(const char* tag)
        : root(std::filesystem::temp_directory_path() /
               ("luban-p3-headless-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        replies_dir = root / "delivery" / "replies";
    }
};

runtime::HeadlessExecutor::Options BaseOptions(const HeadlessFixture& fixture) {
    runtime::HeadlessExecutor::Options options;
    options.workspaces_root = fixture.root / "workspaces";
    options.workspace_root = fixture.root;
    options.workspace_identity = workspace::MakeFallbackIdentity(fixture.root);
    options.cwd_utf8 = tools::PathToUtf8(fixture.root);
    options.lubancode_version = "0.26.238-test";
    options.wire_name = "test-wire";
    options.model = "test-model";
    options.replies_dir = fixture.replies_dir;
    return options;
}

}  // namespace

TEST_CASE("宿主(Gateway/headless):ParallelRead 任务批次两读真并发,reply 落文件") {
    HeadlessFixture fixture("parallel");
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    ToolCounters counters;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<GatedRead>(gate, counters));

    backend.scripts = {
        BatchScript({{"u0", "read_file", "{}"}, {"u1", "read_file", "{}"}}),
        TextScript("网关收工:两枚都读完了"),
    };
    runtime::HeadlessExecutor::Options options = BaseOptions(fixture);
    options.tool_batch_strategy = agent::ToolBatchStrategy::ParallelRead;
    options.parallel_read_concurrency = 2;
    runtime::HeadlessExecutor executor(backend, registry, std::move(options));

    runtime::HeadlessWorkBinding binding;
    binding.work_id = "work-p3-parallel";
    binding.source_kind = "automation";
    binding.source_id = "job-p3";
    binding.owner_epoch = "p3-epoch";
    const auto result = executor.Execute("读两枚", binding, {}, nullptr);

    REQUIRE(result.ok);
    CHECK(counters.executions.load() == 2);
    CHECK(gate.gate_timeouts == 0);          // 两枚同进执行体:真并发
    CHECK(counters.peak.load() == 2);        // 峰值恰 2(上限 2 生效)
    CHECK(result.reply_text.find("网关收工") != std::string::npos);
    CHECK_FALSE(result.session_id.empty());
    // reply 原件落盘(既有交付合同不缺环)。
    std::error_code ec;
    CHECK(std::filesystem::exists(fixture.replies_dir / (result.selection_id + ".txt"), ec));
}

TEST_CASE("宿主(Gateway/headless):缺省档(Exclusive)同款批次串行——不声明不并行") {
    HeadlessFixture fixture("serial");
    FakeBackend backend;
    ConcurrencyGate gate;  // enter_target=1:不等
    ToolCounters counters;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<GatedRead>(gate, counters));

    backend.scripts = {
        BatchScript({{"u0", "read_file", "{}"}, {"u1", "read_file", "{}"}}),
        TextScript("串行收工"),
    };
    runtime::HeadlessExecutor::Options options = BaseOptions(fixture);  // 策略缺省 Exclusive
    runtime::HeadlessExecutor executor(backend, registry, std::move(options));

    runtime::HeadlessWorkBinding binding;
    binding.work_id = "work-p3-serial";
    binding.source_kind = "automation";
    binding.source_id = "job-p3";
    binding.owner_epoch = "p3-epoch";
    const auto result = executor.Execute("读两枚", binding, {}, nullptr);

    REQUIRE(result.ok);
    CHECK(counters.executions.load() == 2);
    CHECK(counters.peak.load() == 1);  // 峰值 1 = 逐枚串跑
    CHECK(result.reply_text.find("串行收工") != std::string::npos);
}
