// QQ 接入单 Q6 总装册:ChannelInteractionBroker 单测 + 假渠道 e2e——
// "QQ 按钮批准一次工具调用"的纵向链(todo §十二)。
//
// e2e 验收对齐:
//   1) 工具触发确认闸 → 审批卡经 manager 发出(keyboard payload 官方形状:
//      两枚 action.type=1 回调按钮,data=qai:<token>:<1|0>,permission
//      指定配对 sender)→ 本人按"允许" → 工具执行 → 模型续跑终答;
//   2) 按"拒绝" → 工具跳过(is_error tool_result),不执行;
//   3) 超时 → 默认拒绝(不默认放行);
//   4) 他人代按 → NotAuthorized(ack code=4),请求仍挂,本人还能按;
//   5) 重复回调 → 幂等(ack code=3,只返回已处理);
//   6) 审批等待期间新来信照常收账(执行线程 ≠ 事件泵线程,§12.2 第九行);
//   7) 审批请求/结果落 V3 事件账(channel.approval.requested/resolved)。
// 真平台按钮触达/回调真实形状归 Q3 真机未验;本地钉的是宿主裁决合同。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/manager.hpp"
#include "channel/types.hpp"
#include "fake_channel_sidecar.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/channel_interaction_broker.hpp"
#include "runtime/channel_work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

using namespace lubancode;
using lubancode::test_support::FakeChannelSidecar;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name, value, 1);
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

void CountCall(const std::filesystem::path& counter, const std::string& who) {
    std::error_code ec;
    std::filesystem::create_directories(counter.parent_path(), ec);
    std::ofstream stream(counter, std::ios::binary | std::ios::app);
    stream << who << "\n";
}

