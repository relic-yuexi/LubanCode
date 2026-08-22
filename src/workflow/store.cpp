// Workflow Store 与 resolver 实现(自然语言编排单第 2 批)。

#include "workflow/store.hpp"

#include <shared_mutex>
#include <sstream>
#include <utility>

namespace lubancode::workflow {

void Store::Initialize(const nlohmann::json& inputs, const nlohmann::json& run_meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    inputs_ = inputs;
    run_meta_ = run_meta;
    initialized_ = true;
}

bool Store::CommitOutput(const std::string& node_id, const nlohmann::json& output) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (outputs_.count(node_id) > 0) return false;
    outputs_.emplace(node_id, output);
    return true;
}

bool Store::CommitOutputOverwrite(const std::string& node_id, const nlohmann::json& output) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool first = outputs_.count(node_id) == 0;
    outputs_.insert_or_assign(node_id, output);
    return first;
}

void Store::UpdateMeta(const std::string& node_id, const nlohmann::json& meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json& target = metas_[node_id];
    if (!target.is_object()) target = nlohmann::json::object();
    for (auto it = meta.begin(); it != meta.end(); ++it) {
        target[it.key()] = it.value();
    }
}

bool Store::HasOutput(const std::string& node_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outputs_.count(node_id) > 0;
}

std::optional<nlohmann::json> Store::GetOutput(const std::string& node_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = outputs_.find(node_id);
    if (it == outputs_.end()) return std::nullopt;
    return it->second;
}

std::optional<nlohmann::json> Store::GetMeta(const std::string& node_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = metas_.find(node_id);
    if (it == metas_.end()) return std::nullopt;
    return it->second;
}

nlohmann::json Store::ToJson() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json out = nlohmann::json::object();
    out["inputs"] = inputs_;
    out["run"] = run_meta_;
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [id, output] : outputs_) {
        nlohmann::json entry = nlohmann::json::object();
        entry["output"] = output;
        const auto meta_it = metas_.find(id);
        if (meta_it != metas_.end()) entry["meta"] = meta_it->second;
        nodes[id] = std::move(entry);
    }
    out["nodes"] = std::move(nodes);
    return out;
}

void Store::TakeFrom(Store&& other) {
    inputs_ = std::move(other.inputs_);
    run_meta_ = std::move(other.run_meta_);
    outputs_ = std::move(other.outputs_);
    metas_ = std::move(other.metas_);
    initialized_ = other.initialized_;
}

Store Store::FromJson(const nlohmann::json& j) {
    Store store;
    if (j.is_object()) {
        if (const auto it = j.find("inputs"); it != j.end() && it->is_object()) store.inputs_ = *it;
        if (const auto it = j.find("run"); it != j.end() && it->is_object()) store.run_meta_ = *it;
        if (const auto it = j.find("nodes"); it != j.end() && it->is_object()) {
            for (auto node = it->begin(); node != it->end(); ++node) {
                if (!node->is_object()) continue;
                if (const auto output = node->find("output"); output != node->end()) {
                    store.outputs_.emplace(node.key(), *output);
                }
                if (const auto meta = node->find("meta"); meta != node->end() && meta->is_object()) {
                    store.metas_.emplace(node.key(), *meta);
                }
            }
        }
    }
    store.initialized_ = true;
    return store;
}

// ---------------------------------------------------------------------------
// ${...} resolver
// ---------------------------------------------------------------------------

namespace {

// JSON 路径下钻:段表走对象 key,数字段走数组下标。找不到给 nullopt。
const nlohmann::json* DrillDown(const nlohmann::json& root, const std::vector<std::string>& segments) {
    const nlohmann::json* current = &root;
    for (const std::string& seg : segments) {
        if (current->is_object()) {
            const auto it = current->find(seg);
            if (it == current->end()) return nullptr;
            current = &it.value();
        } else if (current->is_array()) {
            char* end = nullptr;
            const long index = std::strtol(seg.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || index < 0 ||
                static_cast<std::size_t>(index) >= current->size()) {
                return nullptr;
            }
            current = &(*current)[static_cast<std::size_t>(index)];
        } else {
            return nullptr;
        }
    }
    return current;
}

std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        segments.push_back(path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return segments;
}

}  // namespace

