// 子代理递归派工与结构化任务交接(单 P0-0~P0-4)的回归册。
//
// P0-0 的三枚"先铆现场"回归在此落成翻好后的期望(各 TEST_CASE 注释里留了
// 铆现场时的缺口断言原文,记录"先红后翻"):
//   1. 交互 auto 后台子代理的工具表没有 agent(缺口 A)——现已按任务快照
//      挂 scoped agent,翻成"真能再派"。
//   2. 两只并行根前台子任务互相抬高深度(缺口 B,全局 foreground_depth_)
//      ——现已按台账 lineage 记层,翻成"并行根各记各的层"。
//   3. 后台 child completion 只能被 main drain(缺口 C,台账是平的)——
//      现已按 delivery target 取,翻成"嵌套结果只归直接父"。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/task_spec.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_task_coordinator.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/subagent_scheduler.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 线程安全的脚本后端:递归树里父与孩子可能并发打同一条记录后端,记录必须
// 带锁;脚本次序按"本线程第几次请求"取,并行根各自一条序,互不抢本。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    // per-thread 模式:开了之后每条线程按各自计数取脚本(并行根回归用,
    // 全局次序会因线程交错把别人的脚本安到这只请求头上)。
    bool per_thread = false;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        std::vector<api::StreamEvent> script;
        {
            std::lock_guard<std::mutex> lock(mutex);
            captured_requests.push_back(request);
            const std::size_t global_idx = captured_requests.size() - 1;
            std::size_t idx = global_idx;
            if (per_thread) {
                const auto thread_id = std::this_thread::get_id();
                std::size_t& local = per_thread_index_[thread_id];
                idx = local++;
            }
            if (idx >= scripts.size()) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
            }
            script = scripts[idx];
        }
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }

    std::size_t request_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captured_requests.size();
    }

    std::vector<api::Request> requests_copy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captured_requests;
    }

    mutable std::mutex mutex;
    std::vector<api::Request> captured_requests;
    std::map<std::thread::id, std::size_t> per_thread_index_;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScriptInput(const std::string& tool_id, const std::string& tool_name,
                                                 const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

tools::DetachedAgentBackend DetachedFrom(const std::shared_ptr<api::Backend>& backend) {
    tools::DetachedAgentBackend detached;
    // 多只嵌套任务共用同一条记录后端时,经共享壳各持一份非拥有引用:
    // DetachedAgentBackend 收 unique_ptr,这里给一枚只转发壳。
    class SharedBackendShell : public api::Backend {
    public:
        explicit SharedBackendShell(std::shared_ptr<api::Backend> inner) : inner_(std::move(inner)) {}
        std::expected<void, api::Error> send_stream(const api::Request& request,
                                                    const std::function<void(const api::StreamEvent&)>& on_event,
                                                    const std::atomic<bool>* cancel = nullptr) override {
            return inner_->send_stream(request, on_event, cancel);
        }

    private:
        std::shared_ptr<api::Backend> inner_;
    };
    detached.backend = std::make_unique<SharedBackendShell>(backend);
    detached.request_profile.model = "detached-model";
    return detached;
}

// 事件喂完就挂住、直到放闸或取消的后端(取消树回归:孩子必须"还在跑"时
// 被父的级联取消打下来)。
class BlockAfterEventsBackend : public api::Backend {
public:
    explicit BlockAfterEventsBackend(std::vector<api::StreamEvent> script) : script_(std::move(script)) {}
    static std::shared_ptr<std::pair<std::mutex, std::condition_variable>> MakeGate() {
        return std::make_shared<std::pair<std::mutex, std::condition_variable>>();
    }
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>& on_event,
                                                const std::atomic<bool>* cancel = nullptr) override {
        for (const auto& event : script_) {
            on_event(event);
        }
        // 外部原子(cancel)没有对应的 notify 源,轮询式等待:50ms 一拍查
        // 谓词,取消/放闸至多一拍内醒(真后端是 cpr 的取消感知等待,同义)。
        std::unique_lock<std::mutex> lock(gate_->first);
        while (!gate_->second.wait_for(lock, std::chrono::milliseconds(50),
                                       [&] { return released || (cancel != nullptr && cancel->load()); })) {
        }
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
        }
        return {};
    }
    void Release() {
        std::lock_guard<std::mutex> lock(gate_->first);
        released = true;
        gate_->second.notify_all();
    }

