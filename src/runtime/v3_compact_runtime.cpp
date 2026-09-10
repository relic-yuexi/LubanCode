// v3 compact 运行时实现:全链驱动见头注。合同细节引设计总纲
// todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo(§4.6-4.10
// 范围冻结/请求形状/校验/applied;§4.37-4.41 触发/容量/连续压缩;
// §4.64 撞窗整轮回退)。
#include "runtime/v3_compact_runtime.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancode::runtime {

namespace {

using trajectory::v3::MessageLine;

// ---------------------------------------------------------------------------
// 估算(§1.14 首版 bytes/4,ceil)
// ---------------------------------------------------------------------------

std::uint64_t EstimateUtf8Div4(std::string_view utf8) {
    if (utf8.empty()) {
        return 0;
    }
    return (static_cast<std::uint64_t>(utf8.size()) + 3) / 4;
}

std::uint64_t EstimateMessageTokens(const MessageLine& line) {
    const auto canonical = trajectory::CanonicalJsonDump(line.message);
    if (!canonical.has_value()) {
        return 0;
    }
    return EstimateUtf8Div4(*canonical);
}

std::size_t CountUtf8Chars(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// 空白归一(守恒比对用):连续空白折成一,首尾去净——只许调整空白,
// 不许改写(与 v2 ValidateCompactManifest 同口径)。
std::string NormalizeWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

nlohmann::json IdsToJson(const std::vector<std::string>& ids) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& id : ids) {
        array.push_back(id);
    }
    return array;
}

// 取末尾最后一个 ```json 围栏块并解析;缺围栏/坏 JSON 给 nullopt。
std::optional<nlohmann::json> ParseTrailingJsonFence(const std::string& text) {
    const std::string::size_type open = text.rfind("```json");
    if (open == std::string::npos) {
        return std::nullopt;
    }
    const std::string::size_type body_start = text.find('\n', open);
    if (body_start == std::string::npos) {
        return std::nullopt;
    }
    const std::string::size_type close = text.find("```", body_start);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::string body = text.substr(body_start + 1, close > body_start + 1 ? close - body_start - 1 : 0);
    body.erase(body.find_last_not_of(" \t\r\n") + 1);
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

// ---------------------------------------------------------------------------
// 校验清单(§4.8 内容结构:requirementsSnapshot 声明的必需字段/类型/非空)
// ---------------------------------------------------------------------------

struct Requirements {
    std::vector<std::string> required_string_fields{"goal", "next_action"};
    std::vector<std::string> required_array_fields{"constraints", "open_items"};
    std::vector<std::string> required_open_items;
    std::uint64_t min_summary_chars = 40;
};

Requirements ParseRequirements(const nlohmann::json& snapshot) {
    Requirements req;
    auto read_list = [&snapshot](const char* key, std::vector<std::string>& out) {
        const auto it = snapshot.find(key);
        if (it == snapshot.end() || !it->is_array()) {
            return;
        }
        out.clear();
        for (const auto& item : *it) {
            if (item.is_string() && !item.get<std::string>().empty()) {
                out.push_back(item.get<std::string>());
            }
        }
    };
    read_list("requiredStringFields", req.required_string_fields);
    read_list("requiredArrayFields", req.required_array_fields);
    read_list("requiredOpenItems", req.required_open_items);
    if (const auto it = snapshot.find("minSummaryChars");
        it != snapshot.end() && it->is_number_unsigned()) {
        req.min_summary_chars = it->get<std::uint64_t>();
    }
    return req;
}

nlohmann::json RequirementsToJson(const Requirements& req) {
    nlohmann::json json = nlohmann::json::object(
        {{"schema", "compact-requirements-1"},
         {"requiredStringFields", IdsToJson(req.required_string_fields)},
         {"requiredArrayFields", IdsToJson(req.required_array_fields)},
         {"requiredOpenItems", IdsToJson(req.required_open_items)},
         {"minSummaryChars", req.min_summary_chars}});
    return json;
}

// ---------------------------------------------------------------------------
// 缺省压缩专用 system(§4.3:另存实际内容,不替换会话 system;调用方
// 可用 prompts/features/compact-handoff.md 覆盖)
// ---------------------------------------------------------------------------

std::string DefaultSpecialSystem() {
    return
        "你是 LubanCode 的上下文压缩器。任务:把提供的对话材料压成一份给后续会话"
        "接力的交接摘要——保留任务目标、用户明示约束、关键决策、文件路径、代码要点、"
        "已证实事实、失败尝试及其原因、未完成事项;丢弃寒暄与过程噪音。\n"
        "摘要正文用 Markdown,分节清楚。正文之后另起一行输出一枚 JSON 代码块"
        "(```json 围栏),键名逐字照写:\n"
        "```json\n"
        "{\"goal\": \"当前任务目标一句话\", \"constraints\": [\"用户明示的约束或禁止\"], "
        "\"open_items\": [\"未完成事项\"], \"next_action\": \"下一步该做的具体动作\"}\n"
        "```\n"
        "goal 与 next_action 不许为空串;数组元素必须是字符串;JSON 必须能直接解析,"
        "不要加注释。摘要只覆盖指令声明的压缩范围;标注\"仅供参考\"的材料不要整段"
        "复制进摘要。";
}

// ---------------------------------------------------------------------------
// 范围计划(§4.30/§4.41/§4.64):链序整轮分块,removed 前缀 + retained 尾
// ---------------------------------------------------------------------------

struct PlanBlock {
    std::optional<std::string> turn_id;  // nullopt = 游离节点(旧摘要等)
    bool summary_head = false;           // 链头紧随 system 的游离段 = 当前旧摘要 Q
    std::vector<const MessageLine*> messages;  // 链序
    std::uint64_t tokens = 0;
};

struct ScopePlan {
    std::vector<PlanBlock> removed;    // 压缩材料(含旧摘要 Q,§4.41 无永久保留特权)
    std::vector<PlanBlock> retained;   // 保留尾部 R(未完成/受保护 turn)+ 回退并入的 K
    std::vector<std::string> protected_turns;

    std::vector<std::string> RemovedIds() const {
        std::vector<std::string> ids;
        for (const auto& block : removed) {
            for (const auto* message : block.messages) {
                ids.push_back(message->message_id);
            }
        }
        return ids;
    }
    std::vector<std::string> RetainedIds() const {
        std::vector<std::string> ids;
        for (const auto& block : retained) {
            for (const auto* message : block.messages) {
                ids.push_back(message->message_id);
            }
        }
        return ids;
    }
    std::uint64_t RemovedTokens() const {
        std::uint64_t tokens = 0;
        for (const auto& block : removed) {
            tokens += block.tokens;
        }
        return tokens;
    }
    std::uint64_t RetainedTokens() const {
        std::uint64_t tokens = 0;
        for (const auto& block : retained) {
            tokens += block.tokens;
        }
        return tokens;
    }
};

// 折叠状态是否已收口(未收口的 turn 整轮保护,§4.8"工具配对"行)。
bool ToolStatusTerminal(const std::string& status) {
    return status == "done" || status == "failed" || status == "cancelled" || status == "rejected";
}

}  // namespace

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

