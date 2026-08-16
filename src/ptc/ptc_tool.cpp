// PtcTool 的实现:stub 集装配、执行链桥接(agent::RunOneTool)、结果排版
// (语义层摘要 + 审计层账目,Ctrl+O 展开看明细)。

#include "ptc/ptc_tool.hpp"

#include <algorithm>
#include <fstream>
#include <map>

#include "platform/process.hpp"
#include "ptc/stub_generator.hpp"

namespace lubancode::ptc {

namespace {

// 大结果落盘阈值:emit 摘要 dump 超过这数,全文落盘,历史只留引用+头尾。
constexpr std::size_t kSpillThresholdBytes = 8192;

// 把完整 emit 值与逐次调用账写进溢写文件(大结果层)。返回落盘路径;
// 失败返回空串(调用方按"没落盘"处理,不硬失败)。
std::string SpillRunArtifact(const PtcRunResult& run) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-ptc-spill";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return {};
    }
    nlohmann::json artifact = nlohmann::json::object();
    artifact["ptc_run_id"] = run.ptc_run_id;
    artifact["script_hash"] = run.script_hash;
    artifact["emit"] = run.emit_value;
    artifact["captured_stdout"] = run.captured_stdout;
    nlohmann::json calls = nlohmann::json::array();
    for (const auto& call : run.calls) {
        calls.push_back(nlohmann::json{{"id", call.id},
                                       {"tool", call.tool},
                                       {"input", call.input},
                                       {"input_hash", call.input_hash},
                                       {"result_hash", call.result_hash},
                                       {"ok", call.ok},
                                       {"is_error", call.is_error},
                                       {"reused", call.reused},
                                       {"error", call.error},
                                       {"elapsed_ms", call.elapsed_ms},
                                       {"side_effect_class", call.side_effect_class}});
    }
    artifact["calls"] = std::move(calls);
    const auto path = dir / (run.ptc_run_id + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {};
    }
    out << artifact.dump(2);
    if (!out.good()) {
        return {};
    }
    return path.string();
}

}  // namespace

std::vector<std::string> DefaultEligibleTools() {
    return {"read_file", "search"};
}

PythonProbe ProbePythonInterpreter(const std::string& configured) {
    PythonProbe probe;
    std::vector<std::string> candidates;
    if (!configured.empty()) {
        candidates.push_back(configured);
    } else {
#ifdef _WIN32
        candidates.push_back("python");
#else
        candidates.push_back("python3");
        candidates.push_back("python");
#endif
    }
    for (const auto& candidate : candidates) {
        // 探测一条小脚本:拿到版本串才算数(--version 在 Store 假 shim 上
        // 也会成功,真假拿 -c 的实际执行分辨)。
        const auto result = platform::RunProcess(
            {candidate, "-c", "import sys; sys.stdout.write(sys.version.split()[0])"}, 15000);
        if (result.spawn_failed || result.exit_code != 0) {
            continue;
        }
        std::string version = result.stdout_bytes.empty() ? result.output : result.stdout_bytes;
        if (version.rfind("3.", 0) != 0) {
            continue;  // 只要 Python 3
        }
        probe.ok = true;
        probe.command = candidate;
        probe.version = version;
        return probe;
    }
    probe.error = configured.empty() ? "本机找不到 Python 3 解释器(python/python3)"
                                     : ("配置的 Python 解释器不可用: " + configured);
    return probe;
}

PtcTool::PtcTool(tools::ToolRegistry& registry, std::function<bool(const tools::Tool&)> tool_filter, Config config)
    : registry_(registry), tool_filter_(std::move(tool_filter)), config_(std::move(config)) {
    const auto probe = ProbePythonInterpreter(config_.python_cmd);
    if (probe.ok) {
        config_.python_cmd = probe.command;
    } else {
        available_ = false;
        unavailability_reason_ = probe.error;
    }
}

std::string PtcTool::name() const { return "programmatic_tool_calling"; }

std::string PtcTool::description() const {
    return "编排一段 Python 脚本批量调用已挂载的只读工具(read_file/search 等):写变量、条件、循环、"
           "asyncio.gather 扇出,一段脚本收完把 emit() 的精简摘要送回。适合遍历一批文件、先查 A 再喂 "
           "B/C 的长链、同时派多路只读调用后聚合;短任务直接用普通工具更省。输入给 purpose(一句话"
           "目的,进审计账)与 script(Python 源码,import luban_tools 拿 typed stubs,结尾必须 emit)。";
}

