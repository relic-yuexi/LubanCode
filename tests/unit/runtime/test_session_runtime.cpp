// SessionRuntime 单测(显示系统剥离单第六步:拆 SessionRuntime)。
//
// 钉的是搬出来那半账本的语义(原文自 InteractiveSession 的 EnsureSessionBegun
// /PersistNewMessages,一字不改,只是不再自己打印):
//   1. 建档:首条文本做 slug、meta 填账、建档前挂起的标题补事件行;
//   2. 增量落盘:只追加 persisted_count 之后的消息;换短历史后基线钳回;
//   3. 失败路径:Begin 失败置 broken,后续不再撞;
//   4. 权限账与 thread 身份:IdAuthority 发号、always_allowed 直通;
//   5. MakeTurnAdapter:同一 thread_id、同一发号局,事件落 AttachSink。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_runtime.hpp"
#include "tools/registry.hpp"

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

api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

class RecordingSink final : public rt::EventSink {
public:
    void Emit(const rt::ServerEvent& event) override { events.push_back(event); }
    std::vector<rt::ServerEvent> events;
};

}  // namespace

TEST_CASE("SessionRuntime:建档用首句做 slug,挂起标题补事件行") {
    TempSessionsDir dir;
    rt::SessionRuntime runtime({dir.path(), "anthropic", "20260823-120000"});
    CHECK(runtime.store().session_id().empty());

    runtime.title() = "起个名字";
    runtime.title_pending() = true;
    const auto begun = runtime.EnsureBegun("第一句话", "test-model", "/tmp");
    CHECK(begun == rt::SessionBeginResult::Begun);
    CHECK(runtime.store().active());
    CHECK(runtime.store().session_id().find("第一句话") != std::string::npos);
    CHECK_FALSE(runtime.title_pending());

    // 建过档再 Ensure:Active,不重复建。
    CHECK(runtime.EnsureBegun("另一句", "test-model", "/tmp") == rt::SessionBeginResult::Active);
    CHECK(runtime.meta().model == "test-model");
    CHECK(runtime.meta().wire == "anthropic");
}

TEST_CASE("SessionRuntime:增量落盘只追新消息,基线只收不放") {
    TempSessionsDir dir;
    rt::SessionRuntime runtime({dir.path(), "anthropic", "20260823-120001"});
    std::vector<api::Message> history = {UserText("问"), AssistantText("答")};
    CHECK(runtime.PersistNew(history, "m", "/tmp") == rt::SessionPersistResult::Appended);
    CHECK(runtime.persisted_count() == 2);

    // 没新消息:Nothing。
    CHECK(runtime.PersistNew(history, "m", "/tmp") == rt::SessionPersistResult::Nothing);

    history.push_back(UserText("再问"));
    CHECK(runtime.PersistNew(history, "m", "/tmp") == rt::SessionPersistResult::Appended);
    CHECK(runtime.persisted_count() == 3);

    // 压缩换史(/compact 语义):基线钳到新长度,旧账不重写。
    history.resize(1);
    runtime.ClampPersisted(history.size());
    CHECK(runtime.persisted_count() == 1);
}

TEST_CASE("SessionRuntime:Begin 失败置 broken,后续落盘安静跳过") {
    // 目录指到一个文件占着的路径:create/Mkdir 失败,Begin 报 false。
    TempSessionsDir dir;
    const std::string blocker = dir.path() + "/blocker";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "x";
    }
    rt::SessionRuntime runtime({blocker, "anthropic", "20260823-120002"});
    const auto begun = runtime.EnsureBegun("问一句", "m", "/tmp");
    CHECK(begun == rt::SessionBeginResult::Failed);
    CHECK(runtime.store_broken());
    // broken 之后:不撞第二次,Nothing 收场。
    std::vector<api::Message> history = {UserText("问")};
    CHECK(runtime.PersistNew(history, "m", "/tmp") == rt::SessionPersistResult::Nothing);
}

TEST_CASE("SessionRuntime:sessions_dir 空 = 不落盘") {
    rt::SessionRuntime runtime({"", "anthropic", "20260823-120003"});
    std::vector<api::Message> history = {UserText("问")};
    CHECK(runtime.PersistNew(history, "m", "/tmp") == rt::SessionPersistResult::Nothing);
    CHECK(runtime.EnsureBegun("问", "m", "/tmp") == rt::SessionBeginResult::Disabled);
}

TEST_CASE("SessionRuntime:thread 身份、发号局与权限账") {
    rt::SessionRuntime runtime({"", "anthropic", "ts"});
    CHECK(runtime.thread_id().rfind("thread-", 0) == 0);
    const std::uint64_t before = runtime.ids().items_issued();
    (void)runtime.ids().NextItemId();
    CHECK(runtime.ids().items_issued() == before + 1);

    runtime.always_allowed().insert("read_file");
    CHECK(runtime.always_allowed().count("read_file") == 1);
}

TEST_CASE("SessionRuntime:MakeTurnAdapter 共用 thread_id 与发号局,事件落挂的 sink") {
    rt::SessionRuntime runtime({"", "anthropic", "ts"});
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
