// v3 读取侧投影测试(P2):一份原始账三项读取结果(§4.1)——历史时间线
// (全消息滚动/压缩标记/token 数字/降档退链标注)、当前模型上下文(链
// 投影排除 compact 内部问答)、单次请求输入回放(prepared 与 revision 链
// 对表);按 actionId 折叠工具快照(§4.19)、result_preview 展开与缺件标
// 缺口(§4.10)、usage 缺失不补零(§五)、只读 replay 零调用零重跑(§5.1)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/sha256.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

std::string FileSha(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return lubancode::platform::Sha256Hex(data);
}

const HistoryTimeline::Item* FindMessageItem(const HistoryTimeline& timeline,
                                             const std::string& id) {
    auto it = timeline.message_items.find(id);
    return it == timeline.message_items.end() ? nullptr : &timeline.items[it->second];
}

// 合成会话:user → assistant(带 usage 或缺)→(可选)工具全链。
class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

}  // namespace

// ---------------------------------------------------------------------------
// 读账与只读 replay(§5.1"零模型调用、零工具重跑、零外部消息重发")
// ---------------------------------------------------------------------------

TEST_CASE("只读 replay:九册 fixture 全投影,文件字节前后不变") {
    const char* names[] = {
        "startup.jsonl",     "soul_switch.jsonl",   "tool_round.jsonl",
        "compact_full.jsonl", "stream_interrupted.jsonl", "hook_effects.jsonl",
        "preview_reduction.jsonl", "subagent_parent.jsonl", "subagent_child.jsonl"};
    for (const char* name : names) {
        CAPTURE(name);
        const std::filesystem::path path = Fixture(name);
        const std::string before = FileSha(path);

        auto ledger = ReadV3Ledger(path);
        REQUIRE(ledger.has_value());
        HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
        ModelContext context = ProjectModelContext(*ledger);
        std::vector<ToolActionSnapshot> actions = FoldToolActions(*ledger);
        CHECK(!timeline.items.empty());
        CHECK(!context.system_content.empty());
        (void)actions;
        // 读取侧结构上不存在模型/执行器通路;文件字节不变是硬判据。
        CHECK(FileSha(path) == before);
    }
}

TEST_CASE("读账:索引、时间线合流、revision 链历史") {
    auto ledger = ReadV3Ledger(Fixture("tool_round.jsonl"));
    REQUIRE(ledger.has_value());
    CHECK(ledger->session_id == "20260910-083000-V3FIX3");
    CHECK(ledger->lines == 19);
    // 时间线两类行合流,seq 1..19 连续。
    REQUIRE(ledger->timeline.size() == 19);
    for (std::size_t i = 0; i < ledger->timeline.size(); ++i) {
        CHECK(ledger->timeline[i].seq == i + 1);
    }
    CHECK(ledger->FindMessage("msg-000004") != nullptr);
    CHECK(ledger->FindEvent("evt-000013") != nullptr);
    CHECK(ledger->FindMessage("msg-999999") == nullptr);
    // 每版链快照:rev2 = system + user;rev4 再接 assistant、tool。
    REQUIRE(ledger->revision_chains.count(2) == 1);
    CHECK(ledger->revision_chains.at(2).first == "msg-000001");
    REQUIRE(ledger->revision_chains.at(2).second.size() == 1);
    REQUIRE(ledger->revision_chains.count(4) == 1);
    REQUIRE(ledger->revision_chains.at(4).second.size() == 3);
    CHECK(ledger->revision_chains.at(4).second.back() == "msg-000004");
}

// ---------------------------------------------------------------------------
// 投影一:历史时间线(§4.1 行 1)
// ---------------------------------------------------------------------------

