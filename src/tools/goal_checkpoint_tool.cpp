// goal_checkpoint 工具实现:schema 校验、evidence 白名单、状态门槛。

#include "tools/goal_checkpoint_tool.hpp"

#include <algorithm>

namespace lubancode::tools {

std::string GoalCheckpointTool::name() const { return "goal_checkpoint"; }

std::string GoalCheckpointTool::description() const {
    return "为目标当前迭代写一枚检查点:本轮完成了什么、还欠什么、下一步做什么、"
           "引用哪些宿主证据。这是跨轮续跑的路标,不是聊天摘要。收到 ready_for_"
           "evaluation 时宿主才会验收;blocked 须给 blocker_key,needs_user 须给 "
           "question。evidence_ids 只能引用本迭代宿主已产出的证据 id。";
}

nlohmann::json GoalCheckpointTool::input_schema() const {
    // 单子"checkpoint 工具"节的 Schema 原样(additionalProperties=false、
    // required 六项、各字段上限)。
    nlohmann::json status_enum = nlohmann::json::array(
        {"progress", "ready_for_evaluation", "blocked", "needs_user"});
    nlohmann::json schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = nlohmann::json::array(
        {"status", "summary", "completed", "remaining", "next_action", "evidence_ids"});
    nlohmann::json props;
    props["status"]["type"] = "string";
    props["status"]["enum"] = status_enum;
    props["summary"] = {{"type", "string"}, {"maxLength", 1200}};
    props["completed"] = {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 20}};
    props["remaining"] = {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 20}};
    props["next_action"] = {{"type", "string"}, {"maxLength", 600}};
    props["evidence_ids"] = {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 50}};
    props["blocker_key"] = {{"type", "string"}, {"maxLength", 200}};
    props["question"] = {{"type", "string"}, {"maxLength", 600}};
    schema["properties"] = props;
    return schema;
}

tools::Tool::Result GoalCheckpointTool::execute(const nlohmann::json& input) {
    using R = tools::Tool::Result;
    const auto fail = [](std::string code, std::string message) {
        R r;
        r.is_error = true;
        r.outcome = "tool_error";
        r.error_code = std::move(code);
        r.SetText(std::move(message));
        return r;
    };

    if (state_ == nullptr || state_->goal_id.empty()) {
        // 不在 goal turn:普通 turn/子代理看不见这只工具;真被调到(旁路)
        // 明确拒绝,不装成功。
        return fail("goal.not_in_goal_turn",
                    "goal_checkpoint 只在目标执行轮可用;当前会话没有进行中的 goal iteration。");
    }

    if (!input.is_object()) {
        return fail("schema.rejected", "入参必须是 JSON object。");
    }
    const auto& obj = input;
    const auto require = [&](const char* key) -> const nlohmann::json* {
        if (!obj.contains(key)) return nullptr;
        return &obj.at(key);
    };

    // status 四枚。
    const auto* status_field = require("status");
    if (status_field == nullptr || !status_field->is_string()) {
        return fail("schema.rejected", "缺 status 字段。");
    }
    const std::string status_text = status_field->get<std::string>();
    GoalCheckpointStatus status;
    if (status_text == "progress") {
        status = GoalCheckpointStatus::Progress;
    } else if (status_text == "ready_for_evaluation") {
        status = GoalCheckpointStatus::ReadyForEvaluation;
    } else if (status_text == "blocked") {
        status = GoalCheckpointStatus::Blocked;
    } else if (status_text == "needs_user") {
        status = GoalCheckpointStatus::NeedsUser;
    } else {
        return fail("schema.rejected", "status 只认 progress/ready_for_evaluation/blocked/needs_user。");
    }

    GoalCheckpointEntry entry;
    entry.status = status;
    const std::string status_text_for_ack = status_text;
    const auto* summary_raw = require("summary");
    if (summary_raw == nullptr || !summary_raw->is_string() ||
        summary_raw->get<std::string>().size() > 1200) {
        return fail("schema.rejected", "summary 缺失或超 1200 字节。");
    }
    entry.summary = summary_raw->get<std::string>();
    const auto* next_field = require("next_action");
    if (next_field == nullptr || !next_field->is_string() ||
        next_field->get<std::string>().size() > 600) {
        return fail("schema.rejected", "next_action 缺失或超 600 字节。");
    }
    entry.next_action = next_field->get<std::string>();

    // 数组字段。
    const auto read_list = [&](const char* key, std::size_t max_items,
                               std::vector<std::string>& out) -> bool {
        const auto* f = require(key);
        if (f == nullptr) return false;
        if (!f->is_array() || f->size() > max_items) return false;
        out.clear();
        out.reserve(f->size());
        for (const auto& item : *f) {
            if (!item.is_string()) return false;
            out.push_back(item.get<std::string>());
        }
        return true;
    };
    if (!read_list("completed", 20, entry.completed)) {
        return fail("schema.rejected", "completed 须是字符串数组(至多 20 项)。");
    }
    if (!read_list("remaining", 20, entry.remaining)) {
        return fail("schema.rejected", "remaining 须是字符串数组(至多 20 项)。");
    }
    if (!read_list("evidence_ids", 50, entry.evidence_ids)) {
        return fail("schema.rejected", "evidence_ids 须是字符串数组(至多 50 项)。");
    }

    // 语义门槛:blocked 必有 blocker_key;needs_user 必有 question。
    const auto* blocker = require("blocker_key");
    if (blocker != nullptr && blocker->is_string()) {
        const std::string key = blocker->get<std::string>();
        if (!key.empty()) entry.blocker_key = key;
    }
    const auto* question = require("question");
    if (question != nullptr && question->is_string()) {
        const std::string q = question->get<std::string>();
        if (!q.empty()) entry.question = q;
    }
    if (status == GoalCheckpointStatus::Blocked && !entry.blocker_key.has_value()) {
        return fail("goal.checkpoint_blocker_missing", "status=blocked 必须给 blocker_key。");
    }
    if (status == GoalCheckpointStatus::NeedsUser && !entry.question.has_value()) {
        return fail("goal.checkpoint_question_missing", "status=needs_user 必须给 question。");
    }

    // evidence 白名单:只认本 goal、本 iteration 已产出的 id。
    for (const std::string& ev_id : entry.evidence_ids) {
        if (std::find(state_->valid_evidence_ids.begin(), state_->valid_evidence_ids.end(), ev_id) ==
            state_->valid_evidence_ids.end()) {
            return fail("goal.evidence_unknown",
                        "evidence id 不在本迭代宿主证据清单里: " + ev_id);
        }
    }

    // 一轮可多次 checkpoint;最后一枚为候选,每枚都留(state_ 落 trace 由装配层)。
    state_->entries.push_back(entry);

    R r;
    std::string ack = "检查点已记(";
    ack += status_text_for_ack;
    ack += ")。";
    if (status == GoalCheckpointStatus::ReadyForEvaluation) {
        ack += "宿主将按冻结合同与证据验收;ready_for_evaluation 不等于达标。";
    }
    r.SetText(std::move(ack));
    r.is_error = false;
    r.outcome = "succeeded";
    r.details["checkpoint_count"] = static_cast<std::uint64_t>(state_->entries.size());
    return r;
}

}  // namespace lubancode::tools