std::size_t CountOf(const std::filesystem::path& counter, const std::string& who) {
    std::ifstream stream(counter, std::ios::binary);
    std::string line;
    std::size_t count = 0;
    while (std::getline(stream, line)) {
        if (line == who) ++count;
    }
    return count;
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

class ScriptedBackend : public api::Backend {
public:
    ScriptedBackend(std::filesystem::path counter,
                    std::vector<std::vector<api::StreamEvent>> scripts)
        : counter_(std::move(counter)), scripts_(std::move(scripts)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        CountCall(counter_, "model");
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

private:
    std::filesystem::path counter_;
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::size_t calls_ = 0;
};

// 须确认工具:执行计数落盘(批没批,看调用数)。
class ConfirmableTool : public tools::Tool {
public:
    ConfirmableTool(std::filesystem::path counter, std::string name)
        : counter_(std::move(counter)), name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "须确认测试工具"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        CountCall(counter_, "tool");
        return tools::Tool::Result{"工具已执行。", false};
    }

private:
    std::filesystem::path counter_;
    std::string name_;
};

class FakeTransport final : public channel::ChannelBridgeTransport {
public:
    explicit FakeTransport(FakeChannelSidecar& sidecar) : sidecar_(sidecar) {}
    void WriteToSidecar(const std::byte* data, std::size_t size) override {
        sidecar_.FeedFromHost(data, size);
    }
    std::vector<std::byte> DrainFromSidecar() override { return sidecar_.DrainToHost(); }

private:
    FakeChannelSidecar& sidecar_;
};

channel::ChannelInboundEvent MakeDmAt(const std::string& delivery_id,
                                      const std::string& provider_event_id,
                                      const std::string& conversation, const std::string& text,
                                      const std::string& message_id, std::int64_t received_at_ms) {
    channel::ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = received_at_ms;
    event.provider_at_ms = received_at_ms;
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = conversation;
    event.sender.id = "sender-" + conversation;
    event.sender.display_name = "用户";
    event.message_id = message_id;
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// 纵向装配:manager(真 Bridge 帧,假 sidecar)+ 渠道泵(真,审批带 +
// 专用 turn 工作线程——生产同款异步形态)+ broker(真)。
struct Q6Fixture {
    std::filesystem::path root;
    std::filesystem::path workspaces_root;
    std::filesystem::path channels_root;
    std::filesystem::path ws_root;
    std::filesystem::path counter_file;
    std::int64_t now = 1724700000000;
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::int64_t approval_timeout_ms = 60'000;
    std::string allow_tools;  // 空 = 不设 allow(只设 approve 带)

    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<ScriptedBackend> backend;
    std::unique_ptr<channel::ChannelManager> manager;
    tools::ToolRegistry registry;  // 须活过泵(工作线程执行时用)
    std::shared_ptr<runtime::ChannelInteractionBroker> broker =
        std::make_shared<runtime::ChannelInteractionBroker>();
    std::optional<gateway::DurableReplyOutbox> outbox;
    std::optional<runtime::ChannelWorkPump> pump;
    workspace::WorkspaceIdentity identity;

    explicit Q6Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-q6-approval-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        workspaces_root = root / "workspaces";
        channels_root = root / "channels";
        ws_root = root / "ws";
        counter_file = root / "calls.log";
        std::filesystem::create_directories(ws_root, ec);

        channel::ChannelManagerOptions manager_options;
        manager_options.state_root = channels_root;
        manager_options.now_ms = [this] { return now; };
        manager_options.alive_checker = [](unsigned long) { return true; };
        manager = std::make_unique<channel::ChannelManager>(std::move(manager_options));

        channel::ChannelAccountUserConfig config;
        config.enabled = true;
        config.transport = "websocket";
        config.secret_env = "QQBOT_SECRET";
        config.dm_policy = channel::DmPolicy::Open;
        // 工具上限:须确认工具不在 allow(不走预授权),在 approve(可申请
        // 远端审批)——§12.2 第三行的"可申请审批带"。
        config.tools.allow = std::vector<std::string>{};
        config.tools.approve = std::vector<std::string>{"guarded_write"};
        REQUIRE(manager->AddAccount("qqbot", "main", config, &transport).status ==
                channel::ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state ==
                channel::ChannelAccountState::Running);
        identity = workspace::MakeFallbackIdentity(ws_root);
    }

    bool OpenPump() {
        registry.Register(std::make_unique<ConfirmableTool>(counter_file, "guarded_write"));
        backend = std::make_unique<ScriptedBackend>(counter_file, scripts);

        gateway::DurableReplyOutbox::Paths outbox_paths;
        outbox_paths.log_file = root / "delivery" / "outbox.jsonl";
        outbox_paths.replies_dir = root / "delivery" / "replies";
        outbox_paths.published_dir = root / "delivery" / "out";
        const auto outbox_open = gateway::DurableReplyOutbox::Open(&outbox.emplace(), outbox_paths);
        REQUIRE_MESSAGE(outbox_open.ok, outbox_open.error);

        runtime::ChannelWorkPump::Options work_options;
        work_options.manager = manager.get();
        work_options.outbox = &*outbox;
        work_options.channels_state_root = channels_root;
        work_options.workspaces_root = workspaces_root;
        work_options.workspace_identity = identity;
        work_options.cwd_utf8 = tools::PathToUtf8(root);
        work_options.lubancode_version = "0.26.267-test";
        work_options.wire_name = "test-wire";
        work_options.model = "test-model";
        work_options.tools = channel::ToolRoutePolicy{};  // 会话级基线空(逐轮路由冻结)
        work_options.max_steps_per_turn = 8;
        work_options.interaction_broker = broker;
        work_options.approval_timeout_ms = approval_timeout_ms;
        // 生产同款异步形态:turn 在工作线程等按钮,tick 线程收按钮。
        work_options.channel_turn_workers = 1;
        pump.emplace();
        const auto open =
            runtime::ChannelWorkPump::Open(&*pump, *backend, registry, std::move(work_options));
        REQUIRE_MESSAGE(open.ok, open.error);
        if (!open.ok) {
            return false;
        }
        pump->set_owner_epoch("q6-test-epoch");
        return true;
    }

    void Tick() {
        manager->Pump("qqbot", "main");
        REQUIRE(pump->TickOnce(now));
        now += 100;
    }

    void EmitAndIngest(const channel::ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");
    }

    // 真实时钟轮询:审批等待是真 deadline(steady_clock),测试真等。
    bool WaitUntil(const std::function<bool()>& done, int timeout_ms = 8000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) {
                return true;
            }
            Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done();
    }

    // 找审批卡 send(带 keyboard 的那条);找不到给 nullptr。
    const FakeChannelSidecar::RecordedSend* ApprovalCardSend() {
        for (const auto& send : sidecar.sent_messages()) {
            if (send.params.contains("keyboard")) {
                return &send;
            }
        }
        return nullptr;
    }

    // 从审批卡键盘里取按钮 data(action.data)。
    std::string ButtonDataOf(const FakeChannelSidecar::RecordedSend& card, const char* button_id) {
        const auto& buttons = card.params.at("keyboard").at("content").at("rows")[0].at("buttons");
        for (const auto& button : buttons) {
            if (button.at("id") == button_id) {
                return button.at("action").at("data").get<std::string>();
            }
        }
        return std::string();
    }

    void PressButton(const std::string& button_data, const std::string& interaction_id,
                     const std::string& operator_openid) {
        nlohmann::json params = nlohmann::json::object();
        params["interactionId"] = interaction_id;
        params["type"] = 11;
        params["scene"] = "c2c";
        params["chatType"] = 2;
        params["userOpenid"] = operator_openid;
        params["buttonData"] = button_data;
        params["deliveryId"] = "qq-inter-" + interaction_id;
        sidecar.EmitInteractionNotification(params);
        manager->Pump("qqbot", "main");
    }

    std::optional<gateway::ReplyOutboxItem> ChatItem() {
        for (const auto& item : outbox->ListItems()) {
            if (item.source_ref.rfind("ingress:", 0) == 0) {
                return item;
            }
        }
        return std::nullopt;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Broker 单测(平台中立件的合同)
// ---------------------------------------------------------------------------

TEST_CASE("broker:按钮 data 编解码往返;非审批按钮不认") {
    const std::string data = runtime::EncodeApprovalButtonData("tok123", true);
    CHECK(data == "qai:tok123:1");
    std::string token;
    bool accept = false;
    REQUIRE(runtime::DecodeApprovalButtonData(data, &token, &accept));
    CHECK(token == "tok123");
    CHECK(accept);
    REQUIRE(runtime::DecodeApprovalButtonData(runtime::EncodeApprovalButtonData("t2", false),
                                              &token, &accept));
    CHECK_FALSE(accept);
    // 非审批按钮(菜单/他人伪造):不认,不唤醒任何 future。
    CHECK_FALSE(runtime::DecodeApprovalButtonData("menu:open", &token, &accept));
    CHECK_FALSE(runtime::DecodeApprovalButtonData("qai:only", &token, &accept));
    CHECK_FALSE(runtime::DecodeApprovalButtonData("qai:tok:2", &token, &accept));
    CHECK_FALSE(runtime::DecodeApprovalButtonData("", &token, &accept));
}

TEST_CASE("broker:本人裁决 resolve future;重复幂等;他人 NotAuthorized;陌生 Stale") {
    std::int64_t now = 1000;
    runtime::ChannelInteractionBroker broker([now] { return now; });
    runtime::ChannelApprovalContext context;
    context.channel_id = "qqbot";
    context.account_id = "main";
    context.conversation_id = "dm-a";
    context.turn_key = "qqbot/main/1";
    context.tool_use_id = "tu-1";
    context.tool_name = "guarded_write";
    context.operator_id = "sender-a";
    context.deadline_ms = now + 60'000;

    std::string token;
    auto future = broker.AskApproval(
        context, [&token](const runtime::ChannelInteractionBroker::RequestedFact& fact) {
            token = fact.token;
        });
    REQUIRE(future != nullptr);
    CHECK(broker.IsPending(token));
    CHECK(broker.pending_count() == 1);

    // 他人代按:NotAuthorized,请求仍挂(本人还能按)。
    CHECK(broker.ResolveByToken(token, true, "intruder", "inter-1") ==
          runtime::ChannelInteractionBroker::Resolution::NotAuthorized);
    CHECK(broker.IsPending(token));

    // 本人允许:Applied,future 得到 Accept。
    CHECK(broker.ResolveByToken(token, true, "sender-a", "inter-2") ==
          runtime::ChannelInteractionBroker::Resolution::Applied);
    const auto response = future->WaitApproval();
    REQUIRE(response.has_value());
    CHECK(response->decision == runtime::InteractionDecision::Accept);
    CHECK_FALSE(broker.IsPending(token));

    // 同 token 重复回调:幂等 Duplicate(不崩、不再 resolve)。
    CHECK(broker.ResolveByToken(token, true, "sender-a", "inter-3") ==
          runtime::ChannelInteractionBroker::Resolution::Duplicate);
    CHECK(broker.ResolveByToken(token, false, "sender-a", "inter-4") ==
          runtime::ChannelInteractionBroker::Resolution::Duplicate);

    // 陌生 token(重启后/跨账号/篡改):Stale,不唤醒任何 future。
    CHECK(broker.ResolveByToken("no-such-token", true, "sender-a", "inter-5") ==
          runtime::ChannelInteractionBroker::Resolution::Stale);
}

TEST_CASE("broker:拒绝走 Decline;超时收口 NoteTimeout 记 timeout 事实") {
    std::int64_t now = 1000;
    runtime::ChannelInteractionBroker broker([&now] { return now; });
    runtime::ChannelApprovalContext context;
    context.turn_key = "qqbot/main/2";
    context.operator_id = "sender-a";
    context.deadline_ms = now + 100;

    std::string token;
    auto future = broker.AskApproval(
        context, [&token](const runtime::ChannelInteractionBroker::RequestedFact& fact) {
            token = fact.token;
        });
    CHECK(broker.ResolveByToken(token, false, "sender-a", "inter-1") ==
          runtime::ChannelInteractionBroker::Resolution::Applied);
    const auto declined = future->WaitApproval();
    REQUIRE(declined.has_value());
    CHECK(declined->decision == runtime::InteractionDecision::Decline);

    // 超时路:另一枚请求,等待侧超时后 NoteTimeout 摘表记账。
    std::string token2;
    auto future2 = broker.AskApproval(
        context, [&token2](const runtime::ChannelInteractionBroker::RequestedFact& fact) {
            token2 = fact.token;
        });
    broker.NoteTimeout(token2);
    CHECK_FALSE(broker.IsPending(token2));
    // 已收口的 token 再来回调:Duplicate 幂等(不唤醒)。
    CHECK(broker.ResolveByToken(token2, true, "sender-a", "inter-2") ==
          runtime::ChannelInteractionBroker::Resolution::Duplicate);

    // 按轮取流水:requested 两枚,resolved 一枚 decline + 一枚 timeout。
    std::vector<runtime::ChannelInteractionBroker::RequestedFact> requested;
    std::vector<runtime::ChannelInteractionBroker::ResolvedFact> resolved;
    broker.TakeFactsForTurn("qqbot/main/2", &requested, &resolved);
    CHECK(requested.size() == 2);
    REQUIRE(resolved.size() == 2);
    CHECK(resolved[0].outcome == runtime::ChannelInteractionBroker::Outcome::Declined);
    CHECK(resolved[1].outcome == runtime::ChannelInteractionBroker::Outcome::Timeout);
}

TEST_CASE("broker:CancelByToken 悬空收口(卡片失败/turn 取消)") {
    runtime::ChannelInteractionBroker broker;
    runtime::ChannelApprovalContext context;
    context.turn_key = "qqbot/main/3";
    context.operator_id = "sender-a";
    context.deadline_ms = 999999999;
    std::string token;
    auto future = broker.AskApproval(
        context, [&token](const runtime::ChannelInteractionBroker::RequestedFact& fact) {
            token = fact.token;
        });
    broker.CancelByToken(token, "card_failed");
    CHECK_FALSE(broker.IsPending(token));
    // 未知 token 取消:静默不记账。
    broker.CancelByToken("no-such", "card_failed");
}

// ---------------------------------------------------------------------------
// e2e:假渠道 + 假模型 + 真泵(异步 turn)+ 真 broker
// ---------------------------------------------------------------------------

TEST_CASE("QQ 按钮批准一次:工具触发确认→键盘卡→本人允许→执行→终答→V3 落账") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("approve");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\",\"content\":\"hi\"}"),
        TextScript("已按你的批准执行了写入。"),
    };
    REQUIRE(fixture.OpenPump());

    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));

    // 审批卡发出(工作线程跑到 WaitApproval,tick 发卡)。
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ApprovalCardSend() != nullptr; }));
    const auto* card = fixture.ApprovalCardSend();
    REQUIRE(card != nullptr);
    // 卡片被动锚定触发来信;键盘两枚回调按钮,data 带 opaque token 与决定码;
    // permission 指定配对 sender(平台侧限一道,宿主复核是最终防线)。
    CHECK(card->params["reply_to_message_id"] == "m-1");
    const std::string approve_data = fixture.ButtonDataOf(*card, "approve");
    const std::string decline_data = fixture.ButtonDataOf(*card, "decline");
    REQUIRE(runtime::DecodeApprovalButtonData(approve_data, nullptr, nullptr));
    bool approve_flag = false;
    (void)runtime::DecodeApprovalButtonData(approve_data, nullptr, &approve_flag);
    CHECK(approve_flag);
    CHECK(runtime::EncodeApprovalButtonData(
              approve_data.substr(4, approve_data.rfind(':') - 4), false) ==
          decline_data);
    const auto& buttons = card->params.at("keyboard").at("content").at("rows")[0].at("buttons");
    REQUIRE(buttons.size() == 2);
    CHECK(buttons[0].at("action").at("type") == 1);
    CHECK(buttons[0].at("action").at("permission").at("type") == 0);
    CHECK(buttons[0].at("action").at("permission").at("specify_user_ids")[0] ==
          "sender-dm-a");
    // 摘要脱敏(§12.2 第五行):卡片正文带 path 键摘要,不带全文参数
    //(input 的 content 字段值不出现在卡片上)。
    const std::string card_text = card->params.at("parts")[0].at("text").get<std::string>();
    CHECK(card_text.find("guarded_write") != std::string::npos);
    CHECK(card_text.find("path=a.txt") != std::string::npos);
    CHECK(card_text.find("hi") == std::string::npos);

    // 工具未执行(在等审批)。
    CHECK(CountOf(fixture.counter_file, "tool") == 0);

    // 本人按"允许" → 裁决 Applied,回应 code=0。
    fixture.PressButton(approve_data, "inter-1", "sender-dm-a");
    REQUIRE(fixture.WaitUntil([&fixture] {
        return CountOf(fixture.counter_file, "tool") == 1 &&
               !fixture.sidecar.interaction_acks().empty();
    }));
    REQUIRE(fixture.sidecar.interaction_acks().size() == 1);
    CHECK(fixture.sidecar.interaction_acks()[0].interaction_id == "inter-1");
    CHECK(fixture.sidecar.interaction_acks()[0].code == 0);

    // 模型续跑终答 → 聊天回复入 outbox。
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ChatItem().has_value(); }));
    CHECK(CountOf(fixture.counter_file, "model") == 2);

    // V3 事件账:requested + resolved(approved)各一枚,token 只入 hash。
    const std::string session = fixture.pump->session_id_for("qqbot", "main", "dm-a");
    REQUIRE_FALSE(session.empty());
    const auto room = workspace::index::ResolveDirByWorkspaceKey(
        fixture.workspaces_root, fixture.identity.workspace_key);
    REQUIRE(room.has_value());
    const auto stream = trajectory::v3::FindV3SessionStream(*room / "sessions" / session);
    REQUIRE(stream.has_value());
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(ledger.has_value());
    int requested = 0;
    int resolved_approved = 0;
    for (const auto& line : ledger->events) {
        if (line.kind == trajectory::v3::EventKindV3::ChannelApprovalRequested) {
            ++requested;
            CHECK(line.payload["tool"] == "guarded_write");
            CHECK(line.payload["operatorId"] == "sender-dm-a");
            CHECK(line.payload["argsSha256"].get<std::string>().size() == 64);
            CHECK(line.payload["tokenHash"].get<std::string>().size() == 64);
            CHECK(line.payload.contains("deadlineMs"));
        }
        if (line.kind == trajectory::v3::EventKindV3::ChannelApprovalResolved) {
            CHECK(line.payload["decision"] == "approved");
            CHECK(line.payload["by"] == "sender-dm-a");
            ++resolved_approved;
        }
    }
    CHECK(requested == 1);
    CHECK(resolved_approved == 1);
}