private:
    std::vector<api::StreamEvent> script_;
    std::shared_ptr<std::pair<std::mutex, std::condition_variable>> gate_ = MakeGate();
    bool released = false;
};

// 等台账里所有任务进终态(超时明败,不挂死测试)。
bool WaitForSettled(const tools::AgentTool& tool, int max_ms = 15000) {
    for (int i = 0; i < max_ms / 10; ++i) {
        if (!tool.HasRunningTasks()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !tool.HasRunningTasks();
}

}  // namespace

// ---------------------------------------------------------------------------
// P0-1:AgentTaskSpec 合同
// ---------------------------------------------------------------------------
TEST_CASE("AgentTaskSpec:最小合法形状可 parse,canonical JSON 稳定,渲染分栏") {
    const nlohmann::json task = nlohmann::json{
        {"goal", "查清后台子代理为何不能再调 agent"},
        {"deliverable", "一份简明源码调查报告"},
    };
    const auto parsed = agent::ParseAgentTaskSpec(task, "查清后台派工");
    REQUIRE(parsed.ok());
    CHECK(parsed.spec->title == "查清后台派工");
    CHECK(parsed.spec->goal == "查清后台子代理为何不能再调 agent");
    CHECK_FALSE(parsed.spec->legacy_prompt);
    // canonical:同输入两次 dump 逐字节一致;空段不进 JSON。
    const nlohmann::json canonical = agent::CanonicalSpecJson(*parsed.spec);
    CHECK(canonical.dump() == agent::CanonicalSpecJson(*parsed.spec).dump());
    CHECK_FALSE(canonical.contains("context"));
    CHECK_FALSE(canonical.contains("scope"));
    // 渲染:标题/目标/交付在,空段(上下文/范围/约束/验收)不渲染。
    const std::string rendered = agent::RenderDelegatedTask(*parsed.spec);
    CHECK(rendered.find("[委派任务 v1]") == 0);
    CHECK(rendered.find("标题:查清后台派工") != std::string::npos);
    CHECK(rendered.find("目标") != std::string::npos);
    CHECK(rendered.find("交付") != std::string::npos);
    CHECK(rendered.find("已知上下文") == std::string::npos);
    CHECK(rendered.find("验收") == std::string::npos);
    // hash 稳定且随内容变。
    CHECK(agent::TaskSpecHash(*parsed.spec) == agent::TaskSpecHash(*parsed.spec));
    agent::AgentTaskSpec mutated = *parsed.spec;
    mutated.goal = "换一件事";
    CHECK(agent::TaskSpecHash(mutated) != agent::TaskSpecHash(*parsed.spec));
}

TEST_CASE("AgentTaskSpec:缺必填/空串/错类型/超限/NUL 按稳定 JSON path 报错") {
    const nlohmann::json title_only = nlohmann::json{{"goal", "g"}};
    CHECK(agent::ParseAgentTaskSpec(title_only, "t").error.find("task.deliverable") != std::string::npos);
    const nlohmann::json missing_goal = nlohmann::json{{"deliverable", "d"}};
    CHECK(agent::ParseAgentTaskSpec(missing_goal, "t").error.find("task.goal") != std::string::npos);
    const nlohmann::json bad_array = nlohmann::json{{"goal", "g"},
                                                    {"deliverable", "d"},
                                                    {"acceptance", nlohmann::json::array({"", "x"})}};
    CHECK(agent::ParseAgentTaskSpec(bad_array, "t").error.find("task.acceptance[0]") != std::string::npos);
    const nlohmann::json wrong_type = nlohmann::json{{"goal", "g"}, {"deliverable", "d"}, {"context", 3}};
    CHECK(agent::ParseAgentTaskSpec(wrong_type, "t").error.find("task.context 必须是字符串数组") !=
          std::string::npos);
    const nlohmann::json with_nul = nlohmann::json{{"goal", std::string("g\0", 2)}, {"deliverable", "d"}};
    CHECK(agent::ParseAgentTaskSpec(with_nul, "t").error.find("NUL") != std::string::npos);
    std::vector<std::string> too_many(17, "条");
    const nlohmann::json over_count = nlohmann::json{{"goal", "g"}, {"deliverable", "d"}, {"context", too_many}};
    CHECK(agent::ParseAgentTaskSpec(over_count, "t").error.find("条数超限") != std::string::npos);
    const nlohmann::json bad_scope = nlohmann::json{{"goal", "g"},
                                                    {"deliverable", "d"},
                                                    {"scope", nlohmann::json{{"include_paths", 5}}}};
    CHECK(agent::ParseAgentTaskSpec(bad_scope, "t").error.find("task.scope.include_paths") != std::string::npos);
}

TEST_CASE("AgentTaskSpec:legacy prompt 归一(goal=prompt、占位交付、legacy 记号)") {
    const agent::AgentTaskSpec spec = agent::CanonicalizeLegacyPrompt("检索阈值", "去查 subagent 段配置");
    CHECK(spec.legacy_prompt);
    CHECK(spec.goal == "去查 subagent 段配置");
    CHECK(spec.deliverable == "按任务说明交付结果");
    CHECK(spec.title == "检索阈值");
}

TEST_CASE("agent 工具:task 与 prompt 同给即拒;legacy prompt 照旧可用") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("旧路照跑")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    sub_registry.Register(std::make_unique<tools::AgentDispatchTool>(agent_tool));

    const auto both = agent_tool.execute(nlohmann::json{
        {"title", "二选一"},
        {"prompt", "旧说明"},
        {"task", nlohmann::json{{"goal", "g"}, {"deliverable", "d"}}},
    });
    CHECK(both.is_error);
    CHECK(both.content.find("同时给") != std::string::npos);

    const auto legacy = agent_tool.execute(nlohmann::json{{"title", "旧路"}, {"prompt", "旧说明"}});
    CHECK_FALSE(legacy.is_error);
    CHECK(legacy.content == "旧路照跑");
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    // legacy:prompt 原样入账(旧行为对账),canonical spec 同步在账。
    CHECK(snapshots[0].prompt == "旧说明");
    REQUIRE(snapshots[0].spec != nullptr);
    CHECK(snapshots[0].spec->legacy_prompt);
    CHECK(snapshots[0].spec->goal == "旧说明");
}

