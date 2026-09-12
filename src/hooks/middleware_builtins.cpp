// 内置生产槽位实现(LuaHook 单 P0-B)。估算公式 §4.36:ceil(utf8Bytes/4),
// scope=model_input_json_utf8_v1;容量判断消费估算,返回准入决定。
#include "hooks/middleware_builtins.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::hooks::middleware {

namespace {

// 快照里按块 type 剥非文本媒体(image/audio/video/file/document 二进制)。
// 保留类型与占位标记(快照核对用),不把 base64/内嵌字节算进 bytes/4。
constexpr const char* kMediaBlockTypes[] = {"image", "audio", "video", "file"};

bool IsMediaBlockType(const std::string& type) {
    for (const char* media : kMediaBlockTypes) {
        if (type == media) {
            return true;
        }
    }
    return false;
}

// 递归剥媒体:返回净化副本;发现媒体时置 flag 并记 modality 名。
nlohmann::json StripUnestimatedMedia(const nlohmann::json& value, bool* saw_media,
                                     std::vector<std::string>* modalities) {
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : value) {
            out.push_back(StripUnestimatedMedia(item, saw_media, modalities));
        }
        return out;
    }
    if (!value.is_object()) {
        return value;
    }
    // 内容块形状:{"type": "image", ...}。type 命中媒体 → 占位替换。
    const auto type_it = value.find("type");
    if (type_it != value.end() && type_it->is_string() && IsMediaBlockType(type_it->get<std::string>())) {
        if (*saw_media == false) {
            *saw_media = true;
        }
        const std::string modality = type_it->get<std::string>();
        if (std::find(modalities->begin(), modalities->end(), modality) == modalities->end()) {
            modalities->push_back(modality);
        }
        return nlohmann::json{{"type", modality}, {"unestimated", true}};
    }
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, item] : value.items()) {
        out[key] = StripUnestimatedMedia(item, saw_media, modalities);
    }
    return out;
}

std::uint64_t ReadNonNegativeInt(const nlohmann::json& object, const char* key) {
    // 整数两种存法都收(C++ 字面 int 在 nlohmann 里是 number_integer,
    // 解析 JSON 文本得到的非负整数才是 number_unsigned——只认一种会读 0)。
    const auto it = object.find(key);
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        const std::int64_t value = it->get<std::int64_t>();
        return value > 0 ? static_cast<std::uint64_t>(value) : 0;
    }
    return 0;
}

}  // namespace

// 计量对象 ≠ 请求快照(V3-REAL-04):§4.36 明令 bytes/4 只数模型输入,
// 不数日志信封、凭据与 max_output_tokens/temperature 一类非输入设置。
// 宿主递进来的快照可能带控制参数(BuildRequestSnapshotJson 的 control
// 子对象)或容量段的宿主字段(tokenEstimate/outputReserveTokens/
// contextWindowTokens)——这些键只出现在顶层,计量前先摘掉;摘不出
// 任何输入字段时报给调用方(空对象 dump 仍是 "{}",2 字节,足以暴露
// 快照形状错了,不静默当 0)。
constexpr const char* kControlOnlyTopLevelKeys[] = {
    "control", "max_tokens", "max_output_tokens", "temperature", "top_p",
    "top_k", "stop_sequences", "stream", "tokenEstimate", "outputReserveTokens",
    "contextWindowTokens", "declaredMaxOutputTokens", "effectiveOutputLimitTokens",
    "policyReserveTokens", "protocolHeadroomTokens", "outputLimitOverridden"};

nlohmann::json ComputeUtf8BytesDiv4Estimate(const nlohmann::json& model_input_snapshot) {
    bool saw_media = false;
    std::vector<std::string> modalities;
    nlohmann::json measured = model_input_snapshot;
    if (measured.is_object()) {
        for (const char* key : kControlOnlyTopLevelKeys) {
            measured.erase(key);
        }
    }
    measured = StripUnestimatedMedia(measured, &saw_media, &modalities);
    // 紧凑 JSON 序列化即 UTF-8 字节(nlohmann 默认排序键,序列化可复现);
    // 非文本媒体已剥成占位,字节里只剩类型标记。
    const std::string compact = measured.dump();
    const std::uint64_t utf8_bytes = static_cast<std::uint64_t>(compact.size());
    // §4.36:整数实现用除法 + 余数取整,不先加 3(防大整数溢出);总量一次
    // 取整,不逐条 ceil 相加。
    const std::uint64_t tokens = utf8_bytes / 4 + (utf8_bytes % 4 != 0 ? 1 : 0);

    nlohmann::json out = nlohmann::json::object();
    out["estimator"] = "utf8_bytes_div4";
    out["estimatorVersion"] = 1;
    out["measurementPurpose"] = "request_preflight";
    out["scope"] = "model_input_json_utf8_v1";
    out["encoding"] = "utf-8";
    out["rounding"] = "ceil";
    out["inputUtf8Bytes"] = utf8_bytes;
    out["estimatedInputTokens"] = tokens;
    out["coverage"] = saw_media ? "partial" : "text_proxy";
    out["unestimatedModalities"] = modalities;
    return out;
}