TEST_CASE("QQ 按钮拒绝:工具不执行,模型收到拒绝 tool_result 后终答") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("decline");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("好的,这次不写了。"),
    };
    REQUIRE(fixture.OpenPump());
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ApprovalCardSend() != nullptr; }));
    const std::string decline_data =
        fixture.ButtonDataOf(*fixture.ApprovalCardSend(), "decline");
    fixture.PressButton(decline_data, "inter-1", "sender-dm-a");
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ChatItem().has_value(); }));
    // 工具零执行;模型两轮(工具轮+终答轮);拒绝经 tool_result 喂回。
    CHECK(CountOf(fixture.counter_file, "tool") == 0);
    CHECK(CountOf(fixture.counter_file, "model") == 2);
    REQUIRE(fixture.sidecar.interaction_acks().size() == 1);
    CHECK(fixture.sidecar.interaction_acks()[0].code == 0);
}

TEST_CASE("QQ 审批超时:默认拒绝不默认放行,工具零执行") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("timeout");
    fixture.approval_timeout_ms = 400;  // 真实毫秒(steady_clock)
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("审批没人答复,这次先不写了。"),
    };
    REQUIRE(fixture.OpenPump());
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    // 卡发出但没人按 → 审批窗到点 → 默认拒绝 → 模型收到拒绝终答。
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ChatItem().has_value(); }, 10'000));
    CHECK(CountOf(fixture.counter_file, "tool") == 0);
    CHECK(CountOf(fixture.counter_file, "model") == 2);
    CHECK(fixture.broker->pending_count() == 0);  // 超时已摘表
}