nlohmann::json PtcTool::input_schema() const {
    return nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "purpose": {"type": "string", "minLength": 1},
            "script": {"type": "string", "minLength": 1}
        },
        "required": ["purpose", "script"]
    })");
}

void PtcTool::SetHooks(Hooks hooks) {
    std::lock_guard<std::mutex> lock(hooks_mutex_);
    hooks_ = std::move(hooks);
}

std::optional<PtcRunResult> PtcTool::last_run() const {
    return last_run_;
}

std::string PtcTool::GuideSegment() const {
    std::string out = "[programmatic tool calling]\n";
    out += "本会话可写 Python 脚本批量编排只读工具,调用 programmatic_tool_calling 工具\n";
    out += "(purpose + script)。脚本里 `from luban_tools import ...` 取 typed stubs:\n";
    // 当前已挂载的入选工具签名索引。
    std::vector<StubToolInfo> infos;
    const auto eligible = config_.eligible_tools.empty() ? DefaultEligibleTools() : config_.eligible_tools;
    for (const auto& tool : registry_.All()) {
        if (std::find(eligible.begin(), eligible.end(), tool->name()) == eligible.end()) {
            continue;
        }
        if (tool_filter_ && !tool_filter_(*tool)) {
            continue;  // tool_search:延迟未挂载的不进 stub 集
        }
        StubToolInfo info;
        info.definition.name = tool->name();
        info.definition.description = tool->description();
        info.definition.input_schema = tool->input_schema();
        info.needs_confirm = tool->needs_confirm();
        info.parallel_safe = true;  // 入选集全只读
        infos.push_back(std::move(info));
    }
    if (infos.empty()) {
        out += "(当前没有可编排的已挂载只读工具)\n";
        return out;
    }
    const auto module = GenerateStubModule(infos, StubMode::IndexOnly);
    out += module.signatures;
    out += "约定: 结果是 dict(\"content\"), 失败抛 ToolCallError(try/except 收口);\n";
    out += "结尾必须 emit(摘要); 可用标准库仅纯计算(json/math/re/itertools/...),\n";
    out += "禁网络/文件系统/子进程/环境变量。单次调用限额 " + std::to_string(config_.limits.max_calls) +
           " 次,墙钟 " + std::to_string(config_.limits.wall_clock_ms) + " ms。\n";
    return out;
}

