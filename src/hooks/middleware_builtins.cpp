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

nlohmann::json ComputeUtf8BytesDiv4Estimate(const nlohmann::json& model_input_snapshot) {
    bool saw_media = false;
    std::vector<std::string> modalities;
    const nlohmann::json measured =
        StripUnestimatedMedia(model_input_snapshot, &saw_media, &modalities);
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

}  // namespace lubancode::hooks::middleware
