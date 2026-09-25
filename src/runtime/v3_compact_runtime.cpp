// v3 compact 运行时实现:全链驱动见头注。合同细节引设计总纲
// todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo(§4.6-4.10
// 范围冻结/请求形状/校验/applied;§4.37-4.41 触发/容量/连续压缩;
// §4.64 撞窗整轮回退)。
#include "runtime/v3_compact_runtime.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "trajectory/canonical_json.hpp"
#include "hooks/hash.hpp"
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

std::uint64_t EstimateJsonMessageTokens(const nlohmann::json& message) {
    const auto canonical = trajectory::CanonicalJsonDump(message);
    if (!canonical.has_value()) {
        return 0;
    }
    return EstimateUtf8Div4(*canonical);
}

std::uint64_t EstimateMessageTokens(const MessageLine& line) {
    return EstimateJsonMessageTokens(line.message);
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
        "已证实事实、失败尝试及其原因、未完成事项;逐项记清已执行操作、副作用、"
        "成功/失败/未知状态及证据引用，不得暗示已执行工具尚未执行。\n"
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
    std::optional<std::string> step_id;
    std::optional<std::string> turn_id;  // nullopt = 游离节点(旧摘要等)
    bool summary_head = false;           // 链头紧随 system 的游离段 = 当前旧摘要 Q
    std::vector<const MessageLine*> messages;  // 链序
    std::uint64_t tokens = 0;
};

struct ScopePlan {
    std::vector<PlanBlock> removed;    // 压缩材料(含旧摘要 Q,§4.41 无永久保留特权)
    std::vector<PlanBlock> retained;   // 保留尾部 R(未完成/受保护 turn)+ 回退并入的 K
    std::vector<std::string> protected_turns;
    nlohmann::json step_scope = nlohmann::json::object();

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

// ---------------------------------------------------------------------------
// 签名/加密思考载荷:规范块的"看见"层(A/B 两道检查共用)
// ---------------------------------------------------------------------------
//
// 写侧早已保真保存 thinking 的 signature/responses_item
//(trajectory_session.cpp 的主桥/旁路桥/v2 投影三处),恢复侧也读回。旧压缩
// 门禁按 JSON 键名递归扫全链、见着就一刀切拒——工具参数里的普通
// signature 字段也被误报,且不分材料投影与主模型回放两个阶段。这里换成
// 只认规范消息块与协议元数据:content 块数组(§4.42)里 type=="thinking"
// 的块的 signature / responses_item,以及不透明思考块(redacted_thinking /
// reasoning.encrypted;现写侧不落,防御性认)。业务 JSON(工具入参/结果)
// 里的同名键不递归、不误报,空串/空值同样不算。

struct CanonicalThinkingPayload {
    int signed_thinking = 0;  // thinking.signature 非空(anthropic 签名/gemini thoughtSignature 中立层同位)
    int native_items = 0;     // thinking.responses_item 非 null(Responses 原生 reasoning item)
    int opaque_blocks = 0;    // redacted_thinking / reasoning.encrypted 一类不透明块

    void Add(const CanonicalThinkingPayload& other) {
        signed_thinking += other.signed_thinking;
        native_items += other.native_items;
        opaque_blocks += other.opaque_blocks;
    }
    bool Any() const { return signed_thinking > 0 || native_items > 0 || opaque_blocks > 0; }
    int Total() const { return signed_thinking + native_items + opaque_blocks; }
};

CanonicalThinkingPayload ScanCanonicalThinkingPayload(const nlohmann::json& message) {
    CanonicalThinkingPayload payload;
    const auto content_it = message.find("content");
    if (content_it == message.end() || !content_it->is_array()) {
        return payload;  // 纯文本 content:没有规范块数组可扫
    }
    for (const auto& part : *content_it) {
        if (!part.is_object()) {
            continue;
        }
        const std::string type = part.value("type", std::string());
        if (type == "thinking") {
            const auto signature_it = part.find("signature");
            if (signature_it != part.end() && signature_it->is_string() &&
                !signature_it->get<std::string>().empty()) {
                ++payload.signed_thinking;
            }
            const auto item_it = part.find("responses_item");
            if (item_it != part.end() && !item_it->is_null() && item_it->is_object()) {
                ++payload.native_items;
            }
        } else if (type == "redacted_thinking" || type == "reasoning.encrypted") {
            ++payload.opaque_blocks;
        }
    }
    return payload;
}

// ---------------------------------------------------------------------------
// 工具配对索引:材料消息集内 assistant 声明的调用号 + actionId→provider 号桥
// ---------------------------------------------------------------------------
//
// 写侧按 §4.15/§4.18 双号落账:assistant.tool_calls[].id 带 provider 原生
// 号,tool 结果消息 tool_call_id 带内部 action 号,配对靠 FoldToolActions
// 的 providerToolCallId 桥换算。主对话的内存组装与压缩后换账
//(EffectiveConversationFromV3)各自做了换算,compact 摘要材料的账本回放
// 此前没做——role:"tool" 消息按内部号原样出网,provider 端查无此号(案发
// 2013:tool id(action-000001) not found,/compact 对凡带工具调用的会话
// 全链不可用)。投影按同一份桥把 tool_call_id 换算回 provider 号,与材料
// 内 assistant 声明同号;配对够不着的(声明不在材料集、桥缺号、账本残缺)
// 按孤儿兜底投影成普通文本,不再以 role:"tool"/tool_call_id 形态出网。
// 只建视图,原始账本一字不动。
struct MaterialToolPairingIndex {
    std::set<std::string> declared_call_ids;  // 材料内 assistant 声明的调用号(provider 号)
    std::map<std::string, std::string> action_to_provider;  // actionId → provider 原生号
    std::map<std::string, std::string> action_tool_names;   // actionId → 工具名(孤儿标注用)
};

MaterialToolPairingIndex BuildMaterialToolPairingIndex(
    const std::vector<std::string>& message_ids, const trajectory::v3::V3Ledger& ledger) {
    MaterialToolPairingIndex index;
    for (const auto& id : message_ids) {
        const MessageLine* line = ledger.FindMessage(id);
        if (line == nullptr) {
            continue;  // 压缩指令 prompt 一类不在账上的 id 不进配对面
        }
        const auto calls_it = line->message.find("tool_calls");
        if (calls_it != line->message.end() && calls_it->is_array()) {
            for (const auto& call : *calls_it) {
                if (!call.is_object()) {
                    continue;
                }
                const std::string call_id = call.value("id", std::string());
                if (!call_id.empty()) {
                    index.declared_call_ids.insert(call_id);
                }
            }
        }
        // 防御面:assistant content 数组里的 tool_use 块(anthropic 原生形
        // 态;现写侧统一 tool_calls,见了也认)。
        const auto content_it = line->message.find("content");
        if (content_it != line->message.end() && content_it->is_array()) {
            for (const auto& part : *content_it) {
                if (part.is_object() && part.value("type", std::string()) == "tool_use") {
                    const std::string use_id = part.value("id", std::string());
                    if (!use_id.empty()) {
                        index.declared_call_ids.insert(use_id);
                    }
                }
            }
        }
    }
    for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
        if (action.provider_tool_call_id.has_value() && !action.provider_tool_call_id->empty()) {
            index.action_to_provider[action.tool_call_id] = *action.provider_tool_call_id;
        }
        if (action.tool_name.has_value() && !action.tool_name->empty()) {
            index.action_tool_names[action.tool_call_id] = *action.tool_name;
        }
    }
    return index;
}

