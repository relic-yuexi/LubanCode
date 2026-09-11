// hook 中间件执行核(LuaHook 单 P0-A)。两件事:
//   1) 发布:同名选实现(同键取最高层,同层冲突拒绝)+ 排序(阶段 ->
//      before/after 依赖 -> priority -> 逻辑键)+ 槽位定档(required/阶段
//      以 builtin 声明为准,替代实现不能解除)。
//   2) 派发:不可变计划上串行改写链(后项读前项已采用版本)+ 至多一次
//      next + 候选采用/退栈/短路 + 跨 invocation 拒绝 + 只读观察者并发。
// 错误分账:业务 deny 是 completed/Denied 结局,不是失败;脚本/handler
// 错误按 failure_policy 收口(required 槽位恒 Abort)。
#include "hooks/middleware.hpp"

#include <algorithm>
#include <initializer_list>
#include <sstream>
#include <thread>
#include <utility>

#include "hooks/hash.hpp"

namespace lubancode::hooks::middleware {

// ---------------------------------------------------------------------------
// 挂点/阶段/效果/层级/策略的名字表
// ---------------------------------------------------------------------------

std::string_view ToString(HookPoint point) {
    switch (point) {
        case HookPoint::PreSystem: return "PreSystem";
        case HookPoint::PostSystem: return "PostSystem";
        case HookPoint::PreUser: return "PreUser";
        case HookPoint::PostUser: return "PostUser";
        case HookPoint::PreAssistant: return "PreAssistant";
        case HookPoint::PostAssistant: return "PostAssistant";
        case HookPoint::PreTurn: return "PreTurn";
        case HookPoint::PostTurn: return "PostTurn";
        case HookPoint::PreStep: return "PreStep";
        case HookPoint::PostStep: return "PostStep";
        case HookPoint::PreRequest: return "PreRequest";
        case HookPoint::PreAction: return "PreAction";
        case HookPoint::PostAction: return "PostAction";
    }
    return "Unknown";
}

bool ParseHookPoint(std::string_view name, HookPoint& out) {
    if (name == "PreSystem") { out = HookPoint::PreSystem; }
    else if (name == "PostSystem") { out = HookPoint::PostSystem; }
    else if (name == "PreUser") { out = HookPoint::PreUser; }
    else if (name == "PostUser") { out = HookPoint::PostUser; }
    else if (name == "PreAssistant") { out = HookPoint::PreAssistant; }
    else if (name == "PostAssistant") { out = HookPoint::PostAssistant; }
    else if (name == "PreTurn") { out = HookPoint::PreTurn; }
    else if (name == "PostTurn") { out = HookPoint::PostTurn; }
    else if (name == "PreStep") { out = HookPoint::PreStep; }
    else if (name == "PostStep") { out = HookPoint::PostStep; }
    else if (name == "PreRequest") { out = HookPoint::PreRequest; }
    else if (name == "PreAction") { out = HookPoint::PreAction; }
    else if (name == "PostAction") { out = HookPoint::PostAction; }
    else { return false; }
    return true;
}

std::string_view ToString(Stage stage) {
    switch (stage) {
        case Stage::Default: return "default";
        case Stage::Mutate: return "mutate";
        case Stage::Estimate: return "estimate";
        case Stage::Capacity: return "capacity";
    }
    return "?";
}

bool ParseStage(std::string_view name, Stage& out) {
    if (name == "mutate") { out = Stage::Mutate; }
    else if (name == "estimate") { out = Stage::Estimate; }
    else if (name == "capacity") { out = Stage::Capacity; }
    else if (name == "default") { out = Stage::Default; }
    else { return false; }
    return true;
}

bool StageAllowed(HookPoint point, Stage stage) {
    if (point == HookPoint::PreRequest) {
        return stage == Stage::Mutate || stage == Stage::Estimate || stage == Stage::Capacity;
    }
    return stage == Stage::Default;
}

std::string_view ToString(EffectType type) {
    switch (type) {
        case EffectType::InputRewrite: return "input.rewrite";
        case EffectType::BackendSelect: return "backend.select";
        case EffectType::ResultReplace: return "result.replace";
        case EffectType::ResultSupplement: return "result.supplement";
        case EffectType::ResultFilter: return "result.filter";
        case EffectType::AdmissionDecision: return "admission.decision";
        case EffectType::ContextAppend: return "context.append";
        case EffectType::SystemChange: return "system.change";
    }
    return "?";
}

bool ParseEffectType(std::string_view name, EffectType& out) {
    if (name == "input.rewrite") { out = EffectType::InputRewrite; }
    else if (name == "backend.select") { out = EffectType::BackendSelect; }
    else if (name == "result.replace") { out = EffectType::ResultReplace; }
    else if (name == "result.supplement") { out = EffectType::ResultSupplement; }
    else if (name == "result.filter") { out = EffectType::ResultFilter; }
    else if (name == "admission.decision") { out = EffectType::AdmissionDecision; }
    else if (name == "context.append") { out = EffectType::ContextAppend; }
    else if (name == "system.change") { out = EffectType::SystemChange; }
    else { return false; }
    return true;
}

std::string_view ToString(SourceLayer layer) {
    switch (layer) {
        case SourceLayer::Builtin: return "builtin";
        case SourceLayer::Extension: return "extension";
        case SourceLayer::Project: return "project";
        case SourceLayer::User: return "user";
        case SourceLayer::Session: return "session";
    }
    return "?";
}

std::string_view ToString(FailurePolicy policy) {
    switch (policy) {
        case FailurePolicy::Abort: return "abort";
        case FailurePolicy::KeepOriginal: return "keep_original";
    }
    return "?";
}

bool ParseFailurePolicy(std::string_view name, FailurePolicy& out) {
    if (name == "abort") { out = FailurePolicy::Abort; }
    else if (name == "keep_original") { out = FailurePolicy::KeepOriginal; }
    else { return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 挂点(+阶段)x 效果合同矩阵(P0-A 冻结)。
// ---------------------------------------------------------------------------

namespace {

bool EffectIn(EffectType needle, std::initializer_list<EffectType> hay) {
    for (const EffectType candidate : hay) {
        if (candidate == needle) {
            return true;
        }
    }
    return false;
}

// 观察者不许给改变链路走向的效果(§3.1:只读、不参与本次准入或结果决定)。
bool EffectAllowedForObserver(EffectType type) {
    return EffectIn(type, {EffectType::ContextAppend, EffectType::ResultSupplement});
}

}  // namespace

bool EffectAllowed(HookPoint point, Stage stage, EffectType type) {
    switch (point) {
        case HookPoint::PreSystem:
            return EffectIn(type, {EffectType::SystemChange, EffectType::ContextAppend});
        case HookPoint::PostSystem:
            return false;  // 不再改写当前 system(§4.47)
        case HookPoint::PreUser:
            return EffectIn(type, {EffectType::InputRewrite, EffectType::AdmissionDecision, EffectType::ContextAppend});
        case HookPoint::PostUser:
            // 不能回写原 user(§4.47);只追加带来源的隐藏上下文。
            return EffectIn(type, {EffectType::ContextAppend, EffectType::ResultSupplement});
        case HookPoint::PreAssistant:
            // 不改模型原始回复/签名/tool_calls;只准入。
            return type == EffectType::AdmissionDecision;
        case HookPoint::PostAssistant:
            return EffectIn(type, {EffectType::ContextAppend, EffectType::ResultSupplement});
        case HookPoint::PreTurn:
            return EffectIn(type, {EffectType::ContextAppend, EffectType::AdmissionDecision});
        case HookPoint::PostTurn:
            return false;
        case HookPoint::PreStep:
            return EffectIn(type, {EffectType::InputRewrite, EffectType::ContextAppend, EffectType::AdmissionDecision});
        case HookPoint::PostStep:
            return false;
        case HookPoint::PreRequest:
            switch (stage) {
                case Stage::Mutate:
                    return EffectIn(type, {EffectType::InputRewrite, EffectType::ContextAppend});
                case Stage::Estimate:
                    // 输入已冻结(§4.36):估算只产出结构化测量结果,不改请求。
                    return false;
                case Stage::Capacity:
                    // 容量判断消费估算,返回允许发送/要求恢复/拒绝。
                    return type == EffectType::AdmissionDecision;
                case Stage::Default:
                    return false;
            }
            return false;
        case HookPoint::PreAction:
            return EffectIn(type, {EffectType::InputRewrite, EffectType::BackendSelect, EffectType::AdmissionDecision,
                                   EffectType::ContextAppend});
        case HookPoint::PostAction:
            // 副作用已发生:无准入;只加工/选择结果。
            return EffectIn(type, {EffectType::ResultReplace, EffectType::ResultSupplement, EffectType::ResultFilter});
    }
    return false;
}

bool InputRewriteAllowed(HookPoint point, Stage stage) {
    return EffectAllowed(point, stage, EffectType::InputRewrite);
}

// ---------------------------------------------------------------------------
// 身份发行
// ---------------------------------------------------------------------------

std::string NextMiddlewareDispatchId() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "hookmw_" + std::to_string(ms) + "_" + std::to_string(counter.fetch_add(1));
}

std::string NextMiddlewareInvocationId(const std::string& dispatch_id, int order) {
    return dispatch_id + "#" + std::to_string(order);
}

// ---------------------------------------------------------------------------
// 定义 hash:覆盖清单 id/版本、条目声明与脚本体(§三:定义 hash 覆盖清单、
// 入口和声明依赖)。
// ---------------------------------------------------------------------------

namespace {

std::string CanonicalDefinitionText(const MiddlewareDefinition& def) {
    std::ostringstream out;
    out << "v1|" << ToString(def.point) << '|' << def.name << '|' << ToString(def.layer) << '|'
        << def.implementation_ref << '|' << (def.is_lua ? "lua" : "builtin") << '|' << def.lua.chunk_name << '|'
        << def.lua.entry << '|' << def.lua.script << '|' << ToString(def.stage) << '|' << def.priority << '|';
    for (const auto& dep : def.before) { out << "b:" << dep << ';'; }
    for (const auto& dep : def.after) { out << "a:" << dep << ';'; }
    out << '|' << def.match.origin.value_or("") << ',' << def.match.purpose.value_or("") << ','
        << def.match.delivery_mode.value_or("") << '|' << ToString(def.failure_policy) << '|'
        << (def.required ? "req" : "opt") << '|' << (def.observer ? "obs" : "chain");
    return out.str();
}

std::string ComputeMiddlewareDefinitionHash(const MiddlewareDefinition& def) {
    return Sha256Hex(CanonicalDefinitionText(def));
}

// P1-C:implementation_ref "hooks/<id>#<entry>[@<ver>]" 的包 id;非该形状
//(直构定义/内置)退 chunk_name。只做展示与命名空间归属,不当身份键。
std::string PackageIdOfRef(const std::string& implementation_ref, const std::string& fallback) {
    constexpr std::string_view kPrefix = "hooks/";
    if (implementation_ref.rfind(kPrefix, 0) == 0) {
        const std::size_t hash = implementation_ref.find('#', kPrefix.size());
        if (hash != std::string::npos && hash > kPrefix.size()) {
            return implementation_ref.substr(kPrefix.size(), hash - kPrefix.size());
        }
    }
    return fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// 匹配
// ---------------------------------------------------------------------------

bool MatchRule::Matches(const DispatchTrigger& trigger) const {
    if (origin.has_value() && trigger.origin != origin) {
        return false;
    }
    if (purpose.has_value() && trigger.purpose != purpose) {
        return false;
    }
    if (delivery_mode.has_value() && trigger.delivery_mode != delivery_mode) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 清单解析(schemaVersion 1)
// ---------------------------------------------------------------------------

namespace {

ManifestError ManifestErr(std::string message) {
    return ManifestError{std::string(err::kManifestInvalid), std::move(message)};
}

std::expected<std::string, ManifestError> ManifestStringField(const nlohmann::json& object, const char* field,
                                                              bool required, const std::string& where) {
    const auto it = object.find(field);
    if (it == object.end() || it->is_null()) {
        if (required) {
            return std::unexpected(ManifestErr(where + "缺 " + field + " 字段"));
        }
        return std::string();
    }
    if (!it->is_string()) {
        return std::unexpected(ManifestErr(where + field + " 字段不是字符串"));
    }
    return it->get<std::string>();
}

}  // namespace

std::expected<std::vector<MiddlewareDefinition>, ManifestError> ParseHookManifest(
    const nlohmann::json& manifest, SourceLayer layer, const std::string& source_label, const std::string& script) {
    if (!manifest.is_object()) {
        return std::unexpected(ManifestErr("清单顶层不是 object"));
    }
    const auto schema = manifest.find("schemaVersion");
    if (schema == manifest.end() || !schema->is_number_integer() || schema->get<int>() != 1) {
        return std::unexpected(ManifestErr("schemaVersion 须为 1"));
    }
    auto id = ManifestStringField(manifest, "id", /*required=*/true, "清单");
    if (!id.has_value()) { return std::unexpected(id.error()); }
    if (id->empty()) {
        return std::unexpected(ManifestErr("id 是空串"));
    }
    auto entry_file = ManifestStringField(manifest, "entry", /*required=*/true, "清单");
    if (!entry_file.has_value()) { return std::unexpected(entry_file.error()); }
    if (entry_file->empty()) {
        return std::unexpected(ManifestErr("entry 是空串"));
    }
    auto version = ManifestStringField(manifest, "version", /*required=*/false, "清单");
    if (!version.has_value()) { return std::unexpected(version.error()); }

    const std::string chunk_name = *entry_file;
    std::vector<MiddlewareDefinition> out;
    const auto& hooks_array = manifest.at("hooks");
    if (!hooks_array.is_array()) {
        return std::unexpected(ManifestErr("hooks 不是数组"));
    }
    for (const auto& hook : hooks_array) {
        const std::string where = "hooks[" + std::to_string(out.size()) + "] ";
        if (!hook.is_object()) {
            return std::unexpected(ManifestErr(where + "不是 object"));
        }
        MiddlewareDefinition def;
        def.layer = layer;
        def.source_label = source_label;

        auto point_name = ManifestStringField(hook, "hookPoint", /*required=*/true, where);
        if (!point_name.has_value()) { return std::unexpected(point_name.error()); }
        if (!ParseHookPoint(*point_name, def.point)) {
            return std::unexpected(ManifestErr(where + "hookPoint 不认识: " + *point_name));
        }

        auto name = ManifestStringField(hook, "name", /*required=*/true, where);
        if (!name.has_value()) { return std::unexpected(name.error()); }
        if (name->empty()) {
            return std::unexpected(ManifestErr(where + "name 是空串"));
        }
        def.name = *name;

        auto handler = ManifestStringField(hook, "handler", /*required=*/true, where);
        if (!handler.has_value()) { return std::unexpected(handler.error()); }
        if (handler->empty()) {
            return std::unexpected(ManifestErr(where + "handler 是空串"));
        }
        def.lua.entry = *handler;
        def.lua.chunk_name = chunk_name;
        def.lua.script = script;
        def.is_lua = true;
        def.implementation_ref = "hooks/" + *id + "#" + *handler;
        if (!version->empty()) {
            def.implementation_ref += "@" + *version;
        }

        if (const auto stage_it = hook.find("stage"); stage_it != hook.end() && !stage_it->is_null()) {
            if (!stage_it->is_string() || !ParseStage(stage_it->get<std::string>(), def.stage)) {
                return std::unexpected(ManifestErr(where + "stage 不认识(只收 mutate/estimate/capacity/default)"));
            }
            if (!StageAllowed(def.point, def.stage)) {
                return std::unexpected(ManifestErr(where + "挂点 " + std::string(ToString(def.point)) +
                                                   " 不收阶段 " + std::string(ToString(def.stage))));
            }
        }

        if (const auto priority_it = hook.find("priority"); priority_it != hook.end() && !priority_it->is_null()) {
            if (!priority_it->is_number_integer()) {
                return std::unexpected(ManifestErr(where + "priority 不是整数"));
            }
            def.priority = priority_it->get<int>();
        }

        for (const auto& [dep_field, deps] :
             std::initializer_list<std::pair<const char*, std::vector<std::string>*>>{{"before", &def.before},
                                                                                       {"after", &def.after}}) {
            if (const auto deps_it = hook.find(dep_field); deps_it != hook.end() && !deps_it->is_null()) {
                if (!deps_it->is_array()) {
                    return std::unexpected(ManifestErr(where + dep_field + std::string(" 不是数组")));
                }
                for (const auto& dep : *deps_it) {
                    if (!dep.is_string() || dep.get_ref<const std::string&>().empty()) {
                        return std::unexpected(ManifestErr(where + dep_field + std::string(" 项须是非空字符串")));
                    }
                    deps->push_back(dep.get<std::string>());
                }
            }
        }

        if (const auto match_it = hook.find("match"); match_it != hook.end() && !match_it->is_null()) {
            if (!match_it->is_object()) {
                return std::unexpected(ManifestErr(where + "match 不是 object"));
            }
            for (const auto& [field, slot] :
                 std::initializer_list<std::pair<const char*, std::optional<std::string>*>>{
                     {"origin", &def.match.origin},
                     {"purpose", &def.match.purpose},
                     {"deliveryMode", &def.match.delivery_mode}}) {
                if (const auto it = match_it->find(field); it != match_it->end() && !it->is_null()) {
                    if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
                        return std::unexpected(ManifestErr(where + "match." + field + std::string(" 须是非空字符串")));
                    }
                    *slot = it->get<std::string>();
                }
            }
        }

        if (const auto caps_it = hook.find("capabilities"); caps_it != hook.end() && !caps_it->is_null()) {
            if (!caps_it->is_array()) {
                return std::unexpected(ManifestErr(where + "capabilities 不是数组"));
            }
            for (const auto& cap : *caps_it) {
                if (!cap.is_string()) {
                    return std::unexpected(ManifestErr(where + "capabilities 项须是字符串"));
                }
                def.capabilities.push_back(cap.get<std::string>());
            }
        }

        if (const auto policy_it = hook.find("failurePolicy"); policy_it != hook.end() && !policy_it->is_null()) {
            if (!policy_it->is_string() || !ParseFailurePolicy(policy_it->get<std::string>(), def.failure_policy)) {
                return std::unexpected(ManifestErr(where + "failurePolicy 只认 abort/keep_original"));
            }
        }

        if (const auto replay_it = hook.find("replayPolicy"); replay_it != hook.end() && !replay_it->is_null()) {
            if (!replay_it->is_string() || replay_it->get<std::string>() != "manual") {
                return std::unexpected(ManifestErr(where + "replayPolicy 首批只认 manual(默认值,可不写)"));
            }
        }

        if (const auto it = hook.find("required"); it != hook.end() && !it->is_null()) {
            if (!it->is_boolean()) {
                return std::unexpected(ManifestErr(where + "required 不是布尔"));
            }
            def.required = it->get<bool>();
        }

        if (const auto it = hook.find("observer"); it != hook.end() && !it->is_null()) {
            if (!it->is_boolean()) {
                return std::unexpected(ManifestErr(where + "observer 不是布尔"));
            }
            def.observer = it->get<bool>();
        }

        if (const auto limits_it = hook.find("limits"); limits_it != hook.end() && !limits_it->is_null()) {
            if (!limits_it->is_object()) {
                return std::unexpected(ManifestErr(where + "limits 不是 object"));
            }
            if (const auto timeout_it = limits_it->find("activeTimeoutMs");
                timeout_it != limits_it->end() && !timeout_it->is_null()) {
                if (!timeout_it->is_number_integer() || timeout_it->get<std::int64_t>() <= 0) {
                    return std::unexpected(ManifestErr(where + "limits.activeTimeoutMs 须是正整数(毫秒)"));
                }
                def.limits.wall_budget = std::chrono::milliseconds(timeout_it->get<std::int64_t>());
            }
        }

        def.definition_hash = ComputeMiddlewareDefinitionHash(def);
        out.push_back(std::move(def));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 注册池与发布
// ---------------------------------------------------------------------------

void MiddlewarePool::AddDefinition(MiddlewareDefinition definition) {
    if (definition.definition_hash.empty()) {
        definition.definition_hash = ComputeMiddlewareDefinitionHash(definition);
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    candidates_.push_back(std::make_shared<const MiddlewareDefinition>(std::move(definition)));
}

std::expected<void, ManifestError> MiddlewarePool::AddManifest(const nlohmann::json& manifest, SourceLayer layer,
                                                               const std::string& source_label,
                                                               const std::string& script) {
    auto parsed = ParseHookManifest(manifest, layer, source_label, script);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& def : *parsed) {
        candidates_.push_back(std::make_shared<const MiddlewareDefinition>(std::move(def)));
    }
    return {};
}

std::size_t MiddlewarePool::candidate_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return candidates_.size();
}

namespace {

// 依赖串规范化:"Point/name" 或裸名(按同挂点解析)。false = 指到别的挂点。
bool NormalizeDep(const std::string& raw, HookPoint owner_point, std::string& out_name) {
    const std::size_t slash = raw.find('/');
    if (slash == std::string::npos) {
        out_name = raw;
        return true;
    }
    HookPoint dep_point{};
    if (!ParseHookPoint(std::string_view(raw).substr(0, slash), dep_point) || dep_point != owner_point) {
        return false;
    }
    out_name = raw.substr(slash + 1);
    return !out_name.empty();
}

int StageRank(Stage stage) { return static_cast<int>(stage); }

}  // namespace

std::expected<std::shared_ptr<const FrozenRegistry>, PlanError> MiddlewarePool::Publish() {
    std::vector<std::shared_ptr<const MiddlewareDefinition>> candidates;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        candidates = candidates_;
    }

    // ---- 第一步:同键选实现(最高层胜;同层冲突拒绝)。----
    // key -> 层 rank -> 定义。
    std::map<std::string, std::map<int, std::vector<std::shared_ptr<const MiddlewareDefinition>>>> by_key;
    for (const auto& def : candidates) {
        by_key[def->Key()][LayerRank(def->layer)].push_back(def);
    }

    auto registry = std::shared_ptr<FrozenRegistry>(new FrozenRegistry());
    for (auto& [key, layers] : by_key) {
        // 每层只许一条;同层两条及以上 = 冲突(不按文件扫描先后猜赢家,
        // 报冲突,要求配置点明所选实现——§4.48)。
        for (auto& [rank, defs] : layers) {
            if (defs.size() > 1) {
                std::string impls;
                for (const auto& def : defs) {
                    impls += (impls.empty() ? "" : ", ") + def->implementation_ref;
                }
                return std::unexpected(
                    PlanError{std::string(err::kPlanSameKeyConflict),
                              "同层同键冲突: " + key + " [" + std::string(ToString(defs.front()->layer)) + "] " +
                                  impls + ";同键只能一项获选,须在配置里点明"});
            }
        }
        // 最高层胜出。
        const std::shared_ptr<const MiddlewareDefinition> winner = layers.rbegin()->second.front();

        // 槽位定档:builtin 候选在场时,required/阶段以 builtin 声明为准,
        // 替代实现不能解除(§4.48)。落到获选项的执行副本上。
        const auto builtin_it = layers.find(LayerRank(SourceLayer::Builtin));
        MiddlewareDefinition effective = *winner;
        if (builtin_it != layers.end() && winner->layer != SourceLayer::Builtin) {
            const MiddlewareDefinition& slot = *builtin_it->second.front();
            if (winner->stage != slot.stage) {
                return std::unexpected(PlanError{
                    std::string(err::kPlanStageMismatch),
                    "替代实现改了槽位阶段: " + key + " 槽位 " + std::string(ToString(slot.stage)) + ",获选项 " +
                        std::string(ToString(winner->stage))});
            }
            effective.required = slot.required;  // 只认槽位,不许解除
        }
        if (effective.observer && effective.required) {
            return std::unexpected(
                PlanError{std::string(err::kPlanObserverRequired), "required 槽位不许声明 observer: " + key});
        }
        if (effective.observer && (!effective.before.empty() || !effective.after.empty())) {
            return std::unexpected(
                PlanError{std::string(err::kPlanObserverWithDeps), "观察者不许带 before/after 依赖: " + key});
        }
        // Lua 定义须有工厂;发布期把 lua 声明物化成 Handler(执行核里 lua
        // 与 builtin 同形)。物化失败(编译/对账不过)整版拒绝。P1-C 起能力
        // 申请随声明传给工厂(宿主造 per-invocation 服务束用,§五交集)。
        if (effective.is_lua) {
            if (!options_.lua_factory) {
                return std::unexpected(PlanError{std::string(err::kPlanNoLuaFactory), "lua 定义而无 lua 工厂: " + key});
            }
            effective.lua.capabilities = effective.capabilities;
            effective.lua.package = PackageIdOfRef(effective.implementation_ref, effective.lua.chunk_name);
            auto handler = options_.lua_factory(effective.lua, effective.limits);
            if (!handler.has_value()) {
                return std::unexpected(PlanError{std::string(err::kLuaCompileError),
                                                  key + " 物化失败: " + handler.error()});
            }
            effective.builtin = std::move(*handler);
        } else if (!effective.builtin) {
            return std::unexpected(PlanError{std::string(err::kHandlerFailed),
                                              "builtin 定义缺可执行 Handler: " + key});
        }
        registry->selected_[effective.point].push_back(
            std::make_shared<const MiddlewareDefinition>(std::move(effective)));
        // 落选项保留定义来源(§3.1:不进本次执行计划,可查)。
        for (auto iter = layers.rbegin(); iter != layers.rend(); ++iter) {
            if (iter->second.front() != winner) {
                registry->overridden_.push_back(iter->second.front());
            }
        }
    }

    // ---- 第二步:逐挂点排序(阶段 -> 依赖 -> priority -> 逻辑键)。----
    for (auto& [point, defs] : registry->selected_) {
        // 基准序:阶段、priority 小者先、逻辑键稳定排序。
        std::sort(defs.begin(), defs.end(), [](const auto& a, const auto& b) {
            if (StageRank(a->stage) != StageRank(b->stage)) {
                return StageRank(a->stage) < StageRank(b->stage);
            }
            if (a->priority != b->priority) {
                return a->priority < b->priority;
            }
            return a->name < b->name;
        });
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < defs.size(); ++i) {
            index[defs[i]->name] = i;
        }

        // 依赖边:from 先跑、to 后跑。规范化与存在性先行。
        std::vector<std::vector<std::size_t>> edges(defs.size());
        std::vector<int> indegree(defs.size(), 0);
        const auto add_edge = [&](std::size_t from, std::size_t to) -> std::optional<PlanError> {
            // 阶段倒置:from 须先跑,但阶段晚于 to(mutate<estimate<capacity)。
            if (StageRank(defs[from]->stage) > StageRank(defs[to]->stage)) {
                return PlanError{std::string(err::kPlanStageInversion),
                                 "阶段倒置: " + defs[from]->Key() + "(" + std::string(ToString(defs[from]->stage)) +
                                     ") 声明先于 " + defs[to]->Key() + "(" + std::string(ToString(defs[to]->stage)) +
                                     ")"};
            }
            edges[from].push_back(to);
            ++indegree[to];
            return std::nullopt;
        };
        for (std::size_t i = 0; i < defs.size(); ++i) {
            for (const std::string& raw : defs[i]->after) {
                std::string dep_name;
                if (!NormalizeDep(raw, point, dep_name)) {
                    return std::unexpected(
                        PlanError{std::string(err::kPlanMissingDep),
                                  "依赖指向别的挂点: " + defs[i]->Key() + " after " + raw +
                                      "(依赖只在本挂点内解析)"});
                }
                const auto dep_it = index.find(dep_name);
                if (dep_it == index.end()) {
                    return std::unexpected(PlanError{std::string(err::kPlanMissingDep),
                                                     "缺失依赖: " + defs[i]->Key() + " after " + raw +
                                                         " 在本挂点没有获选定义"});
                }
                if (auto error = add_edge(dep_it->second, i); error.has_value()) {
                    return std::unexpected(*error);
                }
            }
            for (const std::string& raw : defs[i]->before) {
                std::string dep_name;
                if (!NormalizeDep(raw, point, dep_name)) {
                    return std::unexpected(
                        PlanError{std::string(err::kPlanMissingDep),
                                  "依赖指向别的挂点: " + defs[i]->Key() + " before " + raw +
                                      "(依赖只在本挂点内解析)"});
                }
                const auto dep_it = index.find(dep_name);
                if (dep_it == index.end()) {
                    return std::unexpected(PlanError{std::string(err::kPlanMissingDep),
                                                     "缺失依赖: " + defs[i]->Key() + " before " + raw +
                                                         " 在本挂点没有获选定义"});
                }
                if (auto error = add_edge(i, dep_it->second); error.has_value()) {
                    return std::unexpected(*error);
                }
            }
        }

        // Kahn:就绪项里恒取基准序最小者(依赖优先于 priority;同值按逻辑
        // 键稳定排序)。余下有环 = 循环依赖,整版拒绝。
        std::vector<std::size_t> order;
        order.reserve(defs.size());
        std::vector<bool> done(defs.size(), false);
        for (std::size_t picked = 0; picked < defs.size(); ++picked) {
            bool found = false;
            for (std::size_t i = 0; i < defs.size(); ++i) {
                if (!done[i] && indegree[i] == 0) {
                    done[i] = true;
                    order.push_back(i);
                    for (const std::size_t to : edges[i]) {
                        --indegree[to];
                    }
                    found = true;
                    break;  // 基准序最小(计划规模小,线性扫即够)
                }
            }
            if (!found) {
                std::string stuck;
                for (std::size_t i = 0; i < defs.size(); ++i) {
                    if (!done[i]) {
                        stuck += (stuck.empty() ? "" : ", ") + defs[i]->Key();
                    }
                }
                return std::unexpected(PlanError{std::string(err::kPlanDependencyCycle), "循环依赖: " + stuck});
            }
        }
        std::vector<std::shared_ptr<const MiddlewareDefinition>> ordered;
        ordered.reserve(defs.size());
        for (const std::size_t i : order) {
            ordered.push_back(std::move(defs[i]));
        }
        defs = std::move(ordered);
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        registry->revision_ = next_revision_++;
    }
    return std::shared_ptr<const FrozenRegistry>(std::move(registry));
}

const std::vector<std::shared_ptr<const MiddlewareDefinition>>& FrozenRegistry::Selected(HookPoint point) const {
    static const std::vector<std::shared_ptr<const MiddlewareDefinition>> kEmpty;
    const auto it = selected_.find(point);
    return it == selected_.end() ? kEmpty : it->second;
}

nlohmann::json FrozenRegistry::DescribePlan() const {
    nlohmann::json plan = nlohmann::json::object();
    plan["registryRevision"] = revision_;
    nlohmann::json points = nlohmann::json::object();
    for (const auto& [point, defs] : selected_) {
        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = 0; i < defs.size(); ++i) {
            const MiddlewareDefinition& def = *defs[i];
            arr.push_back(nlohmann::json{{"order", i},
                                         {"key", def.Key()},
                                         {"source", ToString(def.layer)},
                                         {"implementationRef", def.implementation_ref},
                                         {"definitionHash", def.definition_hash},
                                         {"stage", ToString(def.stage)},
                                         {"priority", def.priority},
                                         {"handlerKind", def.is_lua ? "lua" : "builtin"},
                                         {"required", def.required},
                                         {"observer", def.observer}});
        }
        points[std::string(ToString(point))] = std::move(arr);
    }
    plan["points"] = std::move(points);
    nlohmann::json overridden = nlohmann::json::array();
    for (const auto& def : overridden_) {
        overridden.push_back(nlohmann::json{{"key", def->Key()},
                                            {"source", ToString(def->layer)},
                                            {"implementationRef", def->implementation_ref},
                                            {"definitionHash", def->definition_hash}});
    }
    plan["overridden"] = std::move(overridden);
    return plan;
}

// ---------------------------------------------------------------------------
// NextCall
// ---------------------------------------------------------------------------

DownstreamOutcome NextCall::Call(std::optional<nlohmann::json> candidate) {
    ++calls_;
    if (consumed_) {
        // 重复调用:不跑下游、不增加执行次数,明报协议违规。
        DownstreamOutcome invalid;
        invalid.kind = DownstreamOutcome::Kind::Invalid;
        invalid.code = err::kNextAlreadyConsumed;
        invalid.message = "next 至多调用一次;本次调用未执行下游";
        return invalid;
    }
    if (!impl_) {
        DownstreamOutcome invalid;
        invalid.kind = DownstreamOutcome::Kind::Invalid;
        invalid.code = err::kNextNotAllowed;
        invalid.message = "本 handler 没有 next(观察者不参与链执行)";
        return invalid;
    }
    DownstreamOutcome result = impl_(candidate);
    if (result.kind != DownstreamOutcome::Kind::Invalid) {
        consumed_ = true;
        last_ = result;
    }
    return result;
}

NextCall NextCall::Forbidden() {
    return NextCall{};  // 无 impl:Call 恒回 Invalid(hook.next.not_allowed)
}

// ---------------------------------------------------------------------------
// 派发执行核
// ---------------------------------------------------------------------------

namespace {

struct PlanEntry {
    std::shared_ptr<const MiddlewareDefinition> def;
    bool matched = true;
    std::string skip_reason;
};

struct FrameResult {
    DispatchOutcome::Kind kind = DispatchOutcome::Kind::Completed;
    nlohmann::json value;
    std::string code, message;
};

// 一次 dispatch 的执行账(计划本身只读;记录槽按计划序预置,执行中只回填)。
struct DispatchState {
    HookPoint point = HookPoint::PreUser;
    std::string dispatch_id;
    std::uint64_t revision = 0;
    std::vector<PlanEntry> entries;   // 计划序(含未命中项)
    std::vector<std::size_t> chain_index;    // entries 里非观察者下标(执行序)
    std::vector<std::size_t> observer_index; // 观察者下标
    DispatchOutcome outcome;
    DispatchTrigger trigger;
    MiddlewareEventSink* sink = nullptr;
    bool frozen = false;  // PreRequest freeze 边界已过(mutate 段收尾)
    TerminalFn terminal;
    int terminal_runs = 0;
};

// 结局严酷度:Completed < Denied < Failed。外层后置可加工值,不能把下游
// deny/失败洗成成功(§四:外层不能把 required deny 改成 allow)。
int OutcomeSeverity(DispatchOutcome::Kind kind) {
    switch (kind) {
        case DispatchOutcome::Kind::Completed: return 0;
        case DispatchOutcome::Kind::Denied: return 1;
        case DispatchOutcome::Kind::Failed: return 2;
    }
    return 0;
}

DispatchOutcome::Kind WorseKind(DispatchOutcome::Kind a, DispatchOutcome::Kind b) {
    return OutcomeSeverity(a) >= OutcomeSeverity(b) ? a : b;
}

FrameResult ToFrameResult(const DownstreamOutcome& downstream) {
    FrameResult frame;
    switch (downstream.kind) {
        case DownstreamOutcome::Kind::Value:
            frame.kind = DispatchOutcome::Kind::Completed;
            break;
        case DownstreamOutcome::Kind::Denied:
            frame.kind = DispatchOutcome::Kind::Denied;
            break;
        case DownstreamOutcome::Kind::Failed:
        case DownstreamOutcome::Kind::Invalid:
            frame.kind = DispatchOutcome::Kind::Failed;
            break;
    }
    frame.value = downstream.value;
    frame.code = downstream.code;
    frame.message = downstream.message;
    return frame;
}

InvocationMeta MakeMeta(const InvocationCtx& ctx, const InvocationRecord& record) {
    InvocationMeta meta;
    meta.dispatch_id = ctx.dispatch_id;
    meta.invocation_id = ctx.invocation_id;
    meta.hook_id = ctx.hook_id;
    meta.handler_kind = record.handler_kind;
    meta.definition_hash = ctx.definition_hash;
    meta.definition_order = record.definition_order;
    return meta;
}

// 提案载荷(§7.1):after_next/short_circuit 提案带上返回值与效果清单——
// 恢复侧据此判"handler 返回已保存、效果尚缺"(§7.3 行 5),补提交原返回
// 而不重新运行 handler。
nlohmann::json ProposalPayload(const HandlerReturn& result) {
    nlohmann::json effects = nlohmann::json::array();
    for (const Effect& effect : result.effects) {
        effects.push_back(nlohmann::json{{"type", ToString(effect.type)}, {"payload", effect.payload}});
    }
    return nlohmann::json{{"output", result.output},
                          {"deny", result.deny},
                          {"denyCode", result.deny_code},
                          {"denyMessage", result.deny_message},
                          {"effects", std::move(effects)}};
}

DownstreamOutcome ToDownstream(const FrameResult& frame) {
    DownstreamOutcome out;
    out.kind = frame.kind == DispatchOutcome::Kind::Completed
                   ? DownstreamOutcome::Kind::Value
                   : (frame.kind == DispatchOutcome::Kind::Denied ? DownstreamOutcome::Kind::Denied
                                                                  : DownstreamOutcome::Kind::Failed);
    out.value = frame.value;
    out.code = frame.code;
    out.message = frame.message;
    return out;
}

void MarkSkippedRemaining(DispatchState& state, std::size_t chain_pos, const char* outcome, std::string reason) {
    for (std::size_t pos = chain_pos; pos < state.chain_index.size(); ++pos) {
        InvocationRecord& record = state.outcome.records[state.chain_index[pos]];
        record.outcome = outcome;
        record.detail = reason;
    }
}

// 效果校验与落账:候选先存,验证后 applied/rejected(§4.22)。appends 不
// 在这里合入(观察者线程各写各的记录;Dispatch 收口时按计划序统一归并)。
void RecordEffect(DispatchState& state, InvocationRecord& record, const InvocationMeta& meta, EffectType type,
                  const nlohmann::json& payload, bool observer) {
    EffectRecord effect;
    effect.type = std::string(ToString(type));
    effect.payload = payload;
    const Stage stage = state.entries[static_cast<std::size_t>(record.definition_order)].def->stage;
    const bool point_allows = EffectAllowed(state.point, stage, type);
    const bool observer_allows = !observer || EffectAllowedForObserver(type);
    if (point_allows && observer_allows) {
        effect.applied = true;
        if (state.sink != nullptr) {
            state.sink->OnEffectApplied(meta, ToString(type));
            state.sink->OnEffectSettled(meta, ToString(type), /*applied=*/true, {}, payload);
        }
    } else {
        effect.reject_reason = !point_allows
                                   ? "挂点/阶段不收这枚效果(" + std::string(ToString(state.point)) + "/" +
                                         std::string(ToString(stage)) + ")"
                                   : "观察者不许给改变链路走向的效果";
        if (state.sink != nullptr) {
            state.sink->OnEffectRejected(meta, ToString(type), effect.reject_reason);
            state.sink->OnEffectSettled(meta, ToString(type), /*applied=*/false, effect.reject_reason, payload);
        }
    }
    record.effects.push_back(std::move(effect));
}

FrameResult RunFrame(DispatchState& state, std::size_t chain_pos, const nlohmann::json& input);

// 观察者单跑(独立线程调用):无 next、看原始触发输入、失败不连累 dispatch。
// 只读 DispatchState(计划/触发);自己的记录本地改完整个交回。
InvocationRecord RunObserver(DispatchState& state, std::size_t entry_pos) {
    const MiddlewareDefinition& def = *state.entries[entry_pos].def;
    InvocationRecord record = state.outcome.records[entry_pos];
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [started] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };

    InvocationCtx ctx;
    ctx.dispatch_id = state.dispatch_id;
    ctx.invocation_id = NextMiddlewareInvocationId(state.dispatch_id, record.definition_order);
    ctx.registry_revision = state.revision;
    ctx.point = state.point;
    ctx.stage = def.stage;
    ctx.hook_id = def.Key();
    ctx.definition_hash = def.definition_hash;
    ctx.turn_id = state.trigger.turn_id;
    ctx.step_id = state.trigger.step_id;
    ctx.action_id = state.trigger.action_id;
    ctx.request_id = state.trigger.request_id;
    ctx.cancel = state.trigger.cancel;
    const InvocationMeta meta = MakeMeta(ctx, record);

    if (state.sink != nullptr) {
        state.sink->OnInvocationStarted(meta);
    }

    NextCall next = NextCall::Forbidden();
    const Handler& handler = def.builtin;  // 发布期 lua 已物化成同形 Handler

    if (!handler) {
        record.outcome = "failed";
        record.error_code = std::string(err::kHandlerFailed);
        record.detail = "观察者定义没有可执行 handler";
        if (state.sink != nullptr) {
            state.sink->OnInvocationFailed(meta, record.error_code, elapsed_ms());
        }
        return record;
    }
    try {
        auto result = handler(ctx, state.trigger.input, next);
        record.duration_ms = elapsed_ms();
        if (!result.has_value()) {
            record.outcome = "failed";
            record.error_code = result.error().code;
            record.detail = result.error().message;
            if (state.sink != nullptr) {
                state.sink->OnInvocationFailed(meta, record.error_code, record.duration_ms);
            }
            return record;
        }
        record.outcome = "completed";
        record.next_consumed = next.consumed();
        record.next_calls = next.calls();
        if (state.sink != nullptr) {
            state.sink->OnInvocationCompleted(meta, std::nullopt, record.duration_ms);
        }
        for (const Effect& effect : result->effects) {
            RecordEffect(state, record, meta, effect.type, effect.payload, /*observer=*/true);
        }
        return record;
    } catch (const std::exception& e) {
        record.outcome = "failed";
        record.error_code = std::string(err::kHandlerFailed);
        record.detail = std::string("观察者 handler 抛异常: ") + e.what();
        record.duration_ms = elapsed_ms();
        if (state.sink != nullptr) {
            state.sink->OnInvocationFailed(meta, record.error_code, record.duration_ms);
        }
        return record;
    } catch (...) {
        record.outcome = "failed";
        record.error_code = std::string(err::kHandlerFailed);
        record.detail = "观察者 handler 抛未知异常";
        record.duration_ms = elapsed_ms();
        if (state.sink != nullptr) {
            state.sink->OnInvocationFailed(meta, record.error_code, record.duration_ms);
        }
        return record;
    }
}

FrameResult RunFrame(DispatchState& state, std::size_t chain_pos, const nlohmann::json& input) {
    // 嵌套深度(§4.1):链深守门。
    if (chain_pos > static_cast<std::size_t>(MiddlewareDispatcher::kMaxChainDepth)) {
        MarkSkippedRemaining(state, chain_pos, "skipped_failed_upstream", "链深超限,未进入");
        FrameResult failed;
        failed.kind = DispatchOutcome::Kind::Failed;
        failed.code = std::string(err::kNestingExceeded);
        failed.message = "链深超过 " + std::to_string(MiddlewareDispatcher::kMaxChainDepth);
        return failed;
    }
    // 取消:帧边界检查(Esc/父任务取消;§六)。Lua 内部的取消由 guard
    // 的指令 hook 落锤,两条链同一枚旗。
    if (state.trigger.cancel != nullptr && state.trigger.cancel->load()) {
        MarkSkippedRemaining(state, chain_pos, "skipped_cancelled", "dispatch 取消");
        FrameResult failed;
        failed.kind = DispatchOutcome::Kind::Failed;
        failed.code = std::string(err::kDispatchCancelled);
        failed.message = "dispatch 取消(取消旗已置位)";
        return failed;
    }

    if (chain_pos >= state.chain_index.size()) {
        // 链尾:被围住的一次明确阶段。PreRequest 的 mutate 段在此收口冻结。
        if (state.point == HookPoint::PreRequest && !state.frozen) {
            state.frozen = true;
        }
        state.outcome.adopted_input = input;
        ++state.terminal_runs;
        FrameResult frame;
        frame.kind = DispatchOutcome::Kind::Completed;
        frame.value = state.terminal ? state.terminal(input) : input;
        return frame;
    }

    const std::size_t entry_pos = state.chain_index[chain_pos];
    const MiddlewareDefinition& def = *state.entries[entry_pos].def;
    InvocationRecord& record = state.outcome.records[entry_pos];

    // freeze 边界:mutate 段收尾、首个非 mutate 项之前(§4.36)。估算与
    // 容量阶段看到的输入已冻结;候选改写在这些阶段一律拒绝。
    if (state.point == HookPoint::PreRequest && def.stage != Stage::Mutate && !state.frozen) {
        state.frozen = true;
    }

    InvocationCtx ctx;
    ctx.dispatch_id = state.dispatch_id;
    ctx.invocation_id = NextMiddlewareInvocationId(state.dispatch_id, record.definition_order);
    ctx.registry_revision = state.revision;
    ctx.point = state.point;
    ctx.stage = def.stage;
    ctx.hook_id = def.Key();
    ctx.definition_hash = def.definition_hash;
    ctx.depth = static_cast<int>(chain_pos);
    ctx.turn_id = state.trigger.turn_id;
    ctx.step_id = state.trigger.step_id;
    ctx.action_id = state.trigger.action_id;
    ctx.request_id = state.trigger.request_id;
    ctx.cancel = state.trigger.cancel;
    const InvocationMeta meta = MakeMeta(ctx, record);

    if (state.sink != nullptr) {
        state.sink->OnInvocationStarted(meta);
    }

    // next 边界:候选先过宿主校验(挂点/阶段合同 + freeze),采用后跑下游
    // (至多一次由 NextCall 把守;重复调用不增加下游执行)。
    const nlohmann::json frame_input = input;
    NextCall next([&state, &def, &meta, &record, chain_pos, frame_input](
                      const std::optional<nlohmann::json>& candidate) -> DownstreamOutcome {
        nlohmann::json effective_input = frame_input;
        if (candidate.has_value()) {
            // 候选先存(§7.1):before_next 提案不冒充 handler 已完成;随后
            // 按合同 applied/rejected。
            if (state.sink != nullptr) {
                state.sink->OnOutputProposed(meta, "before_next", *candidate);
            }
            if (!InputRewriteAllowed(state.point, def.stage)) {
                EffectRecord rejected;
                rejected.type = std::string(ToString(EffectType::InputRewrite));
                rejected.payload = *candidate;
                rejected.reject_reason = state.frozen ? "输入已冻结(freeze 之后不许改写)"
                                                      : "挂点不收输入改写(" + std::string(ToString(state.point)) + ")";
                record.effects.push_back(rejected);
                if (state.sink != nullptr) {
                    state.sink->OnEffectRejected(meta, ToString(EffectType::InputRewrite), rejected.reject_reason);
                    state.sink->OnEffectSettled(meta, ToString(EffectType::InputRewrite), /*applied=*/false,
                                                rejected.reject_reason, *candidate);
                }
                // 拒绝 != 失败:按进入本 handler 的版本继续(实际处置已记录)。
            } else {
                EffectRecord adopted;
                adopted.type = std::string(ToString(EffectType::InputRewrite));
                adopted.payload = *candidate;
                adopted.applied = true;
                record.effects.push_back(std::move(adopted));
                if (state.sink != nullptr) {
                    state.sink->OnEffectApplied(meta, ToString(EffectType::InputRewrite));
                    state.sink->OnEffectSettled(meta, ToString(EffectType::InputRewrite), /*applied=*/true, {},
                                                *candidate);
                }
                effective_input = *candidate;
            }
        }
        if (state.sink != nullptr) {
            state.sink->OnContinuationConsumed(meta);  // 一次性执行权(§7.1)
        }
        return ToDownstream(RunFrame(state, chain_pos + 1, effective_input));
    });

    const Handler& handler = def.builtin;  // 发布期 lua 已物化成同形 Handler
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [started] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };

    std::expected<HandlerReturn, HandlerError> result =
        std::unexpected(HandlerError{std::string(err::kHandlerFailed), "定义没有可执行 handler"});
    if (handler) {
        try {
            result = handler(ctx, input, next);
        } catch (const std::exception& e) {
            result = std::unexpected(
                HandlerError{std::string(err::kHandlerFailed), std::string("handler 抛异常: ") + e.what()});
        } catch (...) {
            result = std::unexpected(HandlerError{std::string(err::kHandlerFailed), "handler 抛未知异常"});
        }
    }
    record.duration_ms = elapsed_ms();
    record.next_consumed = next.consumed();
    record.next_calls = next.calls();

    if (!result.has_value()) {
        record.outcome = "failed";
        record.error_code = result.error().code;
        record.detail = result.error().message;
        if (state.sink != nullptr) {
            state.sink->OnInvocationFailed(meta, record.error_code, record.duration_ms);
        }
        // required 槽位恒 Abort:替换实现失败不许绕过检查继续(§4.36/§4.48)。
        const FailurePolicy policy = def.required ? FailurePolicy::Abort : def.failure_policy;
        if (policy == FailurePolicy::KeepOriginal && !next.consumed() && next.calls() == 0) {
            // optional 纯转换失败且未消费 next:以进入本 handler 的版本继续一次。
            record.detail += ";按 keep_original 以原输入继续";
            return RunFrame(state, chain_pos + 1, frame_input);
        }
        if (policy == FailurePolicy::KeepOriginal && next.consumed() && next.last() != nullptr) {
            // next 已消费、下游已完成:采用下游收据(keep_downstream,§六)。
            record.detail += ";按 keep_original 采用下游收据";
            return ToFrameResult(*next.last());
        }
        // Abort:下游未跑则跳过留痕(退栈);已跑则收据已在账上。
        if (!next.consumed()) {
            MarkSkippedRemaining(state, chain_pos + 1, "skipped_failed_upstream",
                                 "上游 " + record.key + " 失败: " + record.error_code);
        }
        FrameResult failed;
        failed.kind = DispatchOutcome::Kind::Failed;
        failed.code = record.error_code;
        failed.message = record.key + ": " + record.detail;
        return failed;
    }

    // 正常返回。deny 先记准入效果,再按 next 消费与否收口。
    if (result->deny) {
        RecordEffect(state, record, meta, EffectType::AdmissionDecision,
                     nlohmann::json{{"decision", "deny"}, {"code", result->deny_code}, {"message", result->deny_message}},
                     /*observer=*/false);
    }
    for (const Effect& effect : result->effects) {
        RecordEffect(state, record, meta, effect.type, effect.payload, /*observer=*/false);
    }

    if (!next.consumed()) {
        // 零次 next = 短路(§四):本帧返回即整链终态,未进入项记 skipped。
        const bool denied = result->deny;
        record.outcome = denied ? "denied" : "completed_short_circuit";
        state.outcome.adopted_input = frame_input;
        MarkSkippedRemaining(state, chain_pos + 1, "skipped_short_circuit", "上游 " + record.key + " 短路");
        if (state.sink != nullptr) {
            state.sink->OnOutputProposed(meta, "short_circuit", ProposalPayload(*result));
            state.sink->OnInvocationCompleted(
                meta, denied ? std::optional<std::string>("deny") : std::optional<std::string>("short_circuit"),
                record.duration_ms);
        }
        FrameResult frame;
        frame.kind = denied ? DispatchOutcome::Kind::Denied : DispatchOutcome::Kind::Completed;
        frame.value = result->output;
        frame.code = result->deny_code;
        frame.message = result->deny_message;
        return frame;
    }

    // 已消费 next:本帧后置加工。值可换,结局只许变严(不能洗掉下游 deny/失败)。
    record.outcome = "completed";
    if (state.sink != nullptr) {
        state.sink->OnOutputProposed(meta, "after_next", ProposalPayload(*result));
        state.sink->OnInvocationCompleted(meta, result->deny ? std::optional<std::string>("deny") : std::nullopt,
                                          record.duration_ms);
    }
    FrameResult frame;
    frame.kind = DispatchOutcome::Kind::Completed;
    frame.value = result->output.is_null() && next.last() != nullptr ? next.last()->value : result->output;
    frame.code = result->deny_code;
    frame.message = result->deny_message;
    if (result->deny) {
        frame.kind = DispatchOutcome::Kind::Denied;
    } else if (next.last() != nullptr) {
        frame.kind = WorseKind(frame.kind, ToFrameResult(*next.last()).kind);
    }
    return frame;
}

}  // namespace

