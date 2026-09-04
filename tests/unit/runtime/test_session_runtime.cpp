// SessionRuntime 单测(显示系统剥离单第六步:拆 SessionRuntime)。
//
// P0-2(Trajectory 升为唯一 Session):旧 SessionStore 建档/轮末补抄路停用
// ——EnsureBegun 恒 Disabled、PersistNew 恒 Nothing,会话真账在 ctor 里恒开
// 的 TrajectorySessionLedger。旧路的建档语义(首句 slug、标题补行、增量
// 落盘、broken 账)随旧档退役,由 P0-5 迁移器/P0-6 删码收口;这里钉的是
// 新语义 + 与旧 Store 无关的部分:
//   1. ledger 恒开:临时根下出 workspace/session 目录与 main.jsonl;
//   2. EnsureBegun/PersistNew 的退役语义(Disabled/Nothing);
//   3. 权限账与 thread 身份:IdAuthority 发号、always_allowed 直通;
//   4. MakeTurnAdapter:同一 thread_id、同一发号局,事件落 AttachSink。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_runtime.hpp"
#include "tools/registry.hpp"
#include "workspace/identity.hpp"

namespace rt = lubancode::runtime;
using namespace lubancode;

namespace {

// 临时会话目录 RAII:先关柄再删、remove_all 用 error_code 形态。
class TempSessionsDir {
public:
    TempSessionsDir() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("lubancode-session-runtime-" + std::to_string(counter_++)))
                    .string();
        std::filesystem::create_directories(path_);
    }
    ~TempSessionsDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    static inline int counter_ = 0;
    std::string path_;
};

class RecordingSink final : public rt::EventSink {
public:
    void Emit(const rt::ServerEvent& event) override { events.push_back(event); }
    std::vector<rt::ServerEvent> events;
};

}  // namespace

TEST_CASE("SessionRuntime:账本恒开,workspace/session 目录在临时根下") {
    TempSessionsDir dir;
    rt::SessionRuntime::Options options;
    options.trajectory_workspaces_root = dir.path() + "/workspaces";
    std::error_code ec;
    std::filesystem::create_directories(dir.path() + "/repo", ec);
    options.trajectory_workspace_identity = workspace::MakeFallbackIdentity(
        std::filesystem::path(dir.path()) / "repo");
    options.lubancode_version = "test";
    rt::SessionRuntime runtime(std::move(options));
    REQUIRE(runtime.trajectory() != nullptr);
    CHECK(std::filesystem::exists(runtime.trajectory()->session_dir() / "main.jsonl"));
    CHECK_FALSE(runtime.trajectory()->session_id().empty());
    CHECK(runtime.trajectory_open_error().empty());
}

// (P0-6:旧档建档/轮末补抄的退役语义用例已删——EnsureBegun/PersistNew
// 本体随 SessionStore 删除,新语义是"这些方法不存在"。)

TEST_CASE("SessionRuntime:thread 身份、发号局与权限账") {
    rt::SessionRuntime runtime({"anthropic", "ts"});
    CHECK(runtime.thread_id().rfind("thread-", 0) == 0);
    const std::uint64_t before = runtime.ids().items_issued();
    (void)runtime.ids().NextItemId();
    CHECK(runtime.ids().items_issued() == before + 1);

    runtime.always_allowed().insert("read_file");
    CHECK(runtime.always_allowed().count("read_file") == 1);
}

TEST_CASE("SessionRuntime:MakeTurnAdapter 共用 thread_id 与发号局,事件落挂的 sink") {
    rt::SessionRuntime runtime({"anthropic", "ts"});
    RecordingSink sink;
    runtime.AttachSink(&sink);

    auto adapter = runtime.MakeTurnAdapter();
    adapter.Attach([&](const rt::ServerEvent& event) { sink.Emit(event); });
    const std::string turn_id = adapter.Start();
    CHECK(turn_id.rfind("turn-", 0) == 0);
    // Finish 走 sink:TurnCompleted 一枚,seq 单调。
    adapter.Finish(rt::Outcome::Succeeded);
    REQUIRE(sink.events.size() == 2);
    CHECK(sink.events[0].kind == rt::ServerEventKind::TurnStarted);
    CHECK(sink.events[0].envelope.thread_id == runtime.thread_id());
    CHECK(sink.events[0].turn_id == turn_id);
    CHECK(sink.events[1].kind == rt::ServerEventKind::TurnCompleted);
    CHECK(sink.events[1].envelope.seq > sink.events[0].envelope.seq);
}