// 配对裁决:返回出网该用的调用号(空串 = 孤儿,须按文本投影)。先认桥接
// 出的 provider 号(与材料内 assistant 声明同号),再认本号直配(同号落
// 账的 fixture/老档形态)。
std::string ResolvePairedToolCallId(const std::string& tool_call_id,
                                    const MaterialToolPairingIndex& pairing) {
    const auto bridged = pairing.action_to_provider.find(tool_call_id);
    if (bridged != pairing.action_to_provider.end() &&
        pairing.declared_call_ids.count(bridged->second) > 0) {
        return bridged->second;
    }
    if (pairing.declared_call_ids.count(tool_call_id) > 0) {
        return tool_call_id;
    }
    return std::string();
}

// 工具结果正文取文本:content 是字符串原样;块数组只拼 text 块(图片等
// 载荷在文本投影里没有载体,不编内容)。
std::string ToolResultBodyText(const nlohmann::json& content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (!content.is_array()) {
        return std::string();
    }
    std::string body;
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string()) == "text") {
            const auto text_it = part.find("text");
            if (text_it != part.end() && text_it->is_string()) {
                if (!body.empty()) {
                    body.push_back('\n');
                }
                body += text_it->get<std::string>();
            }
        }
    }
    return body;
}

// A 阶段投影:账本消息 → 摘要请求的材料视图。规范 thinking 块的协议载荷
// (signature/responses_item)不出网——旧签名跨模型重放会被服务端拒,也不
// 许伪装成摘要模型的新思考;可读思考正文按普通文本材料保留(标明来源,
// 历史文本不冒充摘要指令),不透明块不解密、不猜内容、不进材料。工具结
// 果按配对索引换号/兜底(见 MaterialToolPairingIndex 注)。只建视图,原始
// 账本一字不动:不删签名、不改加密字节、不伪造 item id。
nlohmann::json BuildCompactMaterialView(const nlohmann::json& message,
                                        const MaterialToolPairingIndex& pairing) {
    const std::string role = message.value("role", std::string());
    // role:"tool" 消息:配对够得着 → 换算成与材料内声明同号(正文一字不
    // 动);够不着 → 孤儿,按普通文本投影(照 thinking 块 A 投影的先例标
    // 明来源),不带 role:"tool"/tool_call_id 形态出网。
    if (role == "tool") {
        const std::string tool_call_id = message.value("tool_call_id", std::string());
        const std::string paired_id = ResolvePairedToolCallId(tool_call_id, pairing);
        if (!paired_id.empty()) {
            if (paired_id == tool_call_id) {
                return message;  // 同号直配:原样保真
            }
            nlohmann::json projected = message;
            projected["tool_call_id"] = paired_id;  // action 桥换号,只动这一个键
            return projected;
        }
        std::string label = tool_call_id;
        const auto name = pairing.action_tool_names.find(tool_call_id);
        if (name != pairing.action_tool_names.end()) {
            label = name->second + "(" + tool_call_id + ")";
        }
        return nlohmann::json::object(
            {{"role", "user"},
             {"content", "[工具结果 " + label + "]\n" +
                              ToolResultBodyText(message.value("content", nlohmann::json()))}});
    }
    const auto content_it = message.find("content");
    if (content_it == message.end() || !content_it->is_array()) {
        return message;
    }
    nlohmann::json projected = message;
    nlohmann::json content = nlohmann::json::array();
    bool changed = false;
    for (const auto& part : *content_it) {
        if (part.is_object()) {
            const std::string type = part.value("type", std::string());
            if (type == "thinking") {
                changed = true;
                std::string text;
                if (const auto text_it = part.find("text");
                    text_it != part.end() && text_it->is_string()) {
                    text = text_it->get<std::string>();
                }
                if (!NormalizeWhitespace(text).empty()) {
                    content.push_back(
                        nlohmann::json{{"type", "text"}, {"text", "[历史思考记录]\n" + text}});
                }
                continue;
            }
            if (type == "redacted_thinking" || type == "reasoning.encrypted") {
                changed = true;  // 不透明载荷:材料里没有它的一席,不编内容
                continue;
            }
            // user content 里的 tool_result 块(anthropic 形态):配对裁决
            // 与 role:"tool" 消息同款——够得着换号保真,够不着按文本投影。
            if (type == "tool_result") {
                const std::string use_id = part.value("tool_use_id", std::string());
                const std::string paired_id = ResolvePairedToolCallId(use_id, pairing);
                if (!use_id.empty() && paired_id == use_id) {
                    content.push_back(part);  // 直配保真
                    continue;
                }
                changed = true;
                if (!paired_id.empty()) {
                    nlohmann::json rewritten = part;
                    rewritten["tool_use_id"] = paired_id;
                    content.push_back(std::move(rewritten));
                    continue;
                }
                const std::string body = ToolResultBodyText(part.contains("content")
                                                                ? part["content"]
                                                                : nlohmann::json());
                if (!NormalizeWhitespace(body).empty()) {
                    content.push_back(nlohmann::json{
                        {"type", "text"}, {"text", "[工具结果 " + use_id + "]\n" + body}});
                }
                continue;
            }
        }
        content.push_back(part);
    }
    if (changed) {
        projected["content"] = std::move(content);
    }
    return projected;
}

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

    // T12-C(V3-GAP-07):parent_turn_id 只认调用方递进的真实主轮号,不从
    // 链上猜"最近一枚 turn"顶包——idle 手动压缩按实际无活动主轮表达
    //(parentTurnId 落 null),turn 中途压缩由接线层递 OpenMainTurnId()。

    // 结束兜底:任何提前 return 前必须落终态(除非库层已落)。干跑
    //(session 为空)不写账,只填结果字段。
    // 拥有权注意:CompactSession 的 unique_ptr 必须活到函数尾——begin_session
    // 里的 BeginOutcome 是局部量,session 裸指针从 owner 取,owner 由本层持有。
    std::unique_ptr<CompactSession> session_owner;
    CompactSession* session = nullptr;
    const auto finish_rejected = [&](const std::string& reason) {
        if (session != nullptr) {
            session->Fail(writer, CompactSession::FailKind::Rejected, reason);
        }
        result.terminal_kind = "rejected";
        result.reason = reason;
    };
    const auto finish_failed = [&](const std::string& reason) {
        if (session != nullptr) {
            session->Fail(writer, CompactSession::FailKind::Failed, reason);
        }
        result.terminal_kind = "failed";
        result.reason = reason;
    };

    // 开场(compact.requested + 内部回合,§4.6):已有进行中的 compact 时
    // 库层拒收(busy),不另开场。干跑不开场(T12-B)。
    const auto begin_session = [&]() {
        auto begin = CompactSession::Begin(writer, input.trigger, input.reason,
                                           input.parent_turn_id, RequirementsToJson(requirements));
        result.compact_id = begin.info.compact_id;
        result.turn_id = begin.info.turn_id;
        if (!begin.info.began) {
            result.terminal_kind =
                begin.info.error.rfind("compact.busy", 0) == 0 ? "busy" : "not_begun";
            result.reason = begin.info.error;
            return false;
        }
        result.began = true;
        session_owner = std::move(begin.session);
        session = session_owner.get();
        return true;
    };

    // ---- 2. 范围计划:链序分块(system 之外),保护集 = 调用方钉的 +
    // 工具动作未收口的 turn;removed = 保护界之前全部(含旧摘要),
    // retained = 界后尾部。纯计算,不落账——真跑/干跑共用同一副牌
    //(T12-B:同一候选范围与容量规划器)。 ----
    const std::vector<trajectory::v3::ChainNode>& chain = writer.context().chain;
    std::vector<const MessageLine*> chain_messages;  // 根(system)之后,链序
    std::string plan_error;  // 链上引用缺件:真跑须先开场再落终态(失败有账)
    for (std::size_t i = 1; i < chain.size(); ++i) {
        const MessageLine* line = ledger.FindMessage(chain[i].message_ref);
        if (line == nullptr) {
            plan_error = "compact.missing_chain_ref: " + chain[i].message_ref;
            break;
        }
        chain_messages.push_back(line);
    }
    if (!plan_error.empty()) {
        if (input.dry_run || begin_session()) {
            finish_rejected(plan_error);  // 真跑已开场:compact.failed 落账
        }
        return result;  // begin_session 失败(busy/not_begun)时字段已带原因
    }

    std::set<std::string> protected_turns(input.protected_turn_ids.begin(),
                                          input.protected_turn_ids.end());
    if (input.allow_closed_step_compaction && input.parent_turn_id)
        protected_turns.insert(*input.parent_turn_id);
    for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
        if (action.turn_id.empty() || ToolStatusTerminal(action.folded_status)) {
            continue;
        }
        protected_turns.insert(action.turn_id);  // Default whole-turn protection.
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
            if (action.assistant_message_ref) {
                const auto it = block_of_message.find(*action.assistant_message_ref);
                if (it != block_of_message.end()) {
                    if (it->second < boundary) removed_side = it->second;
                    else retained_side = it->second;
                }
            }
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

    // Only enter the current turn when old turns cannot free enough space.
    // Keep every user input verbatim and the latest step. Identity comes from
    // the envelopes and action ledger, never from append sequence numbers.
    const bool old_turns_insufficient = plan.removed.empty() ||
        (profile.main_window_tokens > 0 &&
         plan.RetainedTokens() + profile.main_output_reserve_tokens +
             profile.compact_output_reserve_tokens >= profile.main_window_tokens);
    if (input.allow_closed_step_compaction && input.parent_turn_id && old_turns_insufficient) {
        const auto& current_turn = *input.parent_turn_id;
        const auto actions = trajectory::v3::FoldToolActions(ledger);
        std::optional<std::string> latest_step;
        for (const auto* line : chain_messages) {
            if (line->turn_id == input.parent_turn_id && line->step_id) latest_step = line->step_id;
        }
        std::vector<PlanBlock> keep;
        std::vector<std::string> compacted_steps;
        bool stopped = false;
        for (auto& block : plan.retained) {
            if (block.turn_id != input.parent_turn_id) {
                // Protected old turns and orphan blocks stay verbatim without
                // tripping the sticky flag: whole-turn protection of another
                // turn must not truncate the parent turn's compactable prefix
                // of closed steps.
                keep.push_back(std::move(block));
                continue;
            }
            std::vector<PlanBlock> steps;
            for (const auto* line : block.messages) {
                const bool user = line->message.value("role", std::string()) == "user";
                if (steps.empty() || user || !line->step_id ||
                    steps.back().step_id != line->step_id) {
                    PlanBlock step;
                    step.turn_id = line->turn_id;
                    step.step_id = user ? std::nullopt : line->step_id;
                    steps.push_back(std::move(step));
                }
                steps.back().messages.push_back(line);
                steps.back().tokens += EstimateMessageTokens(*line);
            }
            for (auto& step : steps) {
                // User messages are pinned but do not prevent removing old steps.
                if (!step.step_id) {
                    for (const auto* line : step.messages)
                        stopped = stopped || line->message.value("role", std::string()) != "user";
                    keep.push_back(std::move(step));
                    continue;
                }
                bool closed = step.step_id != latest_step;
                std::set<std::string> ids;
                for (const auto* line : step.messages) ids.insert(line->message_id);
                for (const auto& action : actions) {
                    if (action.turn_id != current_turn || action.step_id != *step.step_id) continue;
                    bool result_in_step = false;
                    for (const auto& version : action.message_versions)
                        result_in_step = result_in_step || ids.count(version.message_id) > 0;
                    closed = closed && ToolStatusTerminal(action.folded_status) && result_in_step &&
                        action.assistant_message_ref && ids.count(*action.assistant_message_ref) > 0;
                }
                // A declaration absent from the folded action ledger is not closed.
                for (const auto* line : step.messages) {
                    if (auto calls = line->message.find("tool_calls");
                        calls != line->message.end() && calls->is_array()) {
                        for (const auto& call : *calls) {
                            const auto id = call.value("id", std::string());
                            closed = closed && std::any_of(actions.begin(), actions.end(),
                                [&](const auto& action) { return action.tool_call_id == id &&
                                    action.turn_id == current_turn && action.step_id == *step.step_id; });
                        }
                    }
                }
                stopped = stopped || !closed;
                if (stopped) keep.push_back(std::move(step));
                else {
                    compacted_steps.push_back(*step.step_id);
                    plan.removed.push_back(std::move(step));
                }
            }
        }
        plan.retained = std::move(keep);
        if (!compacted_steps.empty()) {
            plan.protected_turns.erase(std::remove(plan.protected_turns.begin(),
                plan.protected_turns.end(), current_turn), plan.protected_turns.end());
            plan.step_scope = {{"turnId", current_turn}, {"stepIds", compacted_steps},
                              {"prefixChanged", true}};
            result.notes.push_back("Closed old steps selected; the sent prefix changes.");
        }
    }

    // ---- T12-B 干跑的数字面:结构可回收量,同一把尺(bytes/4)。只报
    // 结构数字,不编造摘要实际 token(tokens_after 恒 0)。 ----
    const auto fill_dry_numbers = [&]() {
        std::uint64_t system_message_tokens = 0;
        if (const MessageLine* system_line = ledger.FindMessage(writer.context().system_message_ref)) {
            system_message_tokens = EstimateMessageTokens(*system_line);
        }
        result.dry_run = true;
        result.tokens_before = system_message_tokens + plan.RemovedTokens() + plan.RetainedTokens();
        result.removed_messages = plan.RemovedIds().size();
        result.retained_messages = plan.RetainedIds().size();
        result.removed_tokens = plan.RemovedTokens();
        result.retained_tokens = plan.RetainedTokens();
        result.removed_turns.clear();
        for (const auto& block : plan.removed) {
            result.removed_turns.push_back(block.turn_id.has_value() ? *block.turn_id : "(旧摘要)");
        }
        result.protected_turns = plan.protected_turns;
        result.step_scope = plan.step_scope;
    };

    // 压缩专用 system 与估算材料(§4.6):真跑/干跑共用。
    const std::string special_system =
        input.special_system.empty() ? DefaultSpecialSystem() : input.special_system;
    const std::uint64_t system_tokens = EstimateUtf8Div4(special_system);

    const auto execution_records = [&](const ScopePlan& current) {
        nlohmann::json records = nlohmann::json::array();
        if (current.step_scope.empty()) return records;
        const auto ids = current.RemovedIds();
        const std::set<std::string> removed(ids.begin(), ids.end());
        for (const auto& action : trajectory::v3::FoldToolActions(ledger)) {
            if (action.turn_id != input.parent_turn_id || !action.assistant_message_ref ||
                !removed.count(*action.assistant_message_ref)) continue;
            nlohmann::json evidence = nlohmann::json::array();
            for (const auto& version : action.message_versions)
                if (removed.count(version.message_id)) evidence.push_back(version.message_id);
            if (evidence.empty()) continue;
            const auto* declaration = ledger.FindMessage(*action.assistant_message_ref);
            nlohmann::json operation = nlohmann::json::object();
            if (declaration && declaration->message.contains("tool_calls")) {
                for (const auto& call : declaration->message["tool_calls"]) {
                    const auto id = call.value("id", std::string());
                    if (id == action.tool_call_id || id == action.provider_tool_call_id.value_or(""))
                        operation = call.value("function", nlohmann::json::object());
                }
            }
            records.push_back({{"actionId", action.tool_call_id},
                {"executionStatus", action.folded_status}, {"resultOutcome", action.effective_outcome},
                {"operation", operation}, {"evidenceRefs", evidence}});
        }
        return records;
    };

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
        const auto executed = execution_records(current);
        if (!executed.empty()) {
            instruction += "\n这些工具操作已经执行。不得将它们当成待执行动作。"
                "请在末尾 manifest 的 executed_actions 字段完整复制下列执行账，"
                "保留操作参数、执行状态、选用结果状态及证据引用：\n" + executed.dump();
        }
        if (!input.focus.empty()) {
            instruction += "\n重点保留:" + input.focus;
        }
        return instruction;
    };

    // A step summary sees the selected closed groups and pinned user inputs.
    // Unclosed retained calls must not enter the auxiliary model request.
    const auto material_ids = [&](const ScopePlan& current, bool include_reference) {
        const auto removed = current.RemovedIds();
        const auto retained = current.RetainedIds();
        std::set<std::string> selected(removed.begin(), removed.end());
        if (include_reference) selected.insert(retained.begin(), retained.end());
        if (!current.step_scope.empty()) {
            for (const auto* line : chain_messages)
                if (line->turn_id == input.parent_turn_id &&
                    line->message.value("role", std::string()) == "user")
                    selected.insert(line->message_id);
        }
        std::vector<std::string> ids;
        for (const auto* line : chain_messages)
            if (selected.count(line->message_id)) ids.push_back(line->message_id);
        return ids;
    };

    // A 阶段投影的唯一取用口:估算、指纹、prepared 与实发材料全吃这份
    // 视图——容量门禁必须按最终发送视图估,不许旧材料算预算、新材料
    // 上网。material_ids 只回链上已验过的 id,这里不再判空。工具配对索
    // 引按同一份材料集算(换号/孤儿判定吃全集,不是逐条孤立投影)。
    const auto material_view = [&](const std::string& id,
                                   const MaterialToolPairingIndex& pairing) {
        return BuildCompactMaterialView(ledger.FindMessage(id)->message, pairing);
    };
    const auto material_pairing = [&](const std::vector<std::string>& ids) {
        return BuildMaterialToolPairingIndex(ids, ledger);
    };

    const auto estimate_input = [&](const ScopePlan& current, bool reference_included,
                                    const std::string& instruction) {
        std::uint64_t tokens = system_tokens + EstimateUtf8Div4(instruction);
        const std::vector<std::string> ids = material_ids(current, reference_included);
        const MaterialToolPairingIndex pairing = material_pairing(ids);
        for (const auto& id : ids)
            tokens += EstimateJsonMessageTokens(material_view(id, pairing));
        return tokens;
    };

    // P1-C(compact 旁路请求切槽):估算经宿主 PreRequest/estimate 槽位。
    // 快照口径与主线请求同一 scope(system + 有序材料 + 指令),材料按
    // A 投影视图给;槽失败即压缩收口(fail closed),不回落内置公式假装
    // 核过。input.estimate 为空 = 旧路(公式同吃投影),一字不差。
    const auto estimate_input_via_slot = [&](const ScopePlan& current, bool reference_included,
                                             const std::string& instruction)
        -> std::expected<std::uint64_t, std::string> {
        if (!input.estimate) {
            return estimate_input(current, reference_included, instruction);
        }
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["system"] = special_system;
        nlohmann::json messages = nlohmann::json::array();
        const std::vector<std::string> ids = material_ids(current, reference_included);
        const MaterialToolPairingIndex pairing = material_pairing(ids);
        for (const auto& id : ids)
            messages.push_back(material_view(id, pairing));
        snapshot["messages"] = std::move(messages);
        snapshot["instruction"] = instruction;
        auto estimated = input.estimate(snapshot);
        if (!estimated.has_value()) {
            return std::unexpected(estimated.error());
        }
        const auto tokens_it = estimated->find("estimatedInputTokens");
        if (tokens_it == estimated->end() || (!tokens_it->is_number_unsigned() &&
                                               !tokens_it->is_number_integer())) {
            return std::unexpected(
                "compact.estimate_bad_shape: 估算槽产出缺 estimatedInputTokens");
        }
        const std::int64_t tokens = tokens_it->get<std::int64_t>();
        if (tokens < 0) {
            return std::unexpected("compact.estimate_bad_shape: estimatedInputTokens 为负");
        }
        return static_cast<std::uint64_t>(tokens);
    };

    const bool gate_active = profile.compact_window_tokens > 0;
    // ---- 3. 发送前容量门禁与整轮回退(§4.37/§4.64)。
    // Ic + Oc + Mc <= Cc;Cc 未知(0)不做门禁,如实标注。撞窗:先移出
    // 仅供参考的保留尾部(R),再从可压缩历史(H)最新一轮起整轮移出组
    // 成连续保留尾部 K;目标一次至少让出 minRetreatTokens;每步落
    // compact.range.retreated;退空仍不过则 rejected,不发请求碰运气。
    // T12-B:真跑/干跑共用这一只梯子(write_events=false 时不落任何事件,
    // 回退只模拟)。 ----
    const auto run_capacity_gate = [&](bool write_events) {
        struct GateOutcome {
            bool passed = false;
            bool reference_included = true;
        };
        result.gate_checked = gate_active;
        result.window_unknown = !gate_active;
        bool reference_included = plan.step_scope.empty();
        if (!gate_active) {
            result.notes.push_back("压缩模型窗口未知,发送前门禁未校验");
            return GateOutcome{true, reference_included};
        }
        int plan_revision = 0;
        while (true) {
            const auto input_tokens_or =
                estimate_input_via_slot(plan, reference_included,
                                        build_instruction(plan, reference_included));
            if (!input_tokens_or.has_value()) {
                finish_rejected(input_tokens_or.error());
                return GateOutcome{false, reference_included};
            }
            const std::uint64_t input_tokens = *input_tokens_or;
            const std::uint64_t budget =
                profile.compact_window_tokens > profile.compact_output_reserve_tokens +
                                                     profile.compact_margin_tokens
                    ? profile.compact_window_tokens - profile.compact_output_reserve_tokens -
                          profile.compact_margin_tokens
                    : 0;
            result.estimated_input_tokens = input_tokens;
            result.gate_budget_tokens = budget;
            result.fits_budget = input_tokens <= budget;
            if (input_tokens <= budget) {
                break;
            }
            if (!reference_included && plan.removed.size() <= 1) {
                finish_rejected("input_capacity_exceeded");
                result.notes.push_back("压缩输入估 " + std::to_string(input_tokens) +
                                       " tokens,预算 " + std::to_string(budget) +
                                       ",可回退前缀已退空,停止摘要尝试");
                return GateOutcome{false, reference_included};
            }
            if (plan_revision >= profile.max_retreat_steps) {
                finish_rejected("retreat_budget_exhausted");
                result.notes.push_back("回退修订已达上限 " + std::to_string(profile.max_retreat_steps) +
                                       " 仍装不下,停止摘要尝试");
                return GateOutcome{false, reference_included};
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
                plan.retained.push_back(std::move(block));
                std::stable_sort(plan.retained.begin(), plan.retained.end(),
                    [&](const PlanBlock& a, const PlanBlock& b) {
                        return std::find(chain_messages.begin(), chain_messages.end(), a.messages.front()) <
                               std::find(chain_messages.begin(), chain_messages.end(), b.messages.front());
                    });
            }
            if (!dropped_reference && retreated_ids.empty()) {
                // no_change 候选跳过(§4.64):没有可退的内容,不记事件、
                // 不原样重发,直接收场。
                finish_rejected("input_capacity_exceeded");
                result.notes.push_back("压缩输入估 " + std::to_string(input_tokens) +
                                       " tokens,预算 " + std::to_string(budget) +
                                       ",无可回退前缀,停止摘要尝试");
                return GateOutcome{false, reference_included};
            }
            ++plan_revision;
            result.retreat_steps = plan_revision;
            if (!write_events) {
                continue;  // 干跑:回退只模拟,不落 compact.range.retreated
            }
            trajectory::v3::EventDraft retreated;
            retreated.kind = trajectory::v3::EventKindV3::CompactRangeRetreated;
            retreated.turn_id = session->turn_id();
            retreated.parent_turn_id = input.parent_turn_id;
            retreated.compact_id = session->compact_id();
            const auto input_tokens_after_or =
                estimate_input_via_slot(plan, reference_included,
                                        build_instruction(plan, reference_included));
            if (!input_tokens_after_or.has_value()) {
                finish_rejected(input_tokens_after_or.error());
                return GateOutcome{false, reference_included};
            }
            const std::uint64_t input_tokens_after = *input_tokens_after_or;
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
                return GateOutcome{false, reference_included};
            }
        }
        return GateOutcome{true, reference_included};
    };

    // stepScope 收口:回退移动过块,按最终 removed 重列 stepIds(真跑/干跑
    // 共用)。
    const auto fixup_step_scope = [&]() {
        if (plan.step_scope.empty()) {
            return;
        }
        std::vector<std::string> steps;
        for (const auto& block : plan.removed) {
            if (block.turn_id == input.parent_turn_id && block.step_id &&
                std::find(steps.begin(), steps.end(), *block.step_id) == steps.end())
                steps.push_back(*block.step_id);
        }
        if (steps.empty()) plan.step_scope = nlohmann::json::object();
        else plan.step_scope["stepIds"] = steps;
    };
    // ---- 签名/加密思考载荷的两道检查(修复合同:A 材料 / B 回放)----
    // A(摘要材料):removed 只进 BuildCompactMaterialView 的投影——签名/
    // 原生 item 不出网,可读正文按普通材料。投影不拒绝,材料天然安全;
    // removed 里的载荷不阻断整个会话。
    // B(压缩后主模型回放):扫保留尾部(不是全链——removed 已变摘要),
    // 按适配层三态裁决:Unsupported 拒绝并报阶段/模型;Unknown 放行保真
    // 回放并如实标注(不报成已确认不兼容);Supported 放行记已证实。
    // 必须在回退定界之后调用:回退移动过块,retained 以最终计划为准。
    const auto decide_payload_replay = [&]() -> bool {
        CanonicalThinkingPayload removed_payload;
        for (const auto& block : plan.removed) {
            for (const auto* message : block.messages) {
                removed_payload.Add(ScanCanonicalThinkingPayload(message->message));
            }
        }
        CanonicalThinkingPayload retained_payload;
        for (const auto& block : plan.retained) {
            for (const auto* message : block.messages) {
                retained_payload.Add(ScanCanonicalThinkingPayload(message->message));
            }
        }
        result.retained_prefix_bound_payload = retained_payload.Any();
        result.replay_support = input.replay_support;
        if (removed_payload.Any()) {
            result.notes.push_back(
                "compact.material.payload_projected: 可压范围含 " +
                std::to_string(removed_payload.Total()) +
                " 枚签名/加密思考块,摘要材料按普通文本投影,签名与原生 item 不出网");
        }
        if (!retained_payload.Any()) {
            return true;
        }
        const std::string identity =
            (profile.main_provider.empty() && profile.main_wire.empty() && profile.main_model.empty())
                ? std::string("身份未声明")
                : profile.main_provider + "/" + profile.main_wire + "/" + profile.main_model;
        switch (input.replay_support) {
            case V3CompactReplaySupport::Unsupported:
                finish_rejected("compact.replay_prefix_incompatible");
                result.notes.push_back(
                    "保留尾部含 " + std::to_string(retained_payload.Total()) +
                    " 枚签名/加密思考块;协议适配层声明主模型 " + identity +
                    " 不能在拟议前缀下回放——拒绝在压缩后续接阶段(摘要+保留尾部)");
                return false;
            case V3CompactReplaySupport::Supported:
                result.notes.push_back(
                    "compact.replay.verified: 保留尾部含 " + std::to_string(retained_payload.Total()) +
                    " 枚签名/加密思考块,协议适配层证实主模型 " + identity + " 可在拟议前缀下原样回放");
                return true;
            case V3CompactReplaySupport::Unknown:
                break;
        }
        result.notes.push_back(
            "compact.replay.unverified: 保留尾部含 " + std::to_string(retained_payload.Total()) +
            " 枚签名/加密思考块;主模型 " + identity +
            " 的拟议前缀回放兼容性未经协议适配层证实——按原样保真回放"
            "(签名/加密字节不动),不判为不兼容");
        return true;
    };

    // ---- T12-B 干跑:同一副牌只算不压,到此收场——不开场、不落任何
    // 事件、不发模型;门禁回退与签名/加密载荷的兼容决策按同一只梯子
    // 模拟(dry-run 输出与实跑同一份决策)。 ----
    if (input.dry_run) {
        if (plan.removed.empty()) {
            finish_rejected("no_eligible_history");  // 干跑无 session:只填结果字段
            fill_dry_numbers();
            return result;
        }
        const auto gate = run_capacity_gate(/*write_events=*/false);
        if (gate.passed) {
            fixup_step_scope();
            if (decide_payload_replay()) {
                result.terminal_kind = "dry_run";
            }
        }
        fill_dry_numbers();
        return result;
    }

    // ---- 1. 开场:compact.requested + 内部回合(§4.6)。 ----
    if (!begin_session()) {
        return result;
    }

    // 没有可摘要化历史(空链/全受保护):rejected(no_eligible_history),
    // 一次模型都不调(§4.9)。
    if (plan.removed.empty()) {
        session->Freeze(writer, {}, plan.RetainedIds(), plan.protected_turns);
        result.terminal_kind = "rejected";
        result.reason = "no_eligible_history";
        return result;
    }

    const auto gate = run_capacity_gate(/*write_events=*/true);
    const bool reference_included = gate.reference_included;
    if (!gate.passed) {
        return result;
    }
    fixup_step_scope();
    if (!decide_payload_replay()) {
        return result;  // B 阶段拒绝:compact.failed 已落账,链一字未动
    }
    const std::string instruction = build_instruction(plan, reference_included);

    // ---- 4. 冻结源版本与压缩/保留范围(§4.5 行 1:执行时冻结)。 ----
    const auto freeze =
        session->Freeze(writer, plan.RemovedIds(), plan.RetainedIds(), plan.protected_turns,
                       trajectory::Durability::ProcessCrash, plan.step_scope);
    if (!freeze.eligible) {
        result.terminal_kind = "rejected";
        result.reason = freeze.error.empty() ? "no_eligible_history" : freeze.error;
        return result;
    }
    if (freeze.event.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.started_write_failed: " + freeze.event.error_code);
        return result;
    }

    // ---- 5. 组装压缩请求(§4.6/§4.41):压缩专用 system + 材料清单
    //(链序原样,角色/正文不改)+ 末尾指令;prepared 先落稳才许发。 ----
    const WriteReceipt special_system_receipt = session->WriteSpecialSystem(writer, special_system);
    if (special_system_receipt.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.special_system_write_failed: " + special_system_receipt.error_code);
        return result;
    }
    const WriteReceipt prompt_receipt = session->AppendPrompt(
        writer, nlohmann::json::object({{"role", "user"}, {"content", instruction}}));
    if (prompt_receipt.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.prompt_write_failed: " + prompt_receipt.error_code);
        return result;
    }
    std::vector<std::string> input_ids = material_ids(plan, reference_included);
    input_ids.push_back(prompt_receipt.id);
    // 工具配对索引按最终材料集算一次:指纹与实发材料共用,两道口径不岔
    //(prompt 不在账上,索引里天然不进配对面)。
    const MaterialToolPairingIndex pairing = material_pairing(input_ids);
    const std::string request_id = writer.NewRequestId();
    const std::string step_id = writer.NewStepId();
    // 快照里的估算与门禁同一口径(槽路再估一次);槽失败同路收口(fail
    // closed),不拿旧数字顶包。
    const auto prepared_tokens = estimate_input_via_slot(plan, reference_included, instruction);
    if (!prepared_tokens.has_value()) {
        finish_rejected(prepared_tokens.error());
        return result;
    }
    nlohmann::json provider_snapshot = nlohmann::json::object(
        {{"provider", profile.provider},
         {"wire", profile.wire},
         {"model", profile.model},
         {"outputReserveTokens", profile.compact_output_reserve_tokens},
         {"windowTokens", gate_active ? nlohmann::json(profile.compact_window_tokens)
                                      : nlohmann::json(nullptr)},
         {"estimatedInputTokens", *prepared_tokens},
         // 材料视图与回放裁决入账:指纹吃投影、决策可追溯,dry-run/实跑/
         // resume 对得上同一份口径。v2:工具结果配对投影(换号保真/孤儿
         // 按文本兜底)并入同一视图口径。
         {"materialView", "compact-material-projection-v2"},
         {"replaySupport", V3CompactReplaySupportName(input.replay_support)},
         {"retainedPrefixBoundPayload", result.retained_prefix_bound_payload}});
    nlohmann::json fingerprint_messages = nlohmann::json::array();
    for (const auto& id : input_ids) {
        if (id == prompt_receipt.id)
            fingerprint_messages.push_back({{"role", "user"}, {"content", instruction}});
        else fingerprint_messages.push_back(material_view(id, pairing));
    }
    const auto fingerprint_source = trajectory::CanonicalJsonDump(nlohmann::json{
        {"system", special_system}, {"messages", fingerprint_messages},
        {"provider", profile.provider}, {"wire", profile.wire}, {"model", profile.model},
        {"outputReserveTokens", profile.compact_output_reserve_tokens}});
    if (!fingerprint_source) {
        finish_rejected("compact.input_fingerprint_failed");
        return result;
    }
    const auto input_fingerprint = hooks::Sha256Hex(*fingerprint_source);
    provider_snapshot["inputFingerprint"] = input_fingerprint;
    provider_snapshot["inputFingerprintAlgorithm"] = "sha256-canonical-summary-input-v1";
    provider_snapshot["recoveryAttemptLimit"] = 1;
    // A failed capacity request is durable evidence. Resume cannot erase it.
    // On the same source/model budget, retry only a strictly smaller input.
    std::set<std::string> overflow_compacts;
    for (const auto& event : ledger.events) {
        if (event.kind == trajectory::v3::EventKindV3::CompactFailed && event.compact_id &&
            event.payload.value("reason", std::string()) == "input_context_overflow")
            overflow_compacts.insert(*event.compact_id);
    }
    for (const auto& event : ledger.events) {
        if (event.kind != trajectory::v3::EventKindV3::ModelRequestPrepared ||
            !event.compact_id || !overflow_compacts.count(*event.compact_id)) continue;
        const auto& previous = event.payload;
        const bool same_route = previous.value("provider", std::string()) == profile.provider &&
            previous.value("wire", std::string()) == profile.wire &&
            previous.value("model", std::string()) == profile.model &&
            previous.value("outputReserveTokens", std::uint64_t{0}) == profile.compact_output_reserve_tokens &&
            previous.value("windowTokens", nlohmann::json()) == provider_snapshot["windowTokens"];
        if (same_route && previous.value("contextRevision", std::uint64_t{0}) == session->source_revision() &&
            (previous.value("inputFingerprint", std::string()) == input_fingerprint ||
             *prepared_tokens >= previous.value("estimatedInputTokens", std::uint64_t{0}))) {
            finish_rejected("compact.failed_input_not_smaller");
            result.notes.push_back("Rejected repeated capacity input: " + input_fingerprint);
            return result;
        }
    }
    const WriteReceipt prepared = writer.PrepareRequest(
        request_id, session->turn_id(), step_id, "compact", special_system_receipt.id, input_ids,
        std::move(provider_snapshot), session->compact_id());
    if (prepared.status != WriteReceipt::Status::Committed) {
        finish_failed("compact.prepared_write_failed: " + prepared.error_code);
        return result;
    }

    // ---- 6. 发给压缩模型;收回复按 assistant 留档(候选,§4.5 行 5)。
    // 无正文/失败不造空 assistant;截断候选标 truncated 不 applied。材料按
    // A 投影视图发(签名/原生 item 不出网;工具结果换号保真/孤儿按文本
    // 兜底)。 ----
    std::vector<nlohmann::json> material;
    material.reserve(input_ids.size());
    int orphan_tool_results = 0;
    int bridged_tool_results = 0;
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
        if (line->message.value("role", std::string()) == "tool") {
            const std::string tool_call_id = line->message.value("tool_call_id", std::string());
            const std::string paired_id = ResolvePairedToolCallId(tool_call_id, pairing);
            if (paired_id.empty()) {
                ++orphan_tool_results;
            } else if (paired_id != tool_call_id) {
                ++bridged_tool_results;
            }
        }
        material.push_back(BuildCompactMaterialView(line->message, pairing));
    }
    if (orphan_tool_results > 0) {
        result.notes.push_back(
            "compact.material.orphan_tool_projected: " + std::to_string(orphan_tool_results) +
            " 条工具结果的配对声明不在材料内,按普通文本投影(不出 role:tool/"
            "tool_call_id 形态)");
    }
    if (bridged_tool_results > 0) {
        result.notes.push_back("compact.material.tool_call_id_bridged: " +
                               std::to_string(bridged_tool_results) +
                               " 条工具结果按 action 桥换算回 provider 号出网");
    }
    ++result.model_calls;
    const V3CompactModelReply reply = client.Send(special_system, material);
    if (!reply.ok) {
        if (reply.error_code == "cancelled") {
            session->Fail(writer, CompactSession::FailKind::Cancelled, "cancelled");
            result.terminal_kind = "cancelled";
            result.reason = "cancelled";
        } else {
            finish_failed(reply.error_code.empty() ? "provider_error" : reply.error_code);
        }
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
    const WriteReceipt candidate = session->WriteCandidate(
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
    const WriteReceipt validation_started = session->StartValidation(writer);
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
    std::string main_system_text;
    if (const MessageLine* system_line = ledger.FindMessage(writer.context().system_message_ref)) {
        system_message_tokens = EstimateMessageTokens(*system_line);
        const auto content_it = system_line->message.find("content");
        if (content_it != system_line->message.end() && content_it->is_string()) {
            main_system_text = content_it->get<std::string>();
        }
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
    const auto expected_executions = execution_records(plan);
    if (!expected_executions.empty()) {
        const bool preserved = manifest && manifest->contains("executed_actions") &&
            (*manifest)["executed_actions"] == expected_executions;
        add_check("executed_actions_conservation", preserved,
                  preserved ? "" : "summary omitted or changed executed operations, outcomes or evidence");
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
            if (action.assistant_message_ref) {
                in_removed = in_removed || removed_set.count(*action.assistant_message_ref) > 0;
                in_retained = in_retained || retained_set.count(*action.assistant_message_ref) > 0;
            }
            if (in_removed && (in_retained ||
                (!plan.step_scope.empty() && action.turn_id == input.parent_turn_id &&
                 !ToolStatusTerminal(action.folded_status)))) {
                pairing_ok = false;
                break;
            }
        }
        add_check("tool_pairing", pairing_ok,
                  pairing_ok ? std::string() : "压缩边界劈开了工具调用与结果的配对");
    }
    if (!plan.step_scope.empty()) {
        const auto retained_ids = plan.RetainedIds();
        bool users_preserved = true;
        for (const auto* line : chain_messages) {
            if (line->turn_id == input.parent_turn_id &&
                line->message.value("role", std::string()) == "user")
                users_preserved = users_preserved &&
                    std::find(retained_ids.begin(), retained_ids.end(), line->message_id) != retained_ids.end();
        }
        add_check("current_turn_constraints", users_preserved,
                  users_preserved ? "" : "current user input was removed");
    }
    // 7.7 源未变化:校验时点再核一遍(§4.8 表行 7;Apply 内还会再核)。
    add_check("source_revision", writer.context().revision == session->source_revision(),
              writer.context().revision == session->source_revision()
                  ? std::string()
                  : "源上下文在校验前已变化(期望 revision " +
                        std::to_string(session->source_revision()) + ",当前 " +
                        std::to_string(writer.context().revision) + ")");
    // 7.8 收益:候选新上下文须严格变小(§4.8 表行 6;收益不足不空转)。
    add_check("benefit", tokens_after < tokens_before,
              "估算前后 " + std::to_string(tokens_before) + " -> " + std::to_string(tokens_after) +
                  " tokens,候选没有严格变小");
    // 7.9 主请求预算:第二道门禁(S+Q新+K+O主<=C主,§4.64);窗口
    // 未知(0)时跳过并如实记 false 于 gate_checked——不假装核过。
    // AR-10:注入了完整主请求评估口(input.main_request_budget)时,门禁
    // 吃口的数字——同一份工具定义、最终 system、候选历史、输出预留都在
    // 口的闭包与快照里,不再退回 bytes/4 的部分请求。口失败/形状不合同样
    // 落 failed(fail closed),不回落部分请求的旧尺假装核过。口缺场 =
    // 旧路(bytes/4 结构尺),未接线调用方行为一字不变。benefit(7.8)是
    // 结构收益尺,这道门禁是真实下一请求尺——两种统计分开标注,不混一笔。
    std::string budget_gate_estimator;
    std::uint64_t budget_gate_input_tokens = 0;
    std::uint64_t budget_gate_reserve_tokens = 0;
    if (profile.main_window_tokens > 0) {
        if (input.main_request_budget) {
            // 候选主请求快照:新摘要(applied 同形:user 携带正文)+
            // 保留尾部(链序账本原样,签名/加密载荷保真)。
            nlohmann::json candidate_messages = nlohmann::json::array();
            candidate_messages.push_back(
                nlohmann::json::object({{"role", "user"}, {"content", reply.text}}));
            const std::vector<std::string> retained_ids_now = plan.RetainedIds();
            const std::set<std::string> retained_set_now(retained_ids_now.begin(),
                                                          retained_ids_now.end());
            for (const MessageLine* line : chain_messages) {
                if (retained_set_now.count(line->message_id) > 0) {
                    candidate_messages.push_back(line->message);
                }
            }
            const auto estimated = input.main_request_budget(nlohmann::json::object(
                {{"system", main_system_text}, {"messages", std::move(candidate_messages)}}));
            bool port_usable = estimated.has_value();
            std::string port_detail;
            if (port_usable) {
                const auto tokens_it = estimated->find("estimatedInputTokens");
                if (tokens_it == estimated->end() ||
                    (!tokens_it->is_number_unsigned() && !tokens_it->is_number_integer())) {
                    port_usable = false;
                    port_detail =
                        "compact.estimate_bad_shape: 完整主请求评估口产出缺 estimatedInputTokens";
                } else if (tokens_it->get<std::int64_t>() < 0) {
                    port_usable = false;
                    port_detail = "compact.estimate_bad_shape: estimatedInputTokens 为负";
                } else {
                    budget_gate_input_tokens =
                        static_cast<std::uint64_t>(tokens_it->get<std::int64_t>());
                    budget_gate_reserve_tokens = profile.main_output_reserve_tokens;
                    const auto reserve_it = estimated->find("outputReserveTokens");
                    if (reserve_it != estimated->end()) {
                        if ((!reserve_it->is_number_unsigned() &&
                             !reserve_it->is_number_integer()) ||
                            reserve_it->get<std::int64_t>() < 0) {
                            port_usable = false;
                            port_detail =
                                "compact.estimate_bad_shape: outputReserveTokens 形状不合";
                        } else {
                            // FD-02 最终出站快照的预留压过 profile 缺省。
                            budget_gate_reserve_tokens =
                                static_cast<std::uint64_t>(reserve_it->get<std::int64_t>());
                        }
                    }
                }
            } else {
                port_detail = estimated.error();
            }
            if (!port_usable) {
                add_check("post_compact_budget", false,
                          port_detail.empty()
                              ? std::string("完整主请求评估口未产出可用数字")
                              : port_detail);
            } else {
                if (estimated->is_object()) {
                    const auto name_it = estimated->find("estimator");
                    if (name_it != estimated->end() && name_it->is_string()) {
                        budget_gate_estimator = name_it->get<std::string>();
                    }
                }
                if (budget_gate_estimator.empty()) {
                    budget_gate_estimator = "main_request_budget_port";
                }
                result.post_compact_input_tokens = budget_gate_input_tokens;
                const bool fits = budget_gate_input_tokens + budget_gate_reserve_tokens <=
                                  profile.main_window_tokens;
                add_check("post_compact_budget", fits,
                          fits ? "完整主请求估 " + std::to_string(budget_gate_input_tokens) +
                                     " + 输出预留 " + std::to_string(budget_gate_reserve_tokens) +
                                     " 在主窗口 " + std::to_string(profile.main_window_tokens) +
                                     " 内(口径:主请求评估口 " + budget_gate_estimator + ")"
                               : "完整主请求估 " + std::to_string(budget_gate_input_tokens) +
                                     " + 输出预留 " + std::to_string(budget_gate_reserve_tokens) +
                                     " 超主窗口 " + std::to_string(profile.main_window_tokens) +
                                     "(口径:主请求评估口 " + budget_gate_estimator + ")");
            }
        } else {
            const bool fits = tokens_after + profile.main_output_reserve_tokens <=
                              profile.main_window_tokens;
            add_check("post_compact_budget", fits,
                      fits ? std::string()
                           : "压缩后上下文 " + std::to_string(tokens_after) + " + 主输出预留 " +
                                 std::to_string(profile.main_output_reserve_tokens) + " 超主窗口 " +
                                 std::to_string(profile.main_window_tokens));
        }
    }

    const WriteReceipt validation_completed =
        session->CompleteValidation(writer, all_passed, std::move(checks));
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
    if (!budget_gate_estimator.empty()) {
        // AR-10:第二道门禁的真实下一请求口径单列——与上面 tokens_after
        // 的结构尺(bytes/4 前后对照)是两笔统计,各自标名,不互相冒充。
        token_metric["budgetGate"] = nlohmann::json::object(
            {{"estimator", budget_gate_estimator},
             {"estimatedInputTokens", budget_gate_input_tokens},
             {"outputReserveTokens", budget_gate_reserve_tokens},
             {"windowTokens", profile.main_window_tokens}});
    }
    const auto apply = session->Apply(writer, reply.text, tokens_before, tokens_after,
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
