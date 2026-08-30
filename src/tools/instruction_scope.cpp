// 写前作用域闸的实现(AGENTS.md 作用域单 P0)。头文件见分工注释。
#include "tools/instruction_scope.hpp"

#include <algorithm>
#include <map>
#include <system_error>

#include "tools/isolation.hpp"   // IsolationGuard:隔离房里相对路径的解析基准
#include "tools/path_utils.hpp"  // Utf8ToPath/PathToUtf8:路径串 <-> filesystem::path

namespace lubancode::tools {

bool InstructionScopeState::Seen(const std::string& fingerprint) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seen_.count(fingerprint) > 0;
}

void InstructionScopeState::MarkSeen(const std::string& fingerprint) {
    const std::lock_guard<std::mutex> lock(mutex_);
    seen_.insert(fingerprint);
}

void InstructionScopeState::Reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    seen_.clear();
}

std::vector<std::string> CollectWriteTargets(const std::string& tool_name, const nlohmann::json& input) {
    // 名单式认路径:只知道这些工具的入参形状。别的工具一律返回空——
    // 宁可少拦(它们另有自己的确认关),不瞎猜字段名把读工具也拦了。
    if (tool_name != "write_file" && tool_name != "edit_file") {
        return {};
    }
    if (!input.is_object()) {
        return {};
    }
    const auto it = input.find("path");
    if (it == input.end() || !it->is_string()) {
        return {};
    }
    const std::string& path = it->get<std::string>();
    if (path.empty()) {
        return {};
    }
    return {path};
}

std::string BuildChainInjection(const lubancode::config::InstructionChain& chain, std::size_t budget) {
    // 装配次序与显示次序分开:显示按 root -> nearest(机械优先级原样);
    // 预算从最近那份往回装(近处规则永不让位,单子 §5.4)。
    std::vector<bool> fits(chain.documents.size(), false);
    std::size_t used = 0;
    for (std::size_t i = chain.documents.size(); i-- > 0;) {
        const lubancode::config::InstructionDocument& doc = chain.documents[i];
        // 每份的包装:作用域标题 + 来源行 + 正文 + 段间空行。
        const std::size_t wrapper = doc.scope_dir.generic_string().size() + doc.source_path.generic_string().size() +
                                    64;  // 标题/来源/空行的固定开销估算
        if (used + wrapper + doc.content.size() > budget) {
            continue;
        }
        fits[i] = true;
        used += wrapper + doc.content.size();
    }

    std::vector<std::string> dropped;
    std::string out;
    for (std::size_t i = 0; i < chain.documents.size(); ++i) {
        const lubancode::config::InstructionDocument& doc = chain.documents[i];
        if (!fits[i]) {
            dropped.push_back(doc.source_path.generic_string());
            continue;
        }
        if (!out.empty()) {
            out += "\n\n";
        }
        out += "=== 作用域 " + doc.scope_dir.generic_string() +
               (i + 1 == chain.documents.size() ? "(离目标最近,优先级最高)" : "") + " ===\n";
        out += "(来自 " + doc.source_path.generic_string() + ")\n";
        out += doc.content;
    }
    if (!dropped.empty()) {
        out += "\n\n(因字节预算未完整装入: ";
        for (std::size_t i = 0; i < dropped.size(); ++i) {
            out += (i == 0 ? "" : ", ") + dropped[i];
        }
        out += " — 需要全文可自行 read_file)";
    }
    return out;
}