nlohmann::json DecideRequestCapacity(const nlohmann::json& capacity_input) {
    std::uint64_t estimated = ReadNonNegativeInt(capacity_input, "estimatedInputTokens");
    if (estimated == 0) {
        if (const auto nested = capacity_input.find("tokenEstimate"); nested != capacity_input.end()) {
            estimated = ReadNonNegativeInt(*nested, "estimatedInputTokens");
        }
    }
    const std::uint64_t reserve = ReadNonNegativeInt(capacity_input, "outputReserveTokens");
    const std::uint64_t window = ReadNonNegativeInt(capacity_input, "contextWindowTokens");

    nlohmann::json out = nlohmann::json::object();
    out["estimatedInputTokens"] = estimated;
    out["outputReserveTokens"] = reserve;
    out["contextWindowTokens"] = window;
    // 预算四字段分账(V3-REAL-07):回显进决定,日志/事件据此能把"声明
    // 上限/策略预留/判定预留/实发限额"各说各的,不再互相冒充。缺键(旧
    // 调用方直喂 JSON)不补 0——字段带出去才可核,不带不虚造。
    if (capacity_input.contains("declaredMaxOutputTokens")) {
        out["declaredMaxOutputTokens"] = capacity_input.at("declaredMaxOutputTokens");
    }
    if (capacity_input.contains("policyReserveTokens")) {
        out["policyReserveTokens"] = capacity_input.at("policyReserveTokens");
    }
    if (capacity_input.contains("effectiveOutputLimitTokens")) {
        out["effectiveOutputLimitTokens"] = capacity_input.at("effectiveOutputLimitTokens");
    }
    if (capacity_input.contains("protocolHeadroomTokens")) {
        out["protocolHeadroomTokens"] = capacity_input.at("protocolHeadroomTokens");
    }

    // 窗口未知(0)不拦:与 loop 老路"窗口未知走兜底"同一态度,容量判断
    // 不制造新的硬闸;装不下时分 recover(压历史有望救)与 reject(当前
    // 输入自身已超)两档。
    if (window == 0 || estimated + reserve + kCapacityProtocolHeadroomTokens <= window) {
        out["decision"] = "allow";
        out["reason"] = "估算 + 输出预留 + 协议余量在窗口内";
        return out;
    }
    if (estimated + kCapacityProtocolHeadroomTokens > window) {
        out["decision"] = "reject";
        out["reason"] = "输入估算自身加协议余量已超过窗口,压缩历史无济于事;须缩短输入或调低输出上限";
        return out;
    }
    out["decision"] = "recover";
    out["reason"] = "输入估算 + 输出预留超过窗口;须压缩历史/降档预览后重新准备";
    return out;
}

MiddlewareDefinition BuiltinTokenEstimateSlot() {
    MiddlewareDefinition def;
    def.point = HookPoint::PreRequest;
    def.stage = Stage::Estimate;
    def.name = std::string(kTokenEstimateSlot);
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin.token_estimate_v1";
    def.required = true;  // 槽位要求:替换实现不能解除(§4.48)
    def.priority = 100;
    def.capabilities = {"estimate.read"};
    // handler:只读冻结快照,算完结构化测量结果作为本帧产出返回(§4.36:
    // 估算只产出测量结果,不改请求)。next 走一遍(围住链尾),返回值即
    // 估算结果——链上最后一枚 estimate 项的产出成为 dispatch 值。
    def.builtin = [](const InvocationCtx&, const nlohmann::json& input,
                     NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        const nlohmann::json estimate = ComputeUtf8BytesDiv4Estimate(input);
        HandlerReturn out = HandlerReturn::Value(estimate);
        if (next.calls() == 0) {
            const auto downstream = next();
            if (downstream.kind == DownstreamOutcome::Kind::Invalid) {
                return HandlerReturn::Value(estimate);  // 观察位/无下游:估算照常产出
            }
        }
        return out;
    };
    return def;
}