TEST_CASE("时间线:compact 全链可滚动,原文保留并标注已压缩") {
    auto ledger = ReadV3Ledger(Fixture("compact_full.jsonl"));
    REQUIRE(ledger.has_value());
    HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    // 全部 9 条消息都在(含 hidden 的 compact prompt/回复/摘要)。
    for (const char* id : {"msg-000001", "msg-000002", "msg-000003", "msg-000004",
                           "msg-000005", "msg-000006", "msg-000007", "msg-000008",
                           "msg-000009"}) {
        CHECK(FindMessageItem(timeline, id) != nullptr);
    }
    // 被压缩原文:仍在账、标明由 compact-000001 替代(§4.1"原文保留,
    // 标明哪些内容已由摘要替代")。
    const auto* removed = FindMessageItem(timeline, "msg-000002");
    REQUIRE(removed != nullptr);
    CHECK_FALSE(removed->message.in_current_context);
    REQUIRE(removed->message.removed_by_compacts.size() == 1);
    CHECK(removed->message.removed_by_compacts[0] == "compact-000001");
    // compact prompt 默认 hidden(§4.28),不是删除。
    const auto* prompt = FindMessageItem(timeline, "msg-000006");
    REQUIRE(prompt != nullptr);
    CHECK(prompt->message.display == DisplayMode::Hidden);
    CHECK(prompt->message.purpose == MessagePurpose::Compact);
    // 摘要在当前上下文。
    const auto* summary = FindMessageItem(timeline, "msg-000008");
    REQUIRE(summary != nullptr);
    CHECK(summary->message.in_current_context);
    CHECK(summary->message.purpose == MessagePurpose::ContextSummary);
}

TEST_CASE("时间线:压缩标记可展开,token 数字读 applied 持久字段") {
    auto ledger = ReadV3Ledger(Fixture("compact_full.jsonl"));
    REQUIRE(ledger.has_value());
    HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    auto it = timeline.compact_items.find("compact-000001");
    REQUIRE(it != timeline.compact_items.end());
    const auto& marker = timeline.items[it->second].compact;
    CHECK(marker.event_id == "evt-000014");
    CHECK(marker.context_tokens_before == 142800);
    CHECK(marker.context_tokens_after == 31600);
    CHECK(marker.summary_message_ref == "msg-000008");
    CHECK(marker.validation_event_ref == "evt-000013");
    REQUIRE(marker.removed_message_refs.size() == 2);
    CHECK(marker.removed_message_refs[0] == "msg-000002");
    REQUIRE(marker.retained_message_refs.size() == 2);
    CHECK(marker.new_revision == 6);
    CHECK(marker.source_revision == 5);
    // provider usage 缺失不补零:压缩模型的 usage(3400/96)是它自己的账,
    // 不充当主上下文前后数字(§4.11/§五)。
    const auto* candidate = FindMessageItem(timeline, "msg-000007");
    REQUIRE(candidate != nullptr);
    REQUIRE(candidate->message.usage.has_value());
    CHECK(candidate->message.usage->at("inputTokens") == 3400);
    CHECK(marker.context_tokens_before != 3400);
}

TEST_CASE("时间线:provider 没报 usage → null 不补 0(§五)") {
    auto ledger = ReadV3Ledger(Fixture("preview_reduction.jsonl"));
    REQUIRE(ledger.has_value());
    HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    const auto* assistant = FindMessageItem(timeline, "msg-000003");
    REQUIRE(assistant != nullptr);
    CHECK(assistant->message.usage.has_value());   // usage 键在
    CHECK(assistant->message.usage->is_null());    // 值是 null,不是 0
}

TEST_CASE("时间线:system 切换与降档标记") {
    {
        auto ledger = ReadV3Ledger(Fixture("soul_switch.jsonl"));
        REQUIRE(ledger.has_value());
        HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
        int switches = 0;
        for (const auto& item : timeline.items) {
            if (item.kind != HistoryTimeline::Item::Kind::SystemSwitch) {
                continue;
            }
            ++switches;
            CHECK(item.system_switch.completed);
            CHECK(item.system_switch.new_system_ref == "msg-000003");
            CHECK(item.system_switch.old_system_ref == "msg-000001");
        }
        CHECK(switches == 1);
    }
    {
        auto ledger = ReadV3Ledger(Fixture("preview_reduction.jsonl"));
        REQUIRE(ledger.has_value());
        HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
        // 原版 R1_32 退链仍在档,标"被派生版本替代"(§4.38)。
        const auto* original = FindMessageItem(timeline, "msg-000004");
        REQUIRE(original != nullptr);
        CHECK_FALSE(original->message.in_current_context);
        CHECK(original->message.replaced_by_derivation);
        const auto* derived = FindMessageItem(timeline, "msg-000005");
        REQUIRE(derived != nullptr);
        CHECK(derived->message.in_current_context);
        CHECK(derived->message.derived_from == "msg-000004");
        int reductions = 0;
        for (const auto& item : timeline.items) {
            if (item.kind == HistoryTimeline::Item::Kind::PreviewReduction) {
                ++reductions;
            }
        }
        CHECK(reductions == 1);
    }
}

