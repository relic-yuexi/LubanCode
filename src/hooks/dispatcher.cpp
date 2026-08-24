#include "hooks/dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <regex>
#include <set>
#include <thread>

#include "platform/process.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::hooks {

namespace {

using platform::ProcessResult;

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 单只 handler 的执行结果(Emit 的并发 worker 填,主线程按定义序收账)。
struct HandlerRun {
    const HookDefinition* def = nullptr;
    ProcessResult exec{};
    int duration_ms = 0;
};

// v2 执行:exec form 有 args 走 RunProcessWithStdin(不经 shell,Windows
// 引号泥潭免了),shell 字符串形式走 RunShellCommandWithStdin。stdin 全量
// JSON——大输入不塞环境变量。${LUBANCODE_PROJECT_DIR} 占位符已在装载期
// 替换进命令与参数。
ProcessResult ExecuteV2(const HookDefinition& def, const std::string& stdin_json) {
#ifdef _WIN32
    const std::string& command =
        !def.handler.command_windows.empty() ? def.handler.command_windows : def.handler.command;
    const std::vector<std::string>& args =
        !def.handler.args_windows.empty() ? def.handler.args_windows : def.handler.args;
#else
    const std::string& command = def.handler.command;
    const std::vector<std::string>& args = def.handler.args;
#endif
    if (!args.empty()) {
        std::vector<std::string> argv;
        argv.push_back(command);
        argv.insert(argv.end(), args.begin(), args.end());
        return platform::RunProcessWithStdin(argv, stdin_json, def.handler.timeout_ms);
    }
    return platform::RunShellCommandWithStdin(command, stdin_json, def.handler.timeout_ms);
}

// legacy 执行:照旧——shell 字符串、LUBAN_TOOL_* 环境变量、固定 30 秒、
// 不吃 stdin。环境变量的内容与 M9 的 tools/hooks.cpp 逐条对齐。
platform::EnvPairs BuildLegacyEnv(const HookPayload& payload) {
    // 旧协议只有工具事件带环境变量;session_start/session_end 一个没有。
    if (!EventMatchesOnToolName(payload.event)) {
        return {};
    }
    const std::string tool_name = payload.fields.value("tool_name", std::string());
    const nlohmann::json empty_input = nlohmann::json::object();
    const nlohmann::json& tool_input =
        payload.fields.contains("tool_input") ? payload.fields.at("tool_input") : empty_input;
    std::optional<std::string> tool_result;
    bool is_error = false;
    if (payload.fields.contains("tool_response_text")) {
        tool_result = payload.fields.at("tool_response_text").get<std::string>();
        is_error = !payload.fields.value("tool_succeeded", true);
    }
    return BuildLegacyToolEnv(tool_name, tool_input, tool_result, is_error);
}

ProcessResult ExecuteLegacy(const HookDefinition& def, const HookPayload& payload) {
    return platform::RunShellCommand(def.handler.command, def.handler.timeout_ms, BuildLegacyEnv(payload));
}

std::string FirstLines(const std::string& text, std::size_t max_bytes) {
    std::string snippet = text;
    if (snippet.size() > max_bytes) {
        snippet.resize(lubancode::platform::Utf8PrefixBoundary(snippet, max_bytes));
    }
    // 只要前 5 行,拦截说明带一点上下文又不至于刷屏(与 M9 同款)。
    std::string out;
    int lines = 0;
    std::size_t pos = 0;
    while (pos <= snippet.size() && lines < 5) {
        const std::size_t nl = snippet.find('\n', pos);
        const std::string line = (nl == std::string::npos) ? snippet.substr(pos) : snippet.substr(pos, nl - pos);
        if (!out.empty()) {
            out += "\n";
        }
        out += line;
        ++lines;
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

}  // namespace

std::string HookDispatcher::NextHookRunId() {
    static std::atomic<unsigned long long> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "hookrun_" + std::to_string(ms) + "_" + std::to_string(counter.fetch_add(1));
}

// 拷贝语义见头文件注释:账照搬,锁与投递队列不跟。锁序恒为
// definitions_mutex_ -> external_mutex_,与投递/归并路径不反。
HookDispatcher::HookDispatcher(const HookDispatcher& other) {
    const std::lock_guard<std::mutex> defs_lock(other.definitions_mutex_);
    const std::lock_guard<std::mutex> ext_lock(other.external_mutex_);
    definitions_ = other.definitions_;
    trust_ = other.trust_;
    context_ = other.context_;
    recent_ = other.recent_;
    last_record_ = other.last_record_;
}

HookDispatcher& HookDispatcher::operator=(const HookDispatcher& other) {
    if (this == &other) {
        return *this;
    }
    std::lock_guard<std::mutex> defs_lock(definitions_mutex_);
    std::lock_guard<std::mutex> ext_lock(external_mutex_);
    std::lock_guard<std::mutex> other_defs_lock(other.definitions_mutex_);
    std::lock_guard<std::mutex> other_ext_lock(other.external_mutex_);
    definitions_ = other.definitions_;
    trust_ = other.trust_;
    context_ = other.context_;
    recent_ = other.recent_;
    last_record_ = other.last_record_;
    return *this;
}

HookDispatcher::HookDispatcher(HookDispatcher&& other) : HookDispatcher(other) {}

HookDispatcher& HookDispatcher::operator=(HookDispatcher&& other) { return *this = other; }

HookDispatcher::ConfigureResult HookDispatcher::Configure(LoadedHooks loaded, HookTrustStore trust,
                                                          HookContext base_context) {
    ConfigureResult out;
    out.has_untrusted_project = loaded.has_untrusted_project;
    out.has_disabled = loaded.has_disabled;
    {
        const std::lock_guard<std::mutex> lock(definitions_mutex_);
        definitions_ = std::move(loaded.definitions);
        for (std::size_t i = 0; i < definitions_.size(); ++i) {
            definitions_[i].id = static_cast<int>(i) + 1;
        }
    }
    trust_ = std::move(trust);
    context_ = std::move(base_context);
    out.definition_count = static_cast<int>(definitions_.size());
    return out;
}

bool HookDispatcher::HasHandlersFor(HookEvent event) const {
    for (const auto& def : definitions_) {
        if (def.event == event) {
            return true;
        }
    }
    return false;
}

bool HookDispatcher::MatcherHits(const HookDefinition& def, const std::string& match_value) {
    const std::string& matcher = def.matcher;
    if (matcher.empty() || matcher == "*") {
        return true;
    }
    if (match_value.empty()) {
        return false;  // 事件没有可匹配字段,具体 matcher 命不中
    }
    if (def.regex) {
        try {
            return std::regex_match(match_value, std::regex(matcher));
        } catch (const std::regex_error&) {
            return false;  // 坏正则按不命中兜底(不该走到:装载期已显式 regex)
        }
    }
    // 精确名或竖线集合:"run_command|write_file"。
    std::size_t pos = 0;
    while (pos <= matcher.size()) {
        const std::size_t bar = matcher.find('|', pos);
        const std::string name = (bar == std::string::npos) ? matcher.substr(pos) : matcher.substr(pos, bar - pos);
        if (name == match_value) {
            return true;
        }
        if (bar == std::string::npos) {
            break;
        }
        pos = bar + 1;
    }
    return false;
}

HookEventResult HookDispatcher::Emit(HookEvent event, const HookPayload& payload) {
    // 记账所需的 context_ 快照在 EmitImpl 里读;EmitWith 走覆写版。
    return EmitImpl(event, payload, context_, /*context_override=*/false);
}

HookEventResult HookDispatcher::EmitWith(HookEvent event, const HookPayload& payload, HookContext ctx) {
    return EmitImpl(event, payload, ctx, /*context_override=*/true);
}

HookEventResult HookDispatcher::EmitImpl(HookEvent event, const HookPayload& payload, const HookContext& ctx,
                                         bool /*context_override*/) {
    HookEventResult merged = RunEventCore(definitions_, event, payload, ctx);
    // ---- 落账:结果账(定义序)、每只定义的最近一次、会话级流水(新在头)。
    // 失败不只往 cerr 丢一行——先落账,UI 怎么呈由调用方定。跳过项(未
    // 信任/禁用/去重/async)同样入账:/hooks 的"最近结果"看得见它们。
    for (const auto& record : merged.records) {
        last_record_[record.definition_id] = record;
        recent_.push_front(record);
    }
    while (recent_.size() > kRecentCap) {
        recent_.pop_back();
    }
    return merged;
}

// Emit/EmitDetached 共用的执行核:定义表参数化,不碰成员。主线程的 Emit 传
// definitions_,后台执行器(EmitDetached)传只读快照——账本写不写由外层定。
HookEventResult HookDispatcher::RunEventCore(const std::vector<HookDefinition>& definitions, HookEvent event,
                                              const HookPayload& payload, const HookContext& ctx) {
    HookEventResult merged;
    const std::string hook_run_id = NextHookRunId();
    const std::string event_name(ToString(event));
    const EventOutputCapabilities caps = OutputCapabilities(event);

    // 每条命中的定义一个账本槽(定义序:来源 -> 声明次序)。跳过项第一遍
    // 就填好,执行项第三遍回填——merged.records 与运行流水都按这个序落账,
    // 不按谁先跑完谁先说话。
    struct Slot {
        const HookDefinition* def = nullptr;
        bool executed = false;
        HookRunRecord record;
    };
    std::vector<Slot> slots;
    std::vector<Slot*> to_run;
    std::set<std::string> scheduled_hashes;  // 本轮已排进执行的单(去重账)

    // ---- 第一遍:筛选与跳过记录。----
    for (const auto& def : definitions) {
        if (def.event != event || !MatcherHits(def, payload.match_value)) {
            continue;
        }
        Slot slot;
        slot.def = &def;
        slot.record.definition_id = def.id;
        slot.record.event_name = event_name;
        slot.record.definition_hash_short = def.definition_hash_short;
        slot.record.command_display = HookCommandDisplay(def.handler);
        slot.record.source_label = def.source_label;
        slot.record.timestamp_unix = UnixNow();
        // 逐枚追踪单:运行账钉在工具 execution 上(可空,非工具事件没有)。
        slot.record.tool_execution_id = ctx.tool_execution_id;

        if (def.disabled) {
            slot.record.outcome = "skipped_disabled";
            slot.record.detail = "已在 /hooks 里禁用";
            slots.push_back(std::move(slot));
            continue;
        }
        if (!def.trusted) {
            slot.record.outcome = "skipped_untrusted";
            slot.record.detail =
                "项目 hook 未信任(hash " + def.definition_hash_short + "),不起进程;/hooks 审查后信任即生效";
            slots.push_back(std::move(slot));
            continue;
        }
        if (scheduled_hashes.count(def.definition_hash) > 0) {
            // 同事件下 definition hash 相同的另一条(装载期已标 deduped,这里
            // 双保险):来源账保留,执行只跑一次。
            slot.record.outcome = "skipped_dedupe";
            slot.record.detail = "与本事件下另一条同命令定义去重,只执行一次";
            slots.push_back(std::move(slot));
            continue;
        }
        if (def.handler.async) {
            slot.record.outcome = "skipped_async";
            slot.record.detail = "async handler 本期不执行(安全点投递未实现);不假装支持,也不拿它做决定";
            slots.push_back(std::move(slot));
            continue;
        }
        scheduled_hashes.insert(def.definition_hash);
        slot.executed = true;
        to_run.push_back(nullptr);  // 占位,第二遍前回填指针
        slots.push_back(std::move(slot));
    }

    // to_run 填指针(slots 不会再到 moved-from 状态,取址稳定)。
    {
        std::size_t run_index = 0;
        for (auto& slot : slots) {
            if (slot.executed) {
                to_run[run_index++] = &slot;
            }
        }
    }

    // ---- 第二遍:并发执行。每只同步 handler 一条线程,收齐再归并——一只
    // hook 慢/挂不阻止另一只启动,总耗时接近最慢一只,不是全数相加。----
    std::vector<HandlerRun> runs(to_run.size());
    std::vector<std::thread> workers;
    workers.reserve(to_run.size());
    for (std::size_t i = 0; i < to_run.size(); ++i) {
        workers.emplace_back([i, &runs, &to_run, &payload, &hook_run_id, &ctx]() {
            HandlerRun& run = runs[i];
            run.def = to_run[i]->def;
            const auto start = std::chrono::steady_clock::now();
            if (to_run[i]->def->legacy) {
                run.exec = ExecuteLegacy(*to_run[i]->def, payload);
            } else {
                const nlohmann::json stdin_json = BuildStdinPayload(payload, ctx, hook_run_id);
                run.exec = ExecuteV2(*to_run[i]->def, stdin_json.dump());
            }
            run.duration_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                    .count());
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    // ---- 第三遍:按定义序归并(不是完成序)。执行结果回填进各自槽位。----
    for (std::size_t i = 0; i < runs.size(); ++i) {
        Slot& slot = *to_run[i];
        const HookDefinition& def = *slot.def;
        const ProcessResult& exec = runs[i].exec;

        HookRunRecord& record = slot.record;
        record.duration_ms = runs[i].duration_ms;
        record.exit_code = exec.exit_code;
        record.timestamp_unix = UnixNow();
        ++merged.executed;

        // 本次运行里,这只 handler 的单份表态(stdout 解析结果也要带回归并)。
        HookOutput parsed;
        bool parsed_valid = false;

        if (def.legacy) {
            // legacy 语义(M9 原样):pre_tool 任意非零退出都拦;post/session
            // 非零只警告;spawn 失败/超时按放行处理,警告可见。
            if (exec.spawn_failed) {
                record.outcome = "spawn_failed";
                record.detail = "钩子起不来: " + exec.spawn_error;
            } else if (exec.timed_out) {
                record.outcome = "timeout";
                record.detail = "钩子超时(30 秒),按放行处理";
            } else if (exec.exit_code != 0 && event == HookEvent::PreToolUse) {
                record.outcome = "blocked";
                record.decision = "deny";
                record.detail = "退出码 " + std::to_string(exec.exit_code) + ": " + FirstLines(exec.output, 1024);
            } else if (exec.exit_code != 0) {
                record.outcome = "failure";
                record.detail = "退出码 " + std::to_string(exec.exit_code) + ": " + FirstLines(exec.output, 1024);
            } else {
                record.outcome = "ok";
            }
        } else {
            // v2:退出码三分 + stdout 逐事件 schema。stdout/stderr 分开按明示
            // 编码解码(先认 UTF-8,次选控制台输出页/系统 ANSI 页,命中标注;
            // 都解不动留原始字节摘要),不让一只钩子的乱码把 JSON 解析打崩,
            // 也不把中文报错无声替换成替换符。
            const DecodedHookText stdout_decoded = DecodeHookStreamBytes(exec.stdout_bytes);
            const DecodedHookText stderr_decoded = DecodeHookStreamBytes(exec.stderr_bytes);
            record.stderr_encoding = stderr_decoded.encoding;
            record.stderr_head = stderr_decoded.text;
            record.stderr_truncated = stderr_decoded.text.size() > HookRunRecord::kStderrHeadBytes;
            if (record.stderr_truncated) {
                record.stderr_head.resize(HookRunRecord::kStderrHeadBytes);
            }
            if (stdout_decoded.from_raw_digest) {
                // stdout 既不是 UTF-8 也解不出任何候选页:契约要求 UTF-8 JSON,
                // 这里如实记 schema_error 并附原始字节摘要,不拿清洗后的文本
                // 硬解析(那是无声替换)。
                parsed.error = "stdout 不是合法 UTF-8(契约要求 UTF-8 JSON),原始字节摘要: " +
                               stdout_decoded.text;
            } else {
                parsed = ParseStdoutJson(event, stdout_decoded.text);
            }
            parsed_valid = parsed.ok;
            const SingleOutcome judged = JudgeSingleRun(event, exec.exit_code, exec.timed_out, exec.spawn_failed,
                                                        parsed, /*stderr_text=*/stderr_decoded.text);
            record.outcome = judged.outcome;
            record.decision = judged.decision;
            record.detail = judged.detail;
            if (parsed_valid && !parsed.system_message.empty()) {
                record.detail = parsed.system_message;
            }
            if (stderr_decoded.from_raw_digest && !record.stderr_head.empty()) {
                record.detail += ";stderr 编码未定,已留原始字节摘要";
            }
        }

        // ---- 归并(定义序,固定法)。----
        const bool permission_event = caps.permission_decision;
        const bool failed =
            record.outcome == "failure" || record.outcome == "timeout" || record.outcome == "spawn_failed" ||
            record.outcome == "schema_error";

        if (parsed_valid) {
            if (!parsed.system_message.empty()) {
                merged.system_messages.push_back(parsed.system_message);
            }
            if (parsed.has_additional_context && !parsed.additional_context.empty()) {
                merged.additional_context.push_back(parsed.additional_context);
            }
            if (parsed.has_continue && !parsed.continue_flag && caps.can_block) {
                merged.blocked = true;
                merged.block_reason = parsed.stop_reason.empty() ? "钩子拉了闸(continue=false)" : parsed.stop_reason;
            }
        }

        if (record.decision == "deny") {
            // exit 2 / legacy 非零拦截 / permissionDecision=deny。权限事件归
            // 进 permission=Deny;可阻断事件(UserPromptSubmit/PreCompact)归
            // 进 blocked;其余事件(PostToolUse 等)没有拦截语义,记录保留,
            // 归并不动——不许冒充"撤销了副作用"。
            if (permission_event) {
                if (merged.permission != HookEventResult::Permission::Deny) {
                    merged.permission = HookEventResult::Permission::Deny;
                    merged.permission_reason =
                          record.event_name + " 钩子阻断: " +
                          (!parsed.permission_reason.empty() ? parsed.permission_reason
                           : (record.detail.empty() ? std::string("未给理由") : record.detail));
                }
            } else if (caps.can_block && (event == HookEvent::UserPromptSubmit || event == HookEvent::PreCompact)) {
                merged.blocked = true;
                if (merged.block_reason.empty()) {
                    merged.block_reason = record.detail.empty() ? "钩子以退出码 2 阻断" : record.detail;
                }
            }
        } else if (record.decision == "ask" || record.decision == "allow") {
            if (permission_event) {
                // deny > ask > allow:已经 deny/ask 的不被 allow 拉回去。
                if (merged.permission == HookEventResult::Permission::None ||
                    (record.decision == "ask" && merged.permission == HookEventResult::Permission::Allow)) {
                    merged.permission =
                          record.decision == "ask" ? HookEventResult::Permission::Ask : HookEventResult::Permission::Allow;
                    if (!parsed.permission_reason.empty()) {
                        merged.permission_reason = parsed.permission_reason;
                    }
                }
            }
        }

        if (parsed_valid && parsed.has_updated_input && permission_event &&
            merged.permission == HookEventResult::Permission::Allow) {
            // updatedInput 只与 allow 同返(schema 已保证);多只都给时按定义
            // 序取最后一只的。
            merged.updated_input = parsed.updated_input;
        }

        if (failed && def.handler.failure_policy == "deny") {
            // failure_policy=deny:门卫没起来(hook 自己坏/超时/起不来/schema
            // 错)按拦截算。只在能拦的事件上生效;legacy 恒 warn(M9 语义
            // 不动)。严禁静默当放行——记录已在,UI 也会报。
            if (permission_event && merged.permission == HookEventResult::Permission::None) {
                merged.permission = HookEventResult::Permission::Deny;
                merged.permission_reason = "钩子失败且 failure_policy=deny: " + record.outcome + " " + record.detail;
            } else if (caps.can_block && !merged.blocked &&
                       (event == HookEvent::UserPromptSubmit || event == HookEvent::PreCompact)) {
                merged.blocked = true;
                merged.block_reason = "钩子失败且 failure_policy=deny: " + record.outcome;
            }
        }

        // 记账在循环外统一做(见下):这里只填自己的槽。
        (void)def;
    }

    for (auto& slot : slots) {
        merged.records.push_back(std::move(slot.record));
    }
    return merged;
}

const HookDefinition* HookDispatcher::FindDefinition(int id) const {
    for (const auto& def : definitions_) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

const HookRunRecord* HookDispatcher::LastRecordFor(int definition_id) const {
    const auto it = last_record_.find(definition_id);
    return it == last_record_.end() ? nullptr : &it->second;
}

std::vector<HookRunRecord> HookDispatcher::RecentRecords(std::size_t cap) const {
    std::vector<HookRunRecord> out;
    out.reserve(std::min(cap, recent_.size()));
    for (const auto& record : recent_) {
        if (out.size() >= cap) {
            break;
        }
        out.push_back(record);
    }
    return out;
}

bool HookDispatcher::TrustDefinition(int id) {
    const std::lock_guard<std::mutex> lock(definitions_mutex_);
    for (auto& def : definitions_) {
        if (def.id == id) {
            trust_.SetTrusted(def.source_path, def.definition_hash, HookCommandDisplay(def.handler));
            def.trusted = true;
            return true;
        }
    }
    return false;
}

bool HookDispatcher::UntrustDefinition(int id) {
    const std::lock_guard<std::mutex> lock(definitions_mutex_);
    for (auto& def : definitions_) {
        if (def.id == id) {
            trust_.Untrust(def.source_path, def.definition_hash);
            // user/managed 不走信任账,恒 trusted;project 撤信后即未信任。
            def.trusted = def.source_kind != HookSourceKind::Project;
            return true;
        }
    }
    return false;
}

bool HookDispatcher::SetDefinitionDisabled(int id, bool disabled) {
    const std::lock_guard<std::mutex> lock(definitions_mutex_);
    for (auto& def : definitions_) {
        if (def.id == id) {
            if (def.source_kind == HookSourceKind::Managed) {
                return false;  // managed 不可禁
            }
            trust_.SetDisabled(def.source_path, def.definition_hash, disabled);
            def.disabled = disabled;
            return true;
        }
    }
    return false;
}

// ---- 后台(外挂)执行面 ---------------------------------------------------

std::vector<HookDefinition> HookDispatcher::PolicySnapshot() const {
    const std::lock_guard<std::mutex> lock(definitions_mutex_);
    return definitions_;
}

HookEventResult HookDispatcher::EmitDetached(const std::vector<HookDefinition>& definitions, HookEvent event,
                                             const HookPayload& payload, const HookContext& ctx) {
    return RunEventCore(definitions, event, payload, ctx);
}

void HookDispatcher::PostExternalRecords(std::vector<HookRunRecord> records, const std::string& warning) {
    const std::lock_guard<std::mutex> lock(external_mutex_);
    ExternalPending pending;
    pending.records = std::move(records);
    if (!warning.empty()) {
        pending.warnings.push_back(warning);
    }
    external_pending_.push_back(std::move(pending));
}

HookDispatcher::ExternalAdoption HookDispatcher::AdoptExternalRecords() {
    std::deque<ExternalPending> incoming;
    {
        const std::lock_guard<std::mutex> lock(external_mutex_);
        incoming.swap(external_pending_);
    }
    ExternalAdoption adoption;
    for (auto& pending : incoming) {
        for (auto& record : pending.records) {
            last_record_[record.definition_id] = record;
            recent_.push_front(record);
            adoption.records.push_back(std::move(record));
        }
        for (auto& warning : pending.warnings) {
            adoption.warnings.push_back(std::move(warning));
        }
    }
    while (recent_.size() > kRecentCap) {
        recent_.pop_back();
    }
    return adoption;
}

bool HookDispatcher::HasExternalRecords() const {
    const std::lock_guard<std::mutex> lock(external_mutex_);
    return !external_pending_.empty();
}

}  // namespace lubancode::hooks