DispatchOutcome MiddlewareDispatcher::Dispatch(HookPoint point, const DispatchTrigger& trigger, TerminalFn terminal,
                                               MiddlewareEventSink* sink) {
    DispatchState state;
    state.point = point;
    state.dispatch_id = NextMiddlewareDispatchId();
    state.revision = registry_->revision();
    state.trigger = trigger;
    state.sink = sink;
    state.terminal = std::move(terminal);
    state.outcome.dispatch_id = state.dispatch_id;
    state.outcome.registry_revision = state.revision;

    // 计划冻结:dispatch 时取获选定义(shared_ptr 只读快照;执行中注册表
    // 怎么改都不影响本次——"单次 dispatch 固定计划与脚本版本")。
    for (const auto& def : registry_->Selected(point)) {
        PlanEntry entry;
        entry.def = def;
        if (trigger.stage_filter.has_value() && def->stage != *trigger.stage_filter) {
            entry.matched = false;
            entry.skip_reason = "stage 段外(宿主分段驱动)";
        } else if (!def->match.Empty() && !def->match.Matches(trigger)) {
            entry.matched = false;
            entry.skip_reason = "条件未命中(origin/purpose/deliveryMode)";
        }
        state.entries.push_back(std::move(entry));
    }

    // 记录槽按计划序预置(含跳过项——可查所用配置与跳过原因,不伪造执行,
    // 也不为每条无关配置刷事件)。
    int order = 0;
    for (const auto& entry : state.entries) {
        InvocationRecord record;
        record.key = entry.def->Key();
        record.implementation_ref = entry.def->implementation_ref;
        record.definition_hash = entry.def->definition_hash;
        record.source_label = entry.def->source_label;
        record.handler_kind = entry.def->is_lua ? "lua" : "builtin";
        record.definition_order = order++;
        record.observer = entry.def->observer;
        record.outcome = entry.matched ? "pending" : "skipped_no_match";
        record.detail = entry.matched ? std::string() : entry.skip_reason;
        state.outcome.records.push_back(std::move(record));
    }
    for (std::size_t i = 0; i < state.entries.size(); ++i) {
        if (!state.entries[i].matched) {
            continue;
        }
        if (state.entries[i].def->observer) {
            state.observer_index.push_back(i);
        } else {
            state.chain_index.push_back(i);
        }
    }

    DispatchMeta meta;
    meta.dispatch_id = state.dispatch_id;
    meta.hook_point = std::string(ToString(point));
    meta.registry_revision = state.revision;
    meta.turn_id = trigger.turn_id;
    meta.step_id = trigger.step_id;
    meta.action_id = trigger.action_id;
    meta.request_id = trigger.request_id;

    if (state.chain_index.empty()) {
        // 无匹配链项:整次记 skipped(汇总;未命中项各自躺在 records 里)。
        if (sink != nullptr) {
            sink->OnSkipped(meta, state.entries.empty() ? "no_handlers" : "no_matched_handlers");
        }
    } else if (sink != nullptr) {
        std::vector<HandlerSnapshot> snapshots;
        for (const std::size_t i : state.chain_index) {
            const MiddlewareDefinition& def = *state.entries[i].def;
            HandlerSnapshot snapshot;
            snapshot.hook_id = def.Key();
            snapshot.definition_hash = def.definition_hash;
            snapshot.handler_kind = def.is_lua ? "lua" : "builtin";
            snapshot.definition_order = state.outcome.records[i].definition_order;
            snapshot.failure_policy = std::string(ToString(def.failure_policy));
            snapshots.push_back(std::move(snapshot));
        }
        sink->OnDispatchRequested(meta, snapshots);
    }

    // 链执行(串行改写;后项读前项已采用版本)。
    FrameResult frame = RunFrame(state, 0, trigger.input);

    // 观察者:并发跑、join 后才返回(它们本就不能给改变链路的效果,晚到
    // 也追改不了链结果)。
    if (!state.observer_index.empty()) {
        std::vector<InvocationRecord> results(state.observer_index.size());
        std::vector<std::thread> workers;
        workers.reserve(state.observer_index.size());
        for (std::size_t i = 0; i < state.observer_index.size(); ++i) {
            workers.emplace_back([&state, &results, i] {
                results[i] = RunObserver(state, state.observer_index[i]);
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        for (std::size_t i = 0; i < state.observer_index.size(); ++i) {
            state.outcome.records[state.observer_index[i]] = std::move(results[i]);
        }
    }

    // 已采用的 context.append 按计划序归并(单一事实,观察者不因并发乱序)。
    for (const InvocationRecord& record : state.outcome.records) {
        for (const EffectRecord& effect : record.effects) {
            if (effect.applied && effect.type == std::string(ToString(EffectType::ContextAppend)) &&
                effect.payload.contains("text") && effect.payload["text"].is_string()) {
                state.outcome.context_appends.push_back(effect.payload["text"].get<std::string>());
            }
        }
    }

    state.outcome.kind = frame.kind;
    state.outcome.value = frame.value;
    state.outcome.terminal_runs = state.terminal_runs;
    if (frame.kind == DispatchOutcome::Kind::Denied) {
        state.outcome.deny_code = frame.code;
        state.outcome.deny_message = frame.message;
    } else if (frame.kind == DispatchOutcome::Kind::Failed) {
        state.outcome.error_code = frame.code;
        state.outcome.error_detail = frame.message;
    }
    state.outcome.input_frozen = state.frozen;
    return std::move(state.outcome);
}

}  // namespace lubancode::hooks::middleware