TEST_CASE("agent 工具:结构化 task 的首轮输入是渲染文本,快照存 canonical spec") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("结构化结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const auto result = agent_tool.execute(nlohmann::json{
        {"title", "查调度器"},
        {"task",
         nlohmann::json{{"goal", "查清调度器的深度账"},
                        {"context", nlohmann::json::array({"旧账用全局原子"})},
                        {"constraints", nlohmann::json::array({"只读源码"})},
                        {"acceptance", nlohmann::json::array({"给出文件与符号"})},
                        {"deliverable", "一页调查报告"}}},
    });
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.request_count() == 1);
    std::string first_user_text;
    for (const auto& block : backend.requests_copy()[0].messages.front().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            first_user_text = text->text;
        }
    }
    CHECK(first_user_text.find("[委派任务 v1]") == 0);
    CHECK(first_user_text.find("验收") != std::string::npos);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].prompt == first_user_text);  // 投影即渲染文本
    REQUIRE(snapshots[0].spec != nullptr);
    CHECK(snapshots[0].spec->acceptance.size() == 1);
    CHECK(snapshots[0].spec->deliverable == "一页调查报告");
}

// ---------------------------------------------------------------------------
// P0-2:lineage 与 admission
// ---------------------------------------------------------------------------
TEST_CASE("台账 lineage:main->子->孙各层,root 继承,深度上限与父终态拒绝") {
    tools::TaskLedger ledger;
    tools::SubagentGovernance governance;
    governance.max_active = 8;
    governance.max_depth = 3;
    std::string error;

    tools::AgentTaskSnapshot proto;
    proto.title = "根";
    proto.parent_task_id = 0;
    proto.delivery_target = tools::TaskDeliveryTarget::MainTurnContext;
    auto root = ledger.TryRegisterChild(proto, 1, governance, &error);
    REQUIRE(root != nullptr);
    CHECK(root->snapshot.depth == 1);
    CHECK(root->snapshot.root_task_id == root->snapshot.id);

    tools::AgentTaskSnapshot child_proto;
    child_proto.title = "子";
    child_proto.parent_task_id = root->snapshot.id;
    child_proto.delivery_target = tools::TaskDeliveryTarget::ParentTaskInbox;
    auto child = ledger.TryRegisterChild(child_proto, 2, governance, &error);
    REQUIRE(child != nullptr);
    CHECK(child->snapshot.depth == 2);
    CHECK(child->snapshot.root_task_id == root->snapshot.id);
    CHECK(ledger.ChildTaskIds(root->snapshot.id).size() == 1);
    CHECK(ledger.TreeNodesCount(root->snapshot.id) == 2);
    CHECK(ledger.AliveChildCount(root->snapshot.id) == 1);

    // 深度不许自报:父在第 1 层,只能派第 2 层;报第 1 层直接拒。
    CHECK(ledger.TryRegisterChild(child_proto, 1, governance, &error) == nullptr);
    CHECK(error.find("深度") != std::string::npos);

    // max_depth=2 时孙任务(第 3 层)派出前拒绝,文案与旧口径逐字兼容。
    tools::SubagentGovernance two_layers = governance;
    two_layers.max_depth = 2;
    tools::AgentTaskSnapshot grandchild_proto;
    grandchild_proto.title = "孙";
    grandchild_proto.parent_task_id = child->snapshot.id;
    CHECK(ledger.TryRegisterChild(grandchild_proto, 3, two_layers, &error) == nullptr);
    CHECK(error.find("已达子代理派工深度上限") != std::string::npos);
    CHECK(error.find("subagent.max_depth") != std::string::npos);

    // 父终态后拒绝新孩子(parent_finished,不注册半条任务)。child 先正常
    // 收尾(outcome 补成 Completed 再 finalize,FinalizeFromToolResult 按
    // outcome 分型,不是按传入文本猜)。
    child->snapshot.outcome.status = tools::TaskOutcomeStatus::Completed;
    ledger.FinalizeFromToolResult(child, "done", false);
    CHECK(child->snapshot.state == tools::AgentTaskState::Done);
    CHECK(ledger.TryRegisterChild(grandchild_proto, 3, governance, &error) == nullptr);
    CHECK(error.find("父任务已结束") != std::string::npos);
}