V3CompactRunResult RunV3Compact(trajectory::v3::V3Writer& writer,
                                V3CompactModelClient& client,
                                const V3CompactProfile& profile, V3CompactRunInput input) {
    using trajectory::v3::CompactSession;
    using trajectory::v3::WriteReceipt;

    V3CompactRunResult result;
    if (input.trigger.empty()) {
        input.trigger = "manual";
    }
    if (input.reason.empty()) {
        input.reason = input.trigger == "auto" ? "threshold" : "user_command";
    }
    const Requirements requirements = ParseRequirements(input.requirements_snapshot);

    // ---- 0. 读账:范围计划要消息元数据(turn/purpose/正文),writer 内存
    // 视图只有链。读取侧与 writer 同一份验卷逻辑;journal 共享读,单写者
    // 不受扰。读不了/与内存视图不同拍,明说,不猜。 ----
    auto ledger_or = trajectory::v3::ReadV3Ledger(writer.path());
    if (!ledger_or.has_value()) {
        result.terminal_kind = "not_begun";
        result.reason = "compact.ledger_unreadable: " + ledger_or.error();
        return result;
    }
    const trajectory::v3::V3Ledger& ledger = *ledger_or;
    if (ledger.context.revision != writer.context().revision) {
        result.terminal_kind = "not_begun";
        result.reason = "compact.stale_ledger: 账面 revision " +
                        std::to_string(ledger.context.revision) + " 与写者 " +
                        std::to_string(writer.context().revision) + " 不同拍";
        return result;
    }

    // ---- 1. 开场:compact.requested + 内部回合(§4.6)。已有进行中的
    // compact 时库层拒收(busy),不另开场。 ----
    auto begin = CompactSession::Begin(writer, input.trigger, input.reason,
                                       input.parent_turn_id, RequirementsToJson(requirements));
    result.compact_id = begin.info.compact_id;
    result.turn_id = begin.info.turn_id;
    if (!begin.info.began) {
        result.terminal_kind = begin.info.error.rfind("compact.busy", 0) == 0 ? "busy" : "not_begun";
        result.reason = begin.info.error;
        return result;
    }
    CompactSession& session = *begin.session;

    // 结束兜底:任何提前 return 前必须落终态(除非库层已落)。
    const auto finish_rejected = [&](const std::string& reason) {
        session.Fail(writer, CompactSession::FailKind::Rejected, reason);
        result.terminal_kind = "rejected";
        result.reason = reason;
    };
    const auto finish_failed = [&](const std::string& reason) {
        session.Fail(writer, CompactSession::FailKind::Failed, reason);
        result.terminal_kind = "failed";
        result.reason = reason;
    };

    // ---- 2. 范围计划:链序分块(system 之外),保护集 = 调用方钉的 +
    // 工具动作未收口的 turn;removed = 保护界之前全部(含旧摘要),
    // retained = 界后尾部。 ----
    const std::vector<trajectory::v3::ChainNode>& chain = writer.context().chain;
    std::vector<const MessageLine*> chain_messages;  // 根(system)之后,链序
    for (std::size_t i = 1; i < chain.size(); ++i) {
        const MessageLine* line = ledger.FindMessage(chain[i].message_ref);
        if (line == nullptr) {
            finish_rejected("compact.missing_chain_ref: " + chain[i].message_ref);
            return result;
        }
        chain_messages.push_back(line);
    }

    std::set<std::string> protected_turns(input.protected_turn_ids.begin(),
                                          input.protected_turn_ids.end());
    for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
        if (action.turn_id.empty() || ToolStatusTerminal(action.folded_status)) {
            continue;
        }
        protected_turns.insert(action.turn_id);  // 未收口工具所在 turn 整轮保护
    }

    std::vector<PlanBlock> blocks;
    for (const MessageLine* line : chain_messages) {
        const bool starts_new =
            blocks.empty() || blocks.back().turn_id.has_value() != line->turn_id.has_value() ||
            (line->turn_id.has_value() && blocks.back().turn_id != line->turn_id);
        if (starts_new) {
            PlanBlock block;
            block.turn_id = line->turn_id;
            block.summary_head = blocks.empty() && !line->turn_id.has_value();
            blocks.push_back(std::move(block));
        }
        blocks.back().messages.push_back(line);
        blocks.back().tokens += EstimateMessageTokens(*line);
    }

    std::size_t boundary = blocks.size();  // [0,boundary) removed;之后 retained
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].turn_id.has_value() &&
            protected_turns.count(*blocks[i].turn_id) > 0) {
            boundary = i;
            break;
        }
    }

    // 工具配对自愈(§4.64"边界跨工具配对就继续向前扩大保留范围"):
    // 同一 action 的链上消息若跨界,把界点前移到最早牵连块。
    std::unordered_map<std::string, std::size_t> block_of_message;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        for (const auto* message : blocks[i].messages) {
            block_of_message[message->message_id] = i;
        }
    }
    bool pairing_stable = false;
    while (!pairing_stable) {
        pairing_stable = true;
        for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
            std::optional<std::size_t> removed_side;
            std::optional<std::size_t> retained_side;
            for (const auto& version : action.message_versions) {
                const auto it = block_of_message.find(version.message_id);
                if (it == block_of_message.end()) {
                    continue;
                }
                if (it->second < boundary) {
                    removed_side = removed_side.has_value() ? std::min(*removed_side, it->second)
                                                            : it->second;
                } else {
                    retained_side = retained_side.has_value() ? std::min(*retained_side, it->second)
                                                              : it->second;
                }
            }
            if (removed_side.has_value() && retained_side.has_value()) {
                boundary = *removed_side;
                result.notes.push_back("工具配对跨拟定边界,保留范围前移到第 " +
                                       std::to_string(*removed_side + 1) + " 块");
                pairing_stable = false;
                break;
            }
        }
    }
    // 覆盖/配对校验按消息集合判定(回退会移动块,boundary 下标不作准)。

    ScopePlan plan;
    for (std::size_t i = 0; i < boundary; ++i) {
        plan.removed.push_back(std::move(blocks[i]));
    }
    for (std::size_t i = boundary; i < blocks.size(); ++i) {
        plan.retained.push_back(std::move(blocks[i]));
    }
    plan.protected_turns.assign(protected_turns.begin(), protected_turns.end());

    // 没有可摘要化历史(空链/全受保护):rejected(no_eligible_history),
    // 一次模型都不调(§4.9)。
    if (plan.removed.empty()) {
        session.Freeze(writer, {}, plan.RetainedIds(), plan.protected_turns);
        result.terminal_kind = "rejected";
        result.reason = "no_eligible_history";
        return result;
    }

    // ---- 3. 发送前容量门禁与整轮回退(§4.37/§4.64)。
    // Ic + Oc + Mc <= Cc;Cc 未知(0)不做门禁,如实标注。撞窗:先移出
    // 仅供参考的保留尾部(R),再从可压缩历史(H)最新一轮起整轮移出组
    // 成连续保留尾部 K;目标一次至少让出 minRetreatTokens;每步落
    // compact.range.retreated;退空仍不过则 rejected,不发请求碰运气。 ----
    const std::string special_system =
        input.special_system.empty() ? DefaultSpecialSystem() : input.special_system;
    const std::uint64_t system_tokens = EstimateUtf8Div4(special_system);

    const auto build_instruction = [&](const ScopePlan& current, bool reference_included) {
        std::string instruction = "请把以下材料压缩成交接摘要(范围如下),并按系统指令的"
                                  "格式输出正文与末尾的 JSON manifest。\n";
        instruction += "压缩范围:共 " + std::to_string(current.RemovedIds().size()) + " 条消息";
        {
            std::vector<std::string> turn_labels;
            for (const auto& block : current.removed) {
                turn_labels.push_back(block.turn_id.has_value() ? *block.turn_id : "旧摘要");
            }
            if (!turn_labels.empty()) {
                instruction += "(涵盖 " + turn_labels.front();
                if (turn_labels.size() > 1) {
                    instruction += " 至 " + turn_labels.back();
                }
                instruction += ")";
            }
        }
        instruction += "。";
        if (!current.removed.empty() && current.removed.front().summary_head) {
            instruction += "材料开头的既有旧摘要没有永久保留特权:把它与其后历史合并成"
                           "一份新摘要,不要叠加出两份。";
        }
        if (reference_included && !current.retained.empty()) {
            instruction += "\n其后另有标注为\"保留尾部\"的近期消息,仅作参考上下文,"
                           "不要把这段整段复制进摘要,也不要为它新开节。";
        } else if (!current.retained.empty()) {
            instruction += "\n保留尾部(共 " + std::to_string(current.RetainedIds().size()) +
                           " 条消息)不在本次材料里,新摘要不得编造其中的进展。";
        }
        if (!requirements.required_open_items.empty()) {
            instruction += "\n\n以下是当前仍未完成的待办,每一项必须逐字(只许调整空白)"
                           "出现在 manifest 的 open_items 数组里,一项都不许丢、不许改写:";
            for (std::size_t i = 0; i < requirements.required_open_items.size(); ++i) {
                instruction += "\n" + std::to_string(i + 1) + ". " + requirements.required_open_items[i];
            }
        }
        if (!input.focus.empty()) {
            instruction += "\n重点保留:" + input.focus;
        }
        return instruction;
    };

    const auto estimate_input = [&](const ScopePlan& current, bool reference_included,
                                    const std::string& instruction) {
        std::uint64_t tokens = system_tokens + EstimateUtf8Div4(instruction);
        tokens += current.RemovedTokens();
        if (reference_included) {
            tokens += current.RetainedTokens();
        }
        return tokens;
    };

    bool reference_included = true;
    const bool gate_active = profile.compact_window_tokens > 0;
    result.gate_checked = gate_active;
    result.window_unknown = !gate_active;
    if (!gate_active) {
        result.notes.push_back("压缩模型窗口未知,发送前门禁未校验");
    }
    int plan_revision = 0;
    if (gate_active) {
        while (true) {
            const std::uint64_t input_tokens =
                estimate_input(plan, reference_included, build_instruction(plan, reference_included));
            const std::uint64_t budget =
                profile.compact_window_tokens > profile.compact_output_reserve_tokens +
                                                     profile.compact_margin_tokens
                    ? profile.compact_window_tokens - profile.compact_output_reserve_tokens -
                          profile.compact_margin_tokens
                    : 0;
            if (input_tokens <= budget) {
                break;
            }
            if (!reference_included && plan.removed.size() <= 1) {
                finish_rejected("input_capacity_exceeded");
                result.notes.push_back("压缩输入估 " + std::to_string(input_tokens) +
                                       " tokens,预算 " + std::to_string(budget) +
                                       ",可回退前缀已退空,停止摘要尝试");
                return result;
            }
            if (plan_revision >= profile.max_retreat_steps) {
                finish_rejected("retreat_budget_exhausted");
                result.notes.push_back("回退修订已达上限 " + std::to_string(profile.max_retreat_steps) +
                                       " 仍装不下,停止摘要尝试");
                return result;
            }
            // 一次回退:至少让出 minRetreatTokens;整轮较大允许超出,
            // 不拆工具原子组(块即整轮/整段)。
            std::uint64_t freed = 0;
            std::vector<std::string> retreated_ids;
            std::vector<std::string> retreated_turns;
            const bool dropped_reference = reference_included && !plan.retained.empty();
            if (reference_included) {
                freed += plan.RetainedTokens();
                reference_included = false;
            }
            while (freed < profile.min_retreat_tokens && plan.removed.size() > 1) {
                PlanBlock block = std::move(plan.removed.back());
                plan.removed.pop_back();
                freed += block.tokens;
                if (block.turn_id.has_value()) {
                    retreated_turns.push_back(*block.turn_id);
                }
                for (const auto* message : block.messages) {
                    retreated_ids.push_back(message->message_id);
                }
                plan.retained.insert(plan.retained.begin(), std::move(block));
            }
            if (!dropped_reference && retreated_ids.empty()) {
                // no_change 候选跳过(§4.64):没有可退的内容,不记事件、
                // 不原样重发,直接收场。
                finish_rejected("input_capacity_exceeded");
                result.notes.push_back("压缩输入估 " + std::to_string(input_tokens) +
                                       " tokens,预算 " + std::to_string(budget) +
                                       ",无可回退前缀,停止摘要尝试");
                return result;
            }
            ++plan_revision;
            result.retreat_steps = plan_revision;
            trajectory::v3::EventDraft retreated;
            retreated.kind = trajectory::v3::EventKindV3::CompactRangeRetreated;
            retreated.turn_id = session.turn_id();
            retreated.parent_turn_id = input.parent_turn_id;
            retreated.compact_id = session.compact_id();
            const std::uint64_t input_tokens_after =
                estimate_input(plan, reference_included, build_instruction(plan, reference_included));
            retreated.payload = nlohmann::json::object(
                {{"planRevision", plan_revision},
                 {"sourceContextRevision", writer.context().revision},
                 {"lastFailedRequestId", nullptr},  // 本地预检:无前次请求
                 {"retreatTargetTokens", profile.min_retreat_tokens},
                 {"estimatedInputTokensBefore", input_tokens},
                 {"estimatedInputTokensAfter", input_tokens_after},
                 {"freedTokens", freed},
                 {"gate", nlohmann::json::object({{"windowTokens", profile.compact_window_tokens},
                                                  {"outputReserveTokens",
                                                   profile.compact_output_reserve_tokens},
                                                  {"marginTokens", profile.compact_margin_tokens}})},
                 {"summarizedMessageRefs", IdsToJson(plan.RemovedIds())},
                 {"referenceMessageRefs",
                  reference_included ? IdsToJson(plan.RetainedIds()) : nlohmann::json::array()},
                 {"retainedMessageRefs", IdsToJson(plan.RetainedIds())},
                 {"retreatedTurnIds", IdsToJson(retreated_turns)},
                 {"retreatedMessageRefs", IdsToJson(retreated_ids)},
                 {"estimator", profile.estimator}});
            const WriteReceipt receipt =
                writer.AppendEvent(std::move(retreated), trajectory::v3::Durability::ProcessCrash);
            if (receipt.status != WriteReceipt::Status::Committed) {
                finish_failed("compact.retreat_event_write_failed: " + receipt.error_code);
                return result;
            }
        }
    }
    const std::string instruction = build_instruction(plan, reference_included);

    // ---- 4. 冻结源版本与压缩/保留范围(§4.5 行 1:执行时冻结)。 ----
    const auto freeze =
        session.Freeze(writer, plan.RemovedIds(), plan.RetainedIds(), plan.protected_turns);
    if (!freeze.eligible) {
        result.terminal_kind = "rejected";
        result.reason = "no_eligible_history";
        return result;
    }
    if (freeze.event.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.started_write_failed: " + freeze.event.error_code);
        return result;
    }

    // ---- 5. 组装压缩请求(§4.6/§4.41):压缩专用 system + 材料清单
    //(链序原样,角色/正文不改)+ 末尾指令;prepared 先落稳才许发。 ----
    const WriteReceipt special_system_receipt = session.WriteSpecialSystem(writer, special_system);
    if (special_system_receipt.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.special_system_write_failed: " + special_system_receipt.error_code);
        return result;
    }
    const WriteReceipt prompt_receipt = session.AppendPrompt(
        writer, nlohmann::json::object({{"role", "user"}, {"content", instruction}}));
    if (prompt_receipt.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.prompt_write_failed: " + prompt_receipt.error_code);
        return result;
    }
    std::vector<std::string> input_ids = plan.RemovedIds();
    if (reference_included) {
        const std::vector<std::string> retained_ids = plan.RetainedIds();
        input_ids.insert(input_ids.end(), retained_ids.begin(), retained_ids.end());
    }
    input_ids.push_back(prompt_receipt.id);
    const std::string request_id = writer.NewRequestId();
    const std::string step_id = writer.NewStepId();
    nlohmann::json provider_snapshot = nlohmann::json::object(
        {{"provider", profile.provider},
         {"wire", profile.wire},
         {"model", profile.model},
         {"outputReserveTokens", profile.compact_output_reserve_tokens},
         {"windowTokens", gate_active ? nlohmann::json(profile.compact_window_tokens)
                                      : nlohmann::json(nullptr)},
         {"estimatedInputTokens",
          estimate_input(plan, reference_included, instruction)}});
    const WriteReceipt prepared = writer.PrepareRequest(
        request_id, session.turn_id(), step_id, "compact", special_system_receipt.id, input_ids,
        std::move(provider_snapshot), session.compact_id());
    if (prepared.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.prepared_write_failed: " + prepared.error_code);
        return result;
    }

    // ---- 6. 发给压缩模型;收回复按 assistant 留档(候选,§4.5 行 5)。
    // 无正文/失败不造空 assistant;截断候选标 truncated 不 applied。 ----
    std::vector<nlohmann::json> material;
    material.reserve(input_ids.size());
    for (const auto& id : input_ids) {
        if (id == prompt_receipt.id) {
            material.push_back(nlohmann::json::object({{"role", "user"}, {"content", instruction}}));
            continue;
        }
        const MessageLine* line = ledger.FindMessage(id);
        if (line == nullptr) {
            finish_failed("compact.material_missing: " + id);
            return result;
        }
        material.push_back(line->message);
    }
    ++result.model_calls;
    const V3CompactModelReply reply = client.Send(special_system, material);
    if (!reply.ok) {
        finish_failed(reply.error_code.empty() ? "provider_error" : reply.error_code);
        if (!reply.error_detail.empty()) {
            result.notes.push_back("压缩模型请求失败: " + reply.error_detail);
        }
        return result;
    }
    if (NormalizeWhitespace(reply.text).empty()) {
        // HTTP 200 后无响应正文/纯空白一类:只记失败终态,不造空 assistant。
        finish_failed("empty_compact_response");
        return result;
    }
    const WriteReceipt candidate = session.WriteCandidate(
        writer, nlohmann::json::object({{"role", "assistant"}, {"content", reply.text}}),
        request_id, step_id, profile.provider, profile.wire, profile.model,
        reply.usage.has_value() ? *reply.usage : nlohmann::json(nullptr),
        trajectory::v3::Durability::PowerLoss,
        reply.truncated ? std::optional(trajectory::v3::CompletionStatus::Truncated) : std::nullopt);
    if (candidate.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.candidate_write_failed: " + candidate.error_code);
        return result;
    }

    // ---- 7. 校验必需内容(§4.7/§4.8):逐项 checks,失败带详情;
    // 未通过不 applied、不改内存、不显示完成。 ----
    const WriteReceipt validation_started = session.StartValidation(writer);
    if (validation_started.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.validation_write_failed: " + validation_started.error_code);
        return result;
    }
    std::vector<nlohmann::json> checks;
    bool all_passed = true;
    const auto add_check = [&](const char* code, bool passed, std::string detail = std::string()) {
        nlohmann::json check = nlohmann::json::object({{"code", code}, {"passed", passed}});
        if (!detail.empty()) {
            check["detail"] = std::move(detail);
        }
        checks.push_back(std::move(check));
        all_passed = all_passed && passed;
    };

    // 收益/容量用同一把尺先算好(§4.11:同一时点、同一估算口径)。
    std::uint64_t system_message_tokens = 0;
    if (const MessageLine* system_line = ledger.FindMessage(writer.context().system_message_ref)) {
        system_message_tokens = EstimateMessageTokens(*system_line);
    }
    const std::uint64_t tokens_before = system_message_tokens + plan.RemovedTokens() +
                                        plan.RetainedTokens();
    const std::uint64_t summary_tokens = EstimateUtf8Div4(reply.text);
    const std::uint64_t tokens_after =
        system_message_tokens + summary_tokens + plan.RetainedTokens();

    // 7.1 输出上限:截断的候选不是完整摘要(§4.64 表行 3)。
    add_check("output_limit", !reply.truncated,
              reply.truncated ? "压缩回复被输出上限截断(finish_reason=length),候选按不完整留档"
                              : std::string());
    // 7.2 内容结构:manifest 围栏 + 必需字段/类型/非空(§4.8 表行 1)。
    std::optional<nlohmann::json> manifest = ParseTrailingJsonFence(reply.text);
    if (!manifest.has_value()) {
        add_check("content_structure", false, "摘要末尾缺可解析的 ```json manifest 围栏");
    } else {
        std::string missing;
        for (const auto& field : requirements.required_string_fields) {
            const auto it = manifest->find(field);
            if (it == manifest->end() || !it->is_string() ||
                NormalizeWhitespace(it->get<std::string>()).empty()) {
                missing += (missing.empty() ? "" : ",") + field + "(非空字符串)";
            }
        }
        for (const auto& field : requirements.required_array_fields) {
            const auto it = manifest->find(field);
            if (it == manifest->end() || !it->is_array()) {
                missing += (missing.empty() ? "" : ",") + field + "(数组)";
            }
        }
        add_check("content_structure", missing.empty(),
                  missing.empty() ? std::string() : "manifest 缺必需字段或类型不符: " + missing);
    }
    // 7.3 正文下限(防 prefill 残次品,与 v2 同门槛)。
    add_check("body_length", CountUtf8Chars(reply.text) >= requirements.min_summary_chars,
              "摘要正文不足 " + std::to_string(requirements.min_summary_chars) + " 码点");
    // 7.4 待办守恒:requiredOpenItems 逐字在场(§4.7"覆盖范围")。
    if (!requirements.required_open_items.empty() && manifest.has_value()) {
        std::vector<std::string> open_items;
        if (const auto it = manifest->find("open_items");
            it != manifest->end() && it->is_array()) {
            for (const auto& item : *it) {
                if (item.is_string()) {
                    open_items.push_back(item.get<std::string>());
                }
            }
        }
        std::string lost;
        for (const auto& required : requirements.required_open_items) {
            const std::string needle = NormalizeWhitespace(required);
            bool found = false;
            for (const auto& item : open_items) {
                if (NormalizeWhitespace(item) == needle) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                lost += (lost.empty() ? "" : ";") + required;
            }
        }
        add_check("open_items_conservation", lost.empty(),
                  lost.empty() ? std::string() : "manifest.open_items 漏了必须守恒的待办: " + lost);
    } else {
        add_check("open_items_conservation", true);
    }
    // 7.5 覆盖范围:removed ∪ retained 恰好盖住链上全部非 system 消息,
    // 且互斥(§4.8 集合约束)。
    {
        std::set<std::string> covered;
        bool overlap = false;
        for (const auto& id : plan.RemovedIds()) {
            overlap = overlap || !covered.insert(id).second;
        }
        for (const auto& id : plan.RetainedIds()) {
            overlap = overlap || !covered.insert(id).second;
        }
        bool full_cover = covered.size() == chain_messages.size();
        if (full_cover) {
            for (const auto* message : chain_messages) {
                if (covered.count(message->message_id) == 0) {
                    full_cover = false;
                    break;
                }
            }
        }
        add_check("coverage", full_cover && !overlap,
                  (full_cover && !overlap)
                      ? std::string()
                      : "removed/retained 未互斥盖住当前链(共 " +
                            std::to_string(chain_messages.size()) + " 条)");
    }
    // 7.6 工具配对:压缩边界不劈开调用与结果(§4.8 表行 4)。按最终
    // removed/retained 集合判定——回退移动过块,原始 boundary 下标不作准。
    {
        const std::vector<std::string> removed_ids_now = plan.RemovedIds();
        const std::unordered_set<std::string> removed_set(removed_ids_now.begin(),
                                                          removed_ids_now.end());
        const std::vector<std::string> retained_ids_now = plan.RetainedIds();
        const std::unordered_set<std::string> retained_set(retained_ids_now.begin(),
                                                           retained_ids_now.end());
        bool pairing_ok = true;
        for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
            bool in_removed = false;
            bool in_retained = false;
            for (const auto& version : action.message_versions) {
                in_removed = in_removed || removed_set.count(version.message_id) > 0;
                in_retained = in_retained || retained_set.count(version.message_id) > 0;
            }
            if (in_removed && in_retained) {
                pairing_ok = false;
                break;
            }
        }
        add_check("tool_pairing", pairing_ok,
                  pairing_ok ? std::string() : "压缩边界劈开了工具调用与结果的配对");
    }
    // 7.7 源未变化:校验时点再核一遍(§4.8 表行 7;Apply 内还会再核)。
    add_check("source_revision", writer.context().revision == session.source_revision(),
              writer.context().revision == session.source_revision()
                  ? std::string()
                  : "源上下文在校验前已变化(期望 revision " +
                        std::to_string(session.source_revision()) + ",当前 " +
                        std::to_string(writer.context().revision) + ")");
    // 7.8 收益:候选新上下文须严格变小(§4.8 表行 6;收益不足不空转)。
    add_check("benefit", tokens_after < tokens_before,
              "估算前后 " + std::to_string(tokens_before) + " -> " + std::to_string(tokens_after) +
                  " tokens,候选没有严格变小");
    // 7.9 主请求预算:第二道门禁(S+Q新+K+O主<=C主,§4.64);窗口
    // 未知(0)时跳过并如实记 false 于 gate_checked——不假装核过。
    if (profile.main_window_tokens > 0) {
        const bool fits = tokens_after + profile.main_output_reserve_tokens <=
                          profile.main_window_tokens;
        add_check("post_compact_budget", fits,
                  fits ? std::string()
                       : "压缩后上下文 " + std::to_string(tokens_after) + " + 主输出预留 " +
                             std::to_string(profile.main_output_reserve_tokens) + " 超主窗口 " +
                             std::to_string(profile.main_window_tokens));
    }

    const WriteReceipt validation_completed =
        session.CompleteValidation(writer, all_passed, std::move(checks));
    if (validation_completed.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.validation_write_failed: " + validation_completed.error_code);
        return result;
    }
    if (!all_passed) {
        finish_rejected("validation_failed");
        result.tokens_before = tokens_before;
        result.tokens_after = tokens_after;  // 候选前后估算进详情,不展示成已压缩
        return result;
    }

    // ---- 8. applied 原子提交(§4.8:摘要先落稳,applied 按 PowerLoss;
    // 库层提交前再核源版本,变了 rejected(source_conflict))。 ----
    result.tokens_before = tokens_before;
    result.tokens_after = tokens_after;
    nlohmann::json token_metric = nlohmann::json::object(
        {{"estimator", profile.estimator},
         {"estimatorVersion", 1},
         {"scope", "model_input"},
         {"includesSystem", true},
         {"includesOutputReserve", false}});
    const auto apply = session.Apply(writer, reply.text, tokens_before, tokens_after,
                                     std::move(token_metric));
    if (!apply.ok) {
        result.terminal_kind = apply.error == "compact.source_conflict" ? "rejected" : "failed";
        result.reason = apply.error;
        return result;
    }
    result.applied = true;
    result.terminal_kind = "applied";
    result.notes.push_back("上下文已压缩(估算): " + std::to_string(tokens_before) + " -> " +
                           std::to_string(tokens_after) + " tokens");
    return result;
}

std::uint64_t EstimateV3TokensUtf8Div4(std::string_view utf8) {
    return EstimateUtf8Div4(utf8);
}

}  // namespace lubancode::runtime