tools::Tool::Result PtcTool::execute(const nlohmann::json& input) {
    if (!available_) {
        return Result{"PTC 不可用: " + unavailability_reason_, true};
    }
    const auto purpose = input.contains("purpose") && input.at("purpose").is_string()
                             ? input.at("purpose").get<std::string>()
                             : std::string();
    const auto script = input.contains("script") && input.at("script").is_string()
                            ? input.at("script").get<std::string>()
                            : std::string();
    if (script.empty()) {
        return Result{"script 不能为空", true};
    }
    if (script.size() > 128 * 1024) {
        return Result{"script 超过 128 KiB 上限", true};
    }

    // stub 集:入选白名单 ∩ 注册表 ∩ 当前挂载谓词。
    std::vector<StubToolInfo> infos;
    const auto eligible = config_.eligible_tools.empty() ? DefaultEligibleTools() : config_.eligible_tools;
    for (const auto& tool : registry_.All()) {
        if (std::find(eligible.begin(), eligible.end(), tool->name()) == eligible.end()) {
            continue;
        }
        if (tool_filter_ && !tool_filter_(*tool)) {
            continue;
        }
        StubToolInfo info;
        info.definition.name = tool->name();
        info.definition.description = tool->description();
        info.definition.input_schema = tool->input_schema();
        info.needs_confirm = tool->needs_confirm();
        info.parallel_safe = true;
        infos.push_back(std::move(info));
    }
    const auto stub = GenerateStubModule(infos, StubMode::Full);

    // 每枚 stub 调用的完整链:agent::RunOneTool(schema 复检/PreToolUse/
    // PermissionRequest/执行/PostToolUse/审计),与 JSON 后端同一条代码路。
    Hooks hooks;
    {
        std::lock_guard<std::mutex> lock(hooks_mutex_);
        hooks = hooks_;
    }
    agent::Callbacks chain;
    chain.on_tool_start = hooks.on_tool_start;
    chain.on_tool_confirm = hooks.on_tool_confirm;
    chain.on_tool_done = hooks.on_tool_done;
    chain.on_pre_tool_use_hook = hooks.on_pre_tool_use_hook;
    chain.on_permission_request = hooks.on_permission_request;
    chain.on_tool_phase = hooks.on_tool_phase;
    chain.on_post_tool_use_hook = hooks.on_post_tool_use_hook;

    PtcRunner::Options options;
    options.python_cmd = config_.python_cmd;
    options.limits = config_.limits;
    options.cancel = hooks.cancel;
    options.restricted_token = config_.restricted_token;
    options.executor = [this, &chain](const std::string& tool_name, const nlohmann::json& tool_input) {
        api::ToolUseBlock call;
        call.id = "ptc-" + std::to_string(++call_seq_);
        call.name = tool_name;
        call.input = tool_input;
        return agent::RunOneTool(registry_, call, chain, tool_filter_);
    };

    const auto result = PtcRunner::Run(script, stub.python_source, std::move(options));
    last_run_ = result;

    // ---- 结果排版:语义层摘要在头,审计层账目在后(Ctrl+O 展开) ----
    std::string content;
    content += "● programmatic_tool_calling(" + purpose + ")\n";
    // 按工具聚合的账:次数 / 并发峰值 / 耗时。
    std::map<std::string, std::pair<int, int>> per_tool;  // name -> {次数, 错误数}
    int executed = 0;
    int failed = 0;
    for (const auto& call : result.calls) {
        auto& entry = per_tool[call.tool];
        entry.first += 1;
        if (!call.ok || call.is_error) {
            entry.second += 1;
            ++failed;
        } else {
            ++executed;
        }
    }
    for (const auto& [tool_name, counts] : per_tool) {
        content += "  ├─ " + std::to_string(counts.first) + " 次 " + tool_name;
        if (counts.second > 0) {
            content += "  (失败 " + std::to_string(counts.second) + ")";
        }
        content += "\n";
    }
    content += "  └─ " + std::string(result.ok ? "完成" : PtcRunner::FailureText(result.failure)) + "  " +
               std::to_string(executed) + "/" + std::to_string(result.calls.size()) + "  " +
               std::to_string(result.elapsed_ms) + "ms\n";
    content += "  运行 id: " + result.ptc_run_id + "  python " + result.python_version + "\n";
    if (result.ok) {
        std::string summary = result.emit_value.dump();
        // 大结果层(规格"历史与压缩"节):全文落盘,历史只留引用+预览。
        if (summary.size() > kSpillThresholdBytes) {
            const std::string spill_path = SpillRunArtifact(result);
            if (!spill_path.empty()) {
                content += "\n[摘要](大结果落盘 " + spill_path + ", " + std::to_string(summary.size()) +
                           " 字节)\n" + summary.substr(0, 512) + "\n…\n" +
                           summary.substr(summary.size() - 256) + "\n";
            } else {
                content += "\n[摘要](落盘失败,截断展示)\n" + summary.substr(0, kSpillThresholdBytes) + "…\n";
            }
        } else {
            content += "\n[摘要]\n" + summary + "\n";
        }
    } else {
        content += "\n[失败] " + PtcRunner::FailureText(result.failure) + ": " + result.error + "\n";
        if (!result.traceback.empty()) {
            content += result.traceback + "\n";
        }
    }
    // 审计层:逐次调用的账(工具/入参 hash/结果 hash/耗时/错误/复用标记)。
    if (!result.calls.empty()) {
        content += "\n[调用账 " + result.ptc_run_id + "]\n";
        for (const auto& call : result.calls) {
            content += "  #" + std::to_string(call.id) + " " + call.tool + " in=" + call.input_hash.substr(0, 8) +
                       " out=" + (call.result_hash.empty() ? std::string("-") : call.result_hash.substr(0, 8)) +
                       " " + std::to_string(call.elapsed_ms) + "ms";
            if (call.reused) {
                content += "  ↻ 同参复用";
            }
            if (!call.ok || call.is_error) {
                content += "  ✗ " + call.error;
            }
            content += "\n";
        }
    }
    if (!result.captured_stdout.empty()) {
        content += "\n[print 捕获]\n" + result.captured_stdout + "\n";
    }
    return Result{content, !result.ok};
}

}  // namespace lubancode::ptc