std::expected<nlohmann::json, ResolveError> ResolveRef(const Store& store, const std::string& ref) {
    ResolveError err;
    err.path = "${" + ref + "}";
    std::string inner = ref;
    // 容忍带 ${} 包裹的原样输入。
    if (inner.size() >= 3 && inner.substr(0, 2) == "${" && inner.back() == '}') {
        inner = inner.substr(2, inner.size() - 3);
    }
    if (inner.empty()) {
        err.message = "空的 ${} 引用";
        return std::unexpected(err);
    }

    const std::string first = inner.substr(0, inner.find('.'));
    std::string rest;
    if (const std::size_t dot = inner.find('.'); dot != std::string::npos) {
        rest = inner.substr(dot + 1);
    }

    if (first == "inputs") {
        const nlohmann::json* found = DrillDown(store.inputs(), SplitPath(rest));
        if (found == nullptr) {
            err.message = "inputs 里没有字段 '" + rest + "';检查 input_schema 的 required";
            return std::unexpected(err);
        }
        return *found;
    }
    if (first == "run") {
        const nlohmann::json* found = DrillDown(store.run_meta(), SplitPath(rest));
        if (found == nullptr) {
            err.message = "run 元数据里没有 '" + rest + "'";
            return std::unexpected(err);
        }
        return *found;
    }
    if (first == "vars" || first == "artifacts") {
        err.message = "'" + first + "' 分区首版未开放(无 assign/artifact 节点)";
        return std::unexpected(err);
    }
    if (first == "nodes") {
        const std::size_t dot = rest.find('.');
        if (dot == std::string::npos) {
            err.message = "nodes 引用缺字段:nodes.<id> 之后要 .output 或 .meta";
            return std::unexpected(err);
        }
        const std::string node_id = rest.substr(0, dot);
        const std::string field = rest.substr(dot + 1);
        const std::string kind = field.substr(0, field.find('.'));
        if (kind != "output" && kind != "meta") {
            err.message = "nodes.<id> 之后只认 .output/.meta,不认 '" + kind + "'";
            return std::unexpected(err);
        }
        const std::optional<nlohmann::json> source =
            kind == "output" ? store.GetOutput(node_id) : store.GetMeta(node_id);
        if (!source.has_value()) {
            err.message = "节点 '" + node_id + "' 还没有 " + kind +
                          "(未完成或不存在);resolver 只读已完成节点";
            return std::unexpected(err);
        }
        std::string tail = field.substr(kind.size());
        if (!tail.empty() && tail.front() == '.') tail = tail.substr(1);
        const nlohmann::json* found = tail.empty() ? &*source : DrillDown(*source, SplitPath(tail));
        if (found == nullptr) {
            err.message = "节点 '" + node_id + "' 的 " + kind + " 里没有 '" + tail + "'";
            return std::unexpected(err);
        }
        return *found;
    }
    err.message = "认不得的引用前缀 '" + first + "'(认 inputs/vars/nodes/artifacts/run)";
    return std::unexpected(err);
}

std::expected<ResolvedValue, ResolveError> ResolveTemplate(const Store& store, const nlohmann::json& input) {
    ResolvedValue out;
    if (input.is_string()) {
        const std::string& text = input.get<std::string>();
        // 整值单引用:保持类型。
        if (text.size() > 4 && text.front() == '$' && text[1] == '{' && text.back() == '}') {
            const std::string inner = text.substr(2, text.size() - 3);
            if (inner.find("${") == std::string::npos) {
                auto resolved = ResolveRef(store, inner);
                if (!resolved.has_value()) return std::unexpected(resolved.error());
                out.value = std::move(*resolved);
                out.from_single_ref = true;
                return out;
            }
        }
        // 混排:逐段替换,全按字符串拼。
        std::string result;
        std::size_t pos = 0;
        while (true) {
            const std::size_t open = text.find("${", pos);
            if (open == std::string::npos) {
                result += text.substr(pos);
                break;
            }
            result += text.substr(pos, open - pos);
            const std::size_t close = text.find('}', open);
            if (close == std::string::npos) {
                ResolveError err;
                err.path = text;
                err.message = "${ 没有配对的 }";
                return std::unexpected(err);
            }
            const std::string inner = text.substr(open + 2, close - open - 2);
            auto resolved = ResolveRef(store, inner);
            if (!resolved.has_value()) return std::unexpected(resolved.error());
            if (resolved->is_string()) {
                result += resolved->get<std::string>();
            } else {
                result += resolved->dump();
            }
            pos = close + 1;
        }
        out.value = std::move(result);
        return out;
    }
    if (input.is_object()) {
        nlohmann::json obj = nlohmann::json::object();
        for (auto it = input.begin(); it != input.end(); ++it) {
            auto resolved = ResolveTemplate(store, it.value());
            if (!resolved.has_value()) return std::unexpected(resolved.error());
            obj[it.key()] = std::move(resolved.value().value);
        }
        out.value = std::move(obj);
        return out;
    }
    if (input.is_array()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : input) {
            auto resolved = ResolveTemplate(store, item);
            if (!resolved.has_value()) return std::unexpected(resolved.error());
            arr.push_back(std::move(resolved.value().value));
        }
        out.value = std::move(arr);
        return out;
    }
    out.value = input;  // 字面量直通
    return out;
}

}  // namespace lubancode::workflow