TEST_CASE("台账 admission:并发槽满拒根任务,文案与旧口径逐字兼容") {
    tools::SubagentGovernance governance;
    governance.max_active = 1;
    governance.max_depth = 3;
    tools::TaskLedger ledger;
    std::string error;

    tools::AgentTaskSnapshot root_proto;
    root_proto.title = "根";
    auto root = ledger.TryRegisterChild(root_proto, 1, governance, &error);
    REQUIRE(root != nullptr);
    // 并发槽(max_active=1):根还活着,第二只根任务拒绝。
    CHECK(ledger.TryRegisterChild(root_proto, 1, governance, &error) == nullptr);
    CHECK(error.find("子代理并发槽已满") != std::string::npos);
    CHECK(error.find("subagent.max_active") != std::string::npos);
    // 根收尾退槽,又能注册。
    root->snapshot.outcome.status = tools::TaskOutcomeStatus::Completed;
    ledger.FinalizeFromToolResult(root, "done", false);
    CHECK(ledger.TryRegisterChild(root_proto, 1, governance, &error) != nullptr);
}

TEST_CASE("EvaluateAdmission 纯函数面:closing/children_limit/tree_nodes_limit 各门独立") {
    tools::SubagentGovernance governance;
    governance.max_active = 8;
    governance.max_depth = 3;
    governance.max_children_per_task = 1;
    governance.max_tree_nodes = 2;
    tools::AgentAdmissionRequest request;
    request.parent_task_id = 5;
    request.requested_depth = 2;
    request.parent_alive = true;
    tools::AgentLedgerStats stats;
    stats.parent_children_count = 1;
    request.coordinator_closing = true;
    CHECK_FALSE(tools::EvaluateAdmission(request, stats, governance).allowed);
    request.coordinator_closing = false;
    const auto children_gate = tools::EvaluateAdmission(request, stats, governance);
    CHECK_FALSE(children_gate.allowed);
    CHECK(children_gate.error_code == "children_limit");
    stats.parent_children_count = 0;
    stats.tree_nodes_count = 2;
    const auto tree_gate = tools::EvaluateAdmission(request, stats, governance);
    CHECK_FALSE(tree_gate.allowed);
    CHECK(tree_gate.error_code == "tree_nodes_limit");
    stats.tree_nodes_count = 0;
    CHECK(tools::EvaluateAdmission(request, stats, governance).allowed);
    request.parent_alive = false;
    CHECK(tools::EvaluateAdmission(request, stats, governance).error_code == "parent_finished");
}