TEST_CASE("QQ 他人代按:ack code=4,请求仍挂,本人随后能批") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("intruder");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("已执行。"),
    };
    REQUIRE(fixture.OpenPump());
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ApprovalCardSend() != nullptr; }));
    const std::string approve_data =
        fixture.ButtonDataOf(*fixture.ApprovalCardSend(), "approve");

    // 陌生人按允许:NotAuthorized → ack code=4(没有权限),请求仍挂。
    fixture.PressButton(approve_data, "inter-1", "intruder-openid");
    REQUIRE(fixture.WaitUntil(
        [&fixture] { return !fixture.sidecar.interaction_acks().empty(); }));
    CHECK(fixture.sidecar.interaction_acks()[0].code == 4);
    CHECK(CountOf(fixture.counter_file, "tool") == 0);

    // 本人随后按允许:照常批准执行(他人代按没吃掉这枚请求)。
    fixture.PressButton(approve_data, "inter-2", "sender-dm-a");
    REQUIRE(fixture.WaitUntil([&fixture] { return CountOf(fixture.counter_file, "tool") == 1; }));
    CHECK(fixture.sidecar.interaction_acks()[1].code == 0);
}

TEST_CASE("QQ 重复回调幂等:本人连按两次,第二次 ack code=3 只返回已处理") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("duplicate");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("已执行。"),
    };
    REQUIRE(fixture.OpenPump());
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ApprovalCardSend() != nullptr; }));
    const std::string approve_data =
        fixture.ButtonDataOf(*fixture.ApprovalCardSend(), "approve");

    fixture.PressButton(approve_data, "inter-1", "sender-dm-a");
    REQUIRE(fixture.WaitUntil(
        [&fixture] { return fixture.sidecar.interaction_acks().size() >= 1; }));
    // 第二次点(平台新 interaction_id,同 token):幂等 Duplicate → code=3。
    fixture.PressButton(approve_data, "inter-2", "sender-dm-a");
    REQUIRE(fixture.WaitUntil(
        [&fixture] { return fixture.sidecar.interaction_acks().size() >= 2; }));
    CHECK(fixture.sidecar.interaction_acks()[1].code == 3);
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ChatItem().has_value(); }));
    CHECK(CountOf(fixture.counter_file, "tool") == 1);  // 只执行一次
    CHECK(CountOf(fixture.counter_file, "model") == 2);  // 模型不重跑
}