// ---------------------------------------------------------------------------
// 投影二:当前模型上下文(§4.1 行 2)
// ---------------------------------------------------------------------------

TEST_CASE("模型上下文:compact 后 = system + 摘要 + 保留 + 新输入,不混内部问答") {
    auto ledger = ReadV3Ledger(Fixture("compact_full.jsonl"));
    REQUIRE(ledger.has_value());
    ModelContext context = ProjectModelContext(*ledger);
    CHECK(context.revision == 7);
    CHECK(context.system_message_id == "msg-000001");
    CHECK_FALSE(context.system_content.empty());
    CHECK(context.missing_refs.empty());
    CHECK(context.duplicate_action_versions.empty());
    // §4.9 生效后的请求形状:摘要 + 保留 turn + 新输入,无 compact 问答。
    REQUIRE(context.inputs.size() == 4);
    CHECK(context.inputs[0].message_id == "msg-000008");
    CHECK(context.inputs[0].purpose == MessagePurpose::ContextSummary);
    CHECK(context.inputs[1].message_id == "msg-000004");
    CHECK(context.inputs[2].message_id == "msg-000005");
    CHECK(context.inputs[3].message_id == "msg-000009");
    for (const auto& input : context.inputs) {
        CHECK(input.message_id != "msg-000006");  // compact prompt 不入
        CHECK(input.message_id != "msg-000007");  // 压缩模型回复不入
        CHECK(input.message_id != "msg-000002");  // 已压缩原文不重携
        CHECK(input.message_id != "msg-000003");
    }
}

TEST_CASE("模型上下文:降档后链上只选派生版本,同 action 不双版本") {
    auto ledger = ReadV3Ledger(Fixture("preview_reduction.jsonl"));
    REQUIRE(ledger.has_value());
    ModelContext context = ProjectModelContext(*ledger);
    CHECK(context.preview_budget_bytes == 16384);
    CHECK(context.duplicate_action_versions.empty());
    // 链:user + assistant + 派生 tool(原版 msg-000004 已退链)。
    REQUIRE(context.inputs.size() == 3);
    CHECK(context.inputs[2].message_id == "msg-000005");
    CHECK(context.inputs[2].derived_preview);
    // 链上版本是 16 KiB 预览(短文本),不是原版。
    std::string content = context.inputs[2].message.at("content").get<std::string>();
    CHECK(context.inputs[2].message.at("tool_call_id") == "action-000001");
    CHECK_FALSE(content.empty());
}

// ---------------------------------------------------------------------------
// 单次请求输入回放(§4.1 行 3/§4.30 验收"链遍历与 inputMessageRefs 比较")
// ---------------------------------------------------------------------------

TEST_CASE("prepared 回放:conversation 请求与链投影一致") {
    {
        auto ledger = ReadV3Ledger(Fixture("compact_full.jsonl"));
        REQUIRE(ledger.has_value());
        CHECK(CheckPreparedAgainstChain(*ledger, "evt-000016").empty());
    }
    {
        auto ledger = ReadV3Ledger(Fixture("tool_round.jsonl"));
        REQUIRE(ledger.has_value());
        CHECK(CheckPreparedAgainstChain(*ledger, "evt-000015").empty());
    }
}

TEST_CASE("prepared 回放:compact 请求不走主链,引用悬空才报") {
    auto ledger = ReadV3Ledger(Fixture("compact_full.jsonl"));
    REQUIRE(ledger.has_value());
    // evt-000010 是压缩请求(purpose=compact):输入是压缩材料清单,
    // 不与主链对表;但引用必须都能落在账上。
    CHECK(CheckPreparedAgainstChain(*ledger, "evt-000010").empty());
    CHECK_FALSE(CheckPreparedAgainstChain(*ledger, "evt-000014").empty());  // 非 prepared
}