std::optional<ScopeGateDenial> ScopedInstructionGate::CheckTargets(
    const std::vector<std::string>& utf8_targets) {
    if (utf8_targets.empty()) {
        return std::nullopt;
    }

    // 每目标各自 Resolve,按链指纹分组(§7.4):同指纹的目标共用一份注入,
    // 互不相干的两棵子树各拿各的规则,不揉成一段。
    struct ScopeGroup {
        lubancode::config::InstructionChain chain;
        std::vector<std::string> targets;
    };
    std::vector<std::string> order;  // 首见次序,文案稳定
    std::map<std::string, ScopeGroup> groups;
    std::vector<std::string> unconstrained;  // 空链目标:不受任何指令文档管
    for (const std::string& target : utf8_targets) {
        lubancode::config::InstructionChain chain = resolver_->ResolveForPath(Utf8ToPath(target));
        if (chain.documents.empty()) {
            unconstrained.push_back(target);
            continue;
        }
        // 值拷贝,不做引用:emplace 的实参求值次序不定,ScopeGroup 临时物
        // 一旦先移走 chain,引用就指向被掏空 fingerprint——map 键会拷出
        // 空串,at() 对不上账。
        const std::string fp = chain.fingerprint;
        auto it = groups.find(fp);
        if (it == groups.end()) {
            order.push_back(fp);
            it = groups.emplace(fp, ScopeGroup{std::move(chain), {}}).first;
        }
        it->second.targets.push_back(target);
    }

    // 未确认的作用域:任一在案未见过 → 整笔拦(§7.4"任一未确认,整笔不落")。
    std::vector<const ScopeGroup*> pending;
    for (const std::string& fp : order) {
        if (!state_->Seen(fp)) {
            pending.push_back(&groups.at(fp));
        }
    }
    if (pending.empty()) {
        return std::nullopt;  // 全部已确认:放行
    }

    // 拦截文案:目标 -> 作用域的映射先亮出来,再按组注入规则全文。
    std::string message =
        "[instructions_required] 写入暂缓:下列目标的项目指令(AGENTS.md 作用域链)尚未经本 Agent 确认。\n"
        "这是协议握手,不是错误——细读下面规则后,原样重试同一写操作即可放行。\n\n"
        "目标与作用域(离目标最近的规则优先级最高):\n";
    for (const ScopeGroup* group : pending) {
        for (const std::string& target : group->targets) {
            const std::filesystem::path& nearest =
                group->chain.documents.back().scope_dir;
            message += "- " + target + " <- " + nearest.generic_string() + " (指纹 " +
                       group->chain.fingerprint.substr(0, 8) + ")\n";
        }
    }
    if (!unconstrained.empty()) {
        message += "(另有不受任何 AGENTS.md 管辖的目标,放行路不受影响)\n";
    }
    message += "\n规则正文(按作用域列出,父目录在前、离目标最近者在最后):\n";
    for (const ScopeGroup* group : pending) {
        message += "\n" + BuildChainInjection(group->chain, resolver_->max_bytes()) + "\n";
    }

    // 登记已出示的指纹:模型重试同目标(文档未变)即命中放行。文档若在
    // 其间被改,重试时 Resolve 出新指纹,不在账上——重新拦、重新注入。
    ScopeGateDenial denial;
    for (const ScopeGroup* group : pending) {
        state_->MarkSeen(group->chain.fingerprint);
        denial.presented_fingerprints.push_back(group->chain.fingerprint);
    }
    denial.message = std::move(message);
    return denial;
}

std::function<std::optional<std::string>(const std::string&, const nlohmann::json&)>
BuildScopeGateCallback(std::shared_ptr<const lubancode::config::ProjectInstructionResolver> resolver,
                       std::shared_ptr<InstructionScopeState> state) {
    return [resolver = std::move(resolver), state = std::move(state)](
               const std::string& tool_name, const nlohmann::json& input) -> std::optional<std::string> {
        if (resolver == nullptr || state == nullptr) {
            return std::nullopt;
        }
        std::vector<std::string> targets = CollectWriteTargets(tool_name, input);
        if (targets.empty()) {
            return std::nullopt;
        }
        // 相对路径的解析基准与写工具执行时同一套:隔离房里按房(BaseDirTool
        // 改写入参用的就是它——闸在包装层之前看到的是原始入参,这里不补上
        // 同一基准,链会解析到主 checkout 头上,房内 AGENTS.md 就漏了);
        // 没住房就按进程 cwd(std::filesystem 的缺省基准)。
        for (std::string& target : targets) {
            const std::filesystem::path as_path = Utf8ToPath(target);
            if (as_path.is_absolute()) {
                continue;
            }
            std::filesystem::path base;
            if (const IsolationScope* scope = IsolationGuard::Current(); scope != nullptr) {
                base = Utf8ToPath(scope->base_dir);
            } else {
                std::error_code ec;
                base = std::filesystem::current_path(ec);
            }
            target = PathToUtf8(base / as_path);
        }
        ScopedInstructionGate gate(*resolver, *state);
        std::optional<ScopeGateDenial> denial = gate.CheckTargets(targets);
        if (!denial.has_value()) {
            return std::nullopt;
        }
        return denial->message;
    };
}

void MarkBaselineSeen(const lubancode::config::ProjectInstructionResolver& resolver,
                      InstructionScopeState& state, const std::filesystem::path& cwd,
                      const std::string& prompt_instructions) {
    if (prompt_instructions.empty() || cwd.empty()) {
        return;  // 没有可算"已见"的基线;cwd 空(旧装配)也不猜
    }
    const lubancode::config::InstructionChain chain = resolver.ResolveForPath(cwd);
    if (chain.truncated || chain.content != prompt_instructions) {
        return;  // 没读全 / 串对不上(搬房、外部改动):不预登记
    }
    state.MarkSeen(chain.fingerprint);
}

}  // namespace lubancode::tools