// ---------------------------------------------------------------------------
// P0-0 回归二(翻好):并行根前台任务互不抬深度
// 铆现场时的缺口断言(先红):
//   CHECK(child_result.is_error);
//   CHECK(child_result.content.find("深度上限") != npos);
//   // 旧全局 foreground_depth_ 把并行的第二只根抬成第 2 层、它的孩子成第
//   // 3 层,max_depth=2 时当场被拒。
// ---------------------------------------------------------------------------
TEST_CASE("并行根前台任务互不抬深度:两条线程各派根+孩子,全部成功") {
    // 两条线程共享同一只 AgentTool(同一本台账/治理),各自跑两轮
    // "main -> 前台根 -> 前台孩子"。per-thread 后端保证各线程脚本序独立:
    // [0]=根的 agent 调用,[1]=孩子文本,[2]=根收口,[3] 起冗余。
    auto backend = std::make_shared<FakeBackend>();
    backend->per_thread = true;
    for (int i = 0; i < 8; ++i) {
        backend->scripts.push_back(ToolUseScriptInput(
            "tu", "agent",
            R"({"title":"孩子","prompt":"查","execution_mode":"foreground"})"));
        backend->scripts.push_back(TextOnlyScript("孩子结论"));
        backend->scripts.push_back(TextOnlyScript("根收口"));
    }
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(*backend, sub_registry, "/work/dir");
    agent_tool.SetDispatchGovernance(/*max_active=*/8, /*max_depth=*/2);
    sub_registry.Register(std::make_unique<tools::AgentDispatchTool>(agent_tool));

    std::atomic<int> failures{0};
    auto runner = [&]() {
        for (int i = 0; i < 2; ++i) {
            const auto result = agent_tool.execute(nlohmann::json{{"title", "并行根"},
                                                                  {"prompt", "派个孩子"},
                                                                  {"execution_mode", "foreground"}});
            if (result.is_error || result.content != "根收口") {
                ++failures;
            }
        }
    };
    std::thread a(runner);
    std::thread b(runner);
    a.join();
    b.join();
    CHECK(failures.load() == 0);
    // 台账:4 只根(depth 1)+ 4 只孩子(depth 2),无一被深度门拒。
    int roots = 0;
    int children = 0;
    for (const auto& snapshot : agent_tool.TaskSnapshots()) {
        if (snapshot.depth == 1 && snapshot.parent_task_id == 0) {
            ++roots;
            CHECK(snapshot.state == tools::AgentTaskState::Done);
        } else if (snapshot.depth == 2) {
            ++children;
            CHECK(snapshot.parent_task_id != 0);
            CHECK(snapshot.delivery_target == tools::TaskDeliveryTarget::ForegroundCaller);
        }
    }
    CHECK(roots == 4);
    CHECK(children == 4);
}