// ---------------------------------------------------------------------------
// 按 actionId 折叠工具快照(§4.19)
// ---------------------------------------------------------------------------

TEST_CASE("工具折叠:声明/执行/结果/选用/消息一体可查") {
    auto ledger = ReadV3Ledger(Fixture("hook_effects.jsonl"));
    REQUIRE(ledger.has_value());
    std::vector<ToolActionSnapshot> actions = FoldToolActions(*ledger);
    const ToolActionSnapshot* action = FindActionSnapshot(actions, "action-000001");
    REQUIRE(action != nullptr);
    CHECK(action->turn_id == "turn-000001");
    CHECK(action->step_id == "step-000001");
    CHECK(action->assistant_message_ref == "msg-000003");
    CHECK(action->folded_status == "done");
    REQUIRE(action->attempts.size() == 1);
    CHECK(action->attempts[0].attempt == 1);
    CHECK(action->attempts[0].started);
    CHECK(action->attempts[0].status == "done");
    CHECK(action->attempts[0].exit_code == 0);
    CHECK(action->attempts[0].execution_duration_ms == 250);
    CHECK(action->attempts[0].effective_args_ref == "args-action-000001");
    CHECK(action->attempts[0].idempotency_key.has_value());
    // 结果链:persisted → selected。
    REQUIRE(action->persisted_event_refs.size() == 1);
    CHECK(action->selected_event_ref == "evt-000018");
    CHECK(action->effective_outcome == "done");
    REQUIRE(action->result_refs.size() == 1);
    CHECK(action->result_refs[0].size() == 2);  // metadata + stdout
    // 消息版本:一条 tool 消息在链上。
    REQUIRE(action->message_versions.size() == 1);
    CHECK(action->message_versions[0].on_current_chain);
}

TEST_CASE("工具折叠:声明块参数按 provider 号配对解析(§4.15)") {
    auto ledger = ReadV3Ledger(Fixture("tool_round.jsonl"));
    REQUIRE(ledger.has_value());
    std::vector<ToolActionSnapshot> actions = FoldToolActions(*ledger);
    const ToolActionSnapshot* action = FindActionSnapshot(actions, "action-000001");
    REQUIRE(action != nullptr);
    CHECK(action->tool_name == "read_file");
    REQUIRE(action->declared_args.has_value());
    CHECK(action->declared_args->at("path") == "src/app/main.cpp");
    CHECK(action->provider_tool_call_id == "call_prov_9xK");
}