TEST_CASE("QQ 等待审批时新来信照常收账(执行线程不是事件泵线程)") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("concurrent");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("已执行。"),
        TextScript("收到,排队处理。"),
    };
    REQUIRE(fixture.OpenPump());
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ApprovalCardSend() != nullptr; }));

    // dm-a 的 turn 挂在审批上:另一会话(dm-b)的来信照样收账入 inbox
    //(tick 线程没被审批等待堵死——§12.2 第九行的验收)。
    fixture.EmitAndIngest(
        MakeDmAt("in-2", "pe-2", "dm-b", "在吗", "m-2", fixture.now));
    bool dm_b_admitted = false;
    for (const auto& record : fixture.manager->IngressRecords("qqbot", "main")) {
        if (record.event.conversation.id == "dm-b" &&
            record.state != channel::IngressEventState::Pending) {
            dm_b_admitted = true;
        }
    }
    CHECK(dm_b_admitted);

    // dm-a 批准收口后,两个会话的回复都出去。
    fixture.PressButton(fixture.ButtonDataOf(*fixture.ApprovalCardSend(), "approve"),
                        "inter-1", "sender-dm-a");
    REQUIRE(fixture.WaitUntil([&fixture] {
        return CountOf(fixture.counter_file, "model") >= 3;
    }, 12'000));
}

TEST_CASE("QQ 审批卡投递失败:取消等待(fail closed),工具不执行") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q6Fixture fixture("card-failed");
    fixture.scripts = {
        ToolUseScript("tu-1", "guarded_write", "{\"path\":\"a.txt\"}"),
        TextScript("审批卡没发出去,这次先不写了。"),
    };
    REQUIRE(fixture.OpenPump());
    // 平台明确拒绝一切发送:审批卡发不出去 → 取消等待(fail closed)。
    fixture.sidecar.set_send_script(
        test_support::FakeChannelSidecar::SendScript::PermanentReject);
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "帮我写入 a.txt", "m-1", fixture.now));
    REQUIRE(fixture.WaitUntil([&fixture] { return fixture.ChatItem().has_value(); }, 10'000));
    CHECK(CountOf(fixture.counter_file, "tool") == 0);
    CHECK(CountOf(fixture.counter_file, "model") == 2);
    CHECK(fixture.broker->pending_count() == 0);  // 卡片失败已取消收口
}

// ---------------------------------------------------------------------------
// V3 注册面:两枚审批 kind 在册且 statusless(合同与 fixture 先行)
// ---------------------------------------------------------------------------

TEST_CASE("V3 注册表:channel.approval.requested/resolved 在册且 statusless") {
    using K = trajectory::v3::EventKindV3;
    CHECK(std::string(trajectory::v3::EventKindV3Name(K::ChannelApprovalRequested)) ==
          "channel.approval.requested");
    CHECK(std::string(trajectory::v3::EventKindV3Name(K::ChannelApprovalResolved)) ==
          "channel.approval.resolved");
    CHECK(trajectory::v3::EventKindV3FromName("channel.approval.requested") ==
          K::ChannelApprovalRequested);
    CHECK(trajectory::v3::EventKindV3FromName("channel.approval.resolved") ==
          K::ChannelApprovalResolved);
    // statusless 事实行(同 state.goal.applied 族):不携带 status。
    CHECK_FALSE(trajectory::v3::RequiredStatusForKind(K::ChannelApprovalRequested).has_value());
    CHECK_FALSE(trajectory::v3::RequiredStatusForKind(K::ChannelApprovalResolved).has_value());
    const auto& all = trajectory::v3::AllEventKindsV3();
    bool has_requested = false;
    bool has_resolved = false;
    for (const K kind : all) {
        has_requested = has_requested || kind == K::ChannelApprovalRequested;
        has_resolved = has_resolved || kind == K::ChannelApprovalResolved;
    }
    CHECK(has_requested);
    CHECK(has_resolved);
}