// ---------------------------------------------------------------------------
// P0-0 回归一(翻好):后台子代理的工具表真有 agent(缺口 A)
// 铆现场时的缺口断言(先红):
//   // 旧 BuildDetachedRegistry 只挂基础表,后台任务的模型调 agent,工具
//   // 结果直接是"没有这个工具"类的错,根任务收不了孩子的结论。
// ---------------------------------------------------------------------------
TEST_CASE("后台子代理能再派前台孩子:结果作为 Tool::Result 回父,不弹 UI") {
    // main 派后台根任务;根任务的模型调 agent(foreground)派孩子。嵌套前台
    // 与父共用同一份 detached 材料(父阻塞等它,无并发),同一条后端的脚本
    // 按次序:根的工具轮 -> 孩子文本 -> 根收口。
    auto root_backend = std::make_shared<FakeBackend>();
    root_backend->scripts = {
        ToolUseScriptInput("t1", "agent", R"({"title":"读调度器","prompt":"查","execution_mode":"foreground"})"),
        TextOnlyScript("孩子结论"),
        TextOnlyScript("根整合完毕"),
    };
    FakeBackend main_backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(main_backend, sub_registry, "/work/dir");
    // 后台根的独立表是调用方表的转发拷贝:壳在,scoped agent 才挂得上。
    sub_registry.Register(std::make_unique<tools::AgentDispatchTool>(agent_tool));
    agent_tool.SetDetachedBackendFactory([root_backend] { return DetachedFrom(root_backend); });

    const auto launch = agent_tool.execute(
        nlohmann::json{{"title", "后台根"}, {"prompt", "派个孩子"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    REQUIRE(WaitForSettled(agent_tool));

    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 2);
    int root_id = 0;
    int child_id = 0;
    for (const auto& snapshot : snapshots) {
        if (snapshot.parent_task_id == 0) {
            root_id = snapshot.id;
            CHECK(snapshot.state == tools::AgentTaskState::Done);
            CHECK(snapshot.result == "根整合完毕");
        } else {
            child_id = snapshot.id;
            CHECK(snapshot.parent_task_id == root_id);
            CHECK(snapshot.depth == 2);
            CHECK(snapshot.delivery_target == tools::TaskDeliveryTarget::ForegroundCaller);
            CHECK(snapshot.state == tools::AgentTaskState::Done);
            CHECK(snapshot.delivered);  // 前台:结论直接回父的工具调用
        }
    }
    CHECK(root_id != 0);
    CHECK(child_id != 0);
    // main 只收根任务的结果,孩子的结论不跨级裸投 main。
    const std::string drained = agent_tool.DrainCompletionNotices();
    CHECK(drained.find("根整合完毕") != std::string::npos);
    CHECK(drained.find("孩子结论") == std::string::npos);
}

// ---------------------------------------------------------------------------
// P0-0 回归三 + P0-4 主场:后台根 -> 后台孩子,completion 进直接父 mailbox
// 铆现场时的缺口断言(先红):
//   // 旧 DrainCompletionNotices 一把提走所有终态:main 会同时收到根与
//   // 孩子两份结果(旧台账没有 delivery_target,嵌套孩子的账也进 main)。
// ---------------------------------------------------------------------------
TEST_CASE("后台根->后台孩子:孩子结果进父 mailbox,父吸收后收口,main 只收根") {
    // 根任务的 detached 后端:第一轮调 agent(background)派孩子;第二轮交
    // "阶段结论"(此刻孩子在跑,父进 WaitingChildren 等条件变量,不烧
    // token);被唤醒吸收孩子结果后续跑一轮,交最终结论。
    auto root_backend = std::make_shared<FakeBackend>();
    root_backend->scripts = {
        ToolUseScriptInput("t1", "agent", R"({"title":"孙任务","prompt":"查","execution_mode":"background"})"),
        TextOnlyScript("根的阶段结论:等孩子"),
        TextOnlyScript("根的最终结论:孩子说完了"),
    };
    // 孩子自己的独立后端(嵌套后台每只一份独立 client,工厂现造)。
    auto child_backend = std::make_shared<FakeBackend>();
    child_backend->scripts = {TextOnlyScript("孩子的结论正文")};

    FakeBackend main_backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(main_backend, sub_registry, "/work/dir");
    sub_registry.Register(std::make_unique<tools::AgentDispatchTool>(agent_tool));
    auto calls = std::make_shared<std::atomic<int>>(0);
    agent_tool.SetDetachedBackendFactory([root_backend, child_backend, calls] {
        // 第一次给根,之后(嵌套后台孩子的独立 client)给孩子的后端。
        return calls->fetch_add(1) == 0 ? DetachedFrom(root_backend) : DetachedFrom(child_backend);
    });

    const auto launch = agent_tool.execute(
        nlohmann::json{{"title", "后台根"}, {"prompt", "派后台孩子"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    REQUIRE(WaitForSettled(agent_tool, 20000));

    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 2);
    const tools::AgentTaskSnapshot* root = nullptr;
    const tools::AgentTaskSnapshot* child = nullptr;
    for (const auto& snapshot : snapshots) {
        if (snapshot.parent_task_id == 0) {
            root = &snapshot;
        } else {
            child = &snapshot;
        }
    }
    REQUIRE(root != nullptr);
    REQUIRE(child != nullptr);
    CHECK(root->state == tools::AgentTaskState::Done);
    CHECK(root->result == "根的最终结论:孩子说完了");
    CHECK(child->parent_task_id == root->id);
    CHECK(child->depth == 2);
    CHECK(child->delivery_target == tools::TaskDeliveryTarget::ParentTaskInbox);
    CHECK(child->delivered);  // 已被父 mailbox 取走

    // 父的消息账里看得见孩子的完成(ChildCompletion 投影,带"外来资料"声明)。
    bool saw_child_completion = false;
    for (const auto& event : agent_tool.TaskEvents(root->id)) {
        if (event.kind == tools::AgentTaskEventKind::SteeringMessage &&
            event.text.find("[子任务结果 #" + std::to_string(child->id) + ":孙任务") != std::string::npos) {
            saw_child_completion = true;
            CHECK(event.text.find("外来资料") != std::string::npos);
        }
    }
    CHECK(saw_child_completion);

    // main 回合只提走根任务的结果;孩子的正文不进 main 回合上下文。
    const std::string drained = agent_tool.DrainCompletionNotices();
    CHECK(drained.find("根的最终结论") != std::string::npos);
    CHECK(drained.find("孩子的结论正文") == std::string::npos);
    CHECK(agent_tool.DrainCompletionNotices().empty());
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());
}

TEST_CASE("取消树:停后台根,后台孩子随树收 Cancelled/ParentCancelled") {
    // 根后端:派完后台孩子后交阶段结论,进 WaitingChildren;孩子后端喂完
    // 首段就挂住(还活着)。停根 -> 级联取消孩子 -> 两只各自收 Cancelled,
    // 孩子记 ParentCancelled,不冒充自己收到 UserStop。
    auto root_backend = std::make_shared<FakeBackend>();
    root_backend->scripts = {
        ToolUseScriptInput("t1", "agent", R"({"title":"孩子","prompt":"查","execution_mode":"background"})"),
        TextOnlyScript("根等孩子"),
    };
    auto child_backend = std::make_shared<BlockAfterEventsBackend>(std::vector<api::StreamEvent>{
        api::MessageStart{"msg", "model"},
        api::TextDelta{"孩子跑着"},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    });

    FakeBackend main_backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(main_backend, sub_registry, "/work/dir");
    sub_registry.Register(std::make_unique<tools::AgentDispatchTool>(agent_tool));
    auto calls = std::make_shared<std::atomic<int>>(0);
    agent_tool.SetDetachedBackendFactory([root_backend, child_backend, calls] {
        class ChildShell : public api::Backend {
        public:
            explicit ChildShell(std::shared_ptr<api::Backend> inner) : inner_(std::move(inner)) {}
            std::expected<void, api::Error> send_stream(const api::Request& request,
                                                        const std::function<void(const api::StreamEvent&)>& on_event,
                                                        const std::atomic<bool>* cancel = nullptr) override {
                return inner_->send_stream(request, on_event, cancel);
            }

        private:
            std::shared_ptr<api::Backend> inner_;
        };
        tools::DetachedAgentBackend detached;
        detached.request_profile.model = "detached-model";
        if (calls->fetch_add(1) == 0) {
            detached.backend = std::make_unique<ChildShell>(std::static_pointer_cast<api::Backend>(root_backend));
        } else {
            detached.backend = std::make_unique<ChildShell>(std::static_pointer_cast<api::Backend>(child_backend));
        }
        return detached;
    });

    const auto launch = agent_tool.execute(
        nlohmann::json{{"title", "根"}, {"prompt", "派孩子"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    int tries = 0;
    while (agent_tool.TaskSnapshots().size() < 2 && tries++ < 500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(agent_tool.TaskSnapshots().size() == 2);
    int root_id = 0;
    int child_id = 0;
    for (const auto& snapshot : agent_tool.TaskSnapshots()) {
        if (snapshot.parent_task_id == 0) {
            root_id = snapshot.id;
        } else {
            child_id = snapshot.id;
        }
    }
    CHECK(root_id != 0);
    CHECK(child_id != 0);

    CHECK(agent_tool.CancelTask(root_id));
    if (!WaitForSettled(agent_tool, 20000)) {
        // 失败现场:把每只任务的活态/停止请求打出来,定位挂在谁身上。
        for (const auto& snapshot : agent_tool.TaskSnapshots()) {
            std::fprintf(stderr, "[cancel-tree] task #%d parent=%d state=%d stop=%d\n", snapshot.id,
                         snapshot.parent_task_id, static_cast<int>(snapshot.state),
                         snapshot.stop_requested ? 1 : 0);
        }
    }
    REQUIRE(WaitForSettled(agent_tool, 5000));
    {
        const auto root_detail = agent_tool.TaskDetail(root_id);
        const auto child_detail = agent_tool.TaskDetail(child_id);
        REQUIRE(root_detail.has_value());
        REQUIRE(child_detail.has_value());
        CHECK(root_detail->state == tools::AgentTaskState::Cancelled);
        CHECK(root_detail->outcome.reason == tools::TaskOutcomeReason::UserStop);
        CHECK(child_detail->state == tools::AgentTaskState::Cancelled);
        CHECK(child_detail->outcome.reason == tools::TaskOutcomeReason::ParentCancelled);
    }
}

TEST_CASE("协调器 closing:RequestClose 后任何 handle 派工稳定拒绝") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    const auto coordinator = agent_tool.coordinator();
    REQUIRE(coordinator != nullptr);
    coordinator->RequestClose();
    const auto result = agent_tool.execute(nlohmann::json{{"title", "关门后"}, {"prompt", "查"}});
    CHECK(result.is_error);
    CHECK(result.content.find("收场") != std::string::npos);
    CHECK(backend.request_count() == 0);
}

TEST_CASE("父终态与孩子注册并发:只可能是'孩子已注册'或'被拒',不留半条") {
    tools::TaskLedger ledger;
    tools::SubagentGovernance governance;
    governance.max_active = 64;
    governance.max_depth = 3;
    for (int round = 0; round < 200; ++round) {
        tools::AgentTaskSnapshot parent_proto;
        parent_proto.title = "父";
        std::string error;
        auto parent = ledger.TryRegisterChild(parent_proto, 1, governance, &error);
        REQUIRE(parent != nullptr);
        std::thread finalizer([&] { ledger.FinalizeFromToolResult(parent, "done", false); });
        tools::AgentTaskSnapshot child_proto;
        child_proto.title = "子";
        child_proto.parent_task_id = parent->snapshot.id;
        std::string child_error;
        auto child = ledger.TryRegisterChild(child_proto, 2, governance, &child_error);
        if (child == nullptr) {
            // 被拒必须是 parent_finished(父先走完),不是别的暗错。
            CHECK(child_error.find("父任务已结束") != std::string::npos);
        }
        finalizer.join();
        if (child != nullptr) {
            // 孩子注册成功:父在注册那笔事务里必然还是活的,父子边完整。
            CHECK(child->snapshot.parent_task_id == parent->snapshot.id);
            CHECK(child->snapshot.root_task_id == parent->snapshot.root_task_id);
            ledger.FinalizeFromToolResult(child, "done", false);
        }
    }
}