MiddlewareDefinition BuiltinCapacityCheckSlot() {
    MiddlewareDefinition def;
    def.point = HookPoint::PreRequest;
    def.stage = Stage::Capacity;
    def.name = std::string(kCapacityCheckSlot);
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin.capacity_check_v1";
    def.required = true;
    def.priority = 100;
    def.after = {std::string(kTokenEstimateSlot)};  // 依赖:容量消费估算(§4.48)
    def.capabilities = {"capacity.decide"};
    def.builtin = [](const InvocationCtx&, const nlohmann::json& input,
                     NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        const nlohmann::json decision = DecideRequestCapacity(input);
        HandlerReturn out;
        out.output = decision;
        out.effects.push_back(
            Effect{EffectType::AdmissionDecision,
                   nlohmann::json{{"decision", decision.value("decision", std::string("allow"))},
                                  {"reason", decision.value("reason", std::string())}}});
        if (next.calls() == 0) {
            next();  // 围住链尾;容量段无改写权,候选一律拒
        }
        return out;
    };
    return def;
}

void AddBuiltinRequestSlots(MiddlewarePool& pool) {
    pool.AddDefinition(BuiltinTokenEstimateSlot());
    pool.AddDefinition(BuiltinCapacityCheckSlot());
}

// ---- §4.67 G3:PostTurn/goal.review(验收排程槽) ----------------------------

nlohmann::json DecideGoalReview(const nlohmann::json& review_input) {
    // 决定次序钉 §4.67.4 排验收步与 §4.67.10 竞态行:停止意图 > 预算/停态
    // > 后台等待 > 可评。缺键保守(hold),不默认放行。
    const auto decide = [&](const char* decision, std::string reason) {
        return nlohmann::json{{"decision", decision}, {"reason", std::move(reason)}};
    };
    if (!review_input.is_object()) {
        return decide("hold", "goal.review 输入不是 object");
    }
    const std::string lifecycle = review_input.value("lifecycle", std::string());
    const bool stop_requested = review_input.value("stopRequested", false);
    const bool budget_exhausted = review_input.value("budgetExhausted", false);
    if (stop_requested) {
        return decide("hold", "停止意图在账(Esc/pause 先行),不排验收续跑");
    }
    if (budget_exhausted || lifecycle == "budget_exhausted") {
        return decide("hold", "预算已尽,停新请求(连验收请求也不豁免)");
    }
    if (lifecycle.empty() || lifecycle == "paused" || lifecycle == "awaiting_user" ||
        lifecycle == "blocked" || lifecycle == "suspended_by_policy" || lifecycle == "achieved" ||
        lifecycle == "cleared" || lifecycle == "failed") {
        return decide("hold", "停态/终态(" + (lifecycle.empty() ? "未知" : lifecycle) + ")不排");
    }
    if (review_input.contains("waitTaskRefs") && review_input.at("waitTaskRefs").is_array() &&
        !review_input.at("waitTaskRefs").empty()) {
        return decide("wait", "相关后台任务未收口,先走等待路径(真实完成可唤醒)");
    }
    if (lifecycle == "waiting") {
        return decide("wait", "目标在等待态");
    }
    return decide("evaluate", "工作轮已收口,可排独立验收");
}

MiddlewareDefinition BuiltinGoalReviewSlot() {
    MiddlewareDefinition def;
    def.point = HookPoint::PostTurn;
    def.stage = Stage::Default;
    def.name = std::string(kGoalReviewSlot);
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin.goal_review_v1";
    def.required = true;  // 槽位要求:替换实现不能解除验收排程门槛
    def.priority = 100;
    def.capabilities = {"goal.review.decide"};
    // handler:只提出验收工作项(返回决定),不跑模型、不写状态——评估
    // 请求由宿主经内部请求服务调度留账(§4.67.8)。decision=evaluate 时
    // next 放行(围住链尾);wait/hold 短路:链上后续项不跑,宿主按决定
    // 走等待/暂停路径。
    def.builtin = [](const InvocationCtx&, const nlohmann::json& input,
                     NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        const nlohmann::json decision = DecideGoalReview(input);
        if (decision.value("decision", std::string()) == "evaluate" && next.calls() == 0) {
            next();  // 围住链尾:验收工作项由宿主在栈外排
        }
        return HandlerReturn::Value(decision);
    };
    return def;
}

void AddBuiltinGoalReviewSlot(MiddlewarePool& pool) {
    pool.AddDefinition(BuiltinGoalReviewSlot());
}

}  // namespace lubancode::hooks::middleware