TEST_CASE("工具折叠:started 无终态 → 恢复投影标 unknown,不合成假终态") {
    FixedClock clock;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-unknown";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path jsonl = dir / "session.jsonl";
    {
        auto writer = V3Writer::Start(jsonl, "20260910-190000-RDR001", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        REQUIRE(writer.has_value());
        ToolActionSession action = ToolActionSession::Admit(
            *writer, "turn-000001", "step-000001", "action-000001", "queued", std::nullopt,
            std::nullopt);
        REQUIRE(action.Start(*writer, "args-ref", ToolIdentity{"grep", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        // 崩溃:started 后无终态。
    }
    auto ledger = ReadV3Ledger(jsonl);
    REQUIRE(ledger.has_value());
    std::vector<ToolActionSnapshot> actions = FoldToolActions(*ledger);
    const ToolActionSnapshot* action = FindActionSnapshot(actions, "action-000001");
    REQUIRE(action != nullptr);
    CHECK(action->folded_status == "unknown");
    REQUIRE(action->attempts.size() == 1);
    CHECK(action->attempts[0].unknown_recovery);
    CHECK_FALSE(action->attempts[0].exit_code.has_value());  // 不默认 0
}

TEST_CASE("工具折叠:attempt 重试链逐次留档(§4.14)") {
    FixedClock clock;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-retry";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path jsonl = dir / "session.jsonl";
    {
        auto writer = V3Writer::Start(jsonl, "20260910-190000-RDR002", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        REQUIRE(writer.has_value());
        ToolActionSession action = ToolActionSession::Admit(
            *writer, "turn-000001", "step-000001", "action-000001", "queued", std::nullopt,
            std::nullopt);
        REQUIRE(action.Start(*writer, "args-1", ToolIdentity{"grep", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Fail(*writer, "exit_137").status == WriteReceipt::Status::Committed);
        REQUIRE(action.BeginNextAttempt(*writer, "auto_retry")
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Start(*writer, "args-1", ToolIdentity{"grep", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, 0, 40).status == WriteReceipt::Status::Committed);
        // 结果链补齐(失败与恢复单 P1-A:done 无结果链会折成 result_missing
        // 缺口态——本钉只看尝试链折叠,给全链保持 done)。
        WriteReceipt persisted = action.PersistedResult(
            *writer,
            {MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                             std::string(64, '7'), 24, "application/json")},
            action.last_event_id());
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        REQUIRE(action.SelectResult(*writer, {persisted.id}, {}, "done").status ==
                WriteReceipt::Status::Committed);
        REQUIRE(action.AppendToolMessage(*writer, "ok", action.selected_event_id()).status ==
                WriteReceipt::Status::Committed);
    }
    auto ledger = ReadV3Ledger(jsonl);
    REQUIRE(ledger.has_value());
    std::vector<ToolActionSnapshot> actions = FoldToolActions(*ledger);
    const ToolActionSnapshot* action = FindActionSnapshot(actions, "action-000001");
    REQUIRE(action != nullptr);
    REQUIRE(action->attempts.size() == 2);
    CHECK(action->attempts[0].status == "failed");
    CHECK(action->attempts[1].status == "done");
    CHECK(action->attempts[1].exit_code == 0);
    CHECK(action->folded_status == "done");  // 折叠看最后一枚尝试
}

// ---------------------------------------------------------------------------
// result_preview 读取投影(§4.18)与缺件标缺口(§4.10)
// ---------------------------------------------------------------------------

TEST_CASE("result_preview:选用链展开,消息正文即读取投影") {
    // subagent_parent 的 tool 消息带 resultSelectionRef:选用链完整。
    auto ledger = ReadV3Ledger(Fixture("subagent_parent.jsonl"));
    REQUIRE(ledger.has_value());
    // 无 session_dir:引用链照展,artifact 全标缺口,不冒称完整。
    ResultPreviewProjection projection =
        ExpandResultPreview(*ledger, std::filesystem::path{}, "msg-000004");
    CHECK(projection.result_selection_ref == "evt-000012");
    REQUIRE(projection.source_result_event_refs.size() == 1);
    CHECK(projection.source_result_event_refs[0] == "evt-000011");
    CHECK_FALSE(projection.result_preview.empty());
    CHECK(projection.result_preview.find("task accepted") == 0);
    REQUIRE(projection.result_refs.size() == 1);
    CHECK(projection.result_refs[0].at("artifactId") == "res-000001");
    REQUIRE(projection.artifacts.size() == 1);
    CHECK(projection.artifacts[0].gap_reason == "missing_blob");
    CHECK_FALSE(projection.complete);  // §5.1"新档缺 blob 标缺口"
}

TEST_CASE("result_preview:无选用引用时按 actionId 回退收 persisted") {
    // tool_round 的 tool 消息没写可选键 resultSelectionRef(早期形状):
    // 读取回退按 actionId 收全部 persisted,引用照样闭合。
    auto ledger = ReadV3Ledger(Fixture("tool_round.jsonl"));
    REQUIRE(ledger.has_value());
    ResultPreviewProjection projection =
        ExpandResultPreview(*ledger, std::filesystem::path{}, "msg-000004");
    CHECK(projection.result_selection_ref.empty());
    REQUIRE(projection.result_refs.size() == 2);  // evt-000012 的 metadata + stdout
    CHECK(projection.result_refs[0].at("artifactId") == "R-000001");
    REQUIRE(projection.artifacts.size() == 2);
    CHECK(projection.artifacts[0].gap_reason == "missing_blob");
    CHECK_FALSE(projection.complete);
}

TEST_CASE("result_preview:真文件验 hash,坏 hash/缺件分得清") {
    FixedClock clock;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-blob";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path jsonl = dir / "session.jsonl";
    std::string tool_message_id;
    std::string good_sha;
    {
        auto writer = V3Writer::Start(jsonl, "20260910-191000-RDR003", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        REQUIRE(writer.has_value());
        ToolActionSession action = ToolActionSession::Admit(
            *writer, "turn-000001", "step-000001", "action-000001", "queued", std::nullopt,
            std::nullopt);
        REQUIRE(action.Start(*writer, "args", ToolIdentity{"run", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, 0, 10).status == WriteReceipt::Status::Committed);
        // 真落两份 artifact:描述 + stdout。
        const std::string stdout_text = "hello\nworld\n";
        good_sha = lubancode::platform::Sha256Hex(stdout_text);
        std::filesystem::path artifacts = dir / "artifacts";
        std::filesystem::create_directories(artifacts, ec);
        std::filesystem::path stdout_file = artifacts / "res-000001.stdout.txt";
        {
            std::ofstream out(stdout_file, std::ios::binary);
            out << stdout_text;
        }
        nlohmann::json metadata = nlohmann::json::object(
            {{"result_id", "res-000001"}, {"result_kind", "process"}});
        const std::string metadata_sha = lubancode::platform::Sha256Hex(metadata.dump());
        {
            std::ofstream out(artifacts / "res-000001.json", std::ios::binary);
            out << metadata.dump();
        }
        std::vector<nlohmann::json> refs = {
            MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                            metadata_sha, metadata.dump().size(), "application/json"),
            MakeArtifactRef("res-000001-stdout", "stdout", "artifacts/res-000001.stdout.txt",
                            good_sha, stdout_text.size(), "text/plain"),
            MakeArtifactRef("res-000001-stderr", "stderr", "artifacts/res-000001.stderr.txt",
                            std::string(64, '9'), 5, "text/plain"),  // 这份不落盘
        };
        WriteReceipt persisted =
            action.PersistedResult(*writer, refs, action.last_event_id());
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        WriteReceipt selected = action.SelectResult(*writer, {persisted.id}, {}, "done");
        REQUIRE(selected.status == WriteReceipt::Status::Committed);
        WriteReceipt message = action.AppendToolMessage(*writer, "exit_code: 0\nhello",
                                                        action.selected_event_id());
        REQUIRE(message.status == WriteReceipt::Status::Committed);
        // AppendToolMessage 的回执是接纳事件;tool 消息 id 取链尾。
        tool_message_id = writer->context().chain.back().message_ref;
    }
    auto ledger = ReadV3Ledger(jsonl);
    REQUIRE(ledger.has_value());
    ResultPreviewProjection projection = ExpandResultPreview(*ledger, dir, tool_message_id);
    CHECK(projection.complete == false);  // stderr 那份缺件
    REQUIRE(projection.artifacts.size() == 3);
    CHECK(projection.artifacts[0].hash_ok);
    CHECK(projection.artifacts[0].gap_reason.empty());
    CHECK(projection.artifacts[1].hash_ok);
    CHECK(projection.artifacts[2].gap_reason == "missing_blob");
    CHECK(projection.result_preview == "exit_code: 0\nhello");

    // 篡改 stdout:hash 对不上,标 hash_mismatch(§4.21"原始 artifact 被改
    // 或缺失 → 拒绝假称完整")。
    {
        std::ofstream out(dir / "artifacts" / "res-000001.stdout.txt", std::ios::binary);
        out << "tampered";
    }
    ResultPreviewProjection tampered = ExpandResultPreview(*ledger, dir, tool_message_id);
    REQUIRE(tampered.artifacts.size() == 3);
    CHECK(tampered.artifacts[1].exists);
    CHECK_FALSE(tampered.artifacts[1].hash_ok);
    CHECK(tampered.artifacts[1].gap_reason == "hash_mismatch");
    CHECK_FALSE(tampered.complete);
}
