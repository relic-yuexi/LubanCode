// Workspace 收官验收·量级基线驱动(单子 §一第 5 条;不进 ctest,验收手动跑):
//   workspace_scale_driver <输出.json> [--sessions 1,1000,10000]
//                           [--memories 1,1000,10000] [--root <临时根>]
//
// 量四件事(1/1k/10k 三档,冷/热分账,峰值内存记表):
//   1. session list:/sessions 与 resume 选择器的数据源
//      QueryWorkspaceSessions——冷 = 索引重建后首查,热 = 索引命中的二查;
//   2. memory recall:BuildTurnContext(生产召回路)——冷 = 新实例首召
//      (catalog/index 全量装载),热 = 同实例二召;
//   3. replay:FoldStreamReplay(验链 + 折叠,resume 七步的核心开销)与
//      VerifySessionDir(整场验账);
//   4. 生成侧:造数耗时与磁盘占用(夹具成本,供复现参考)。
//
// 造数口径(不碰真主目录,全部落在 --root 或临时目录):
//   - session:先经真 TrajectorySessionLedger 写一场模板会话(6 轮、含
//     工具往返/usage/title/compact 事件,约 100+ 事件),再按档克隆成 N
//     场(目录名换成合规 session id;main.jsonl 逐字节相同——索引与折叠
//     的解析成本形状与真账一致,事件内 session_id 不重铸,如实记入报告)。
//   - memory:按生产 frontmatter 形状(schema 3)直写 N 份主题,再走
//     RebuildMemoryIndex(/memory rebuild 同一条路)建派生账。
//
// 峰值内存:Windows 走 GetProcessMemoryInfo(PeakWorkingSet),POSIX 读
// /proc/self/status 的 VmHWM。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/identity.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
// PSAPI_VERSION=2 时 K32GetProcessMemoryInfo 在 kernel32,无需另链 psapi。
#else
#include <sys/resource.h>
#endif

using namespace lubancode;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double MillisSince(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

std::uint64_t PeakRssKb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize / 1024;
    }
    return 0;
#else
    if (std::ifstream status("/proc/self/status"); status.is_open()) {
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmHWM:", 0) == 0) {
                std::istringstream in(line.substr(6));
                std::uint64_t kb = 0;
                in >> kb;
                return kb;
            }
        }
    }
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
}

std::string ReadFileText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteFileText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::vector<int> ParseCounts(const std::string& raw) {
    std::vector<int> out;
    std::istringstream in(raw);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (!item.empty()) {
            out.push_back(std::stoi(item));
        }
    }
    return out;
}

api::Message TextMessage(api::Role role, const std::string& text) {
    api::Message message;
    message.role = role;
    message.content.push_back(api::TextBlock{text});
    return message;
}

// 手工驱动没有 doctest:失败即打点退出,退出码非零。
#define SCALE_CHECK(x)                                                            \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "断言失败: %s (%s:%d)\n", #x, __FILE__, __LINE__); \
            std::exit(9);                                                         \
        }                                                                         \
    } while (false)

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id,
                                 int index) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "exec-" + std::to_string(index);
    event.tool_use_id = call_id;
    event.tool_name = "read_file";
    event.timestamp_ms = 1759000000000LL + index;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::ReadOnlyLocal;
        event.effective_arguments = nlohmann::json{{"path", "src/module_" + std::to_string(index) +
                                                                 ".cpp"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 11;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, std::to_string(index % 10)[0]);
        event.result_ref.bytes = 220;
    }
    return event;
}

// 模板会话:6 轮,每轮输入→(请求→输出带工具→工具三拍→回喂→请求→输出)
// 再补一条 title;形状与交互会话同构。
void DriveTemplateTurns(runtime::TrajectorySessionLedger& ledger, int turns) {
    for (int turn = 1; turn <= turns; ++turn) {
        const std::string turn_id = "turn-" + std::to_string(turn);
        auto bridge = ledger.NewTurnBridge({"demo", "responses", "terminal"});
        bridge->BeginTurn(turn_id, "external_user");
        api::Message user = TextMessage(api::Role::User,
                                        "第 " + std::to_string(turn) +
                                            " 轮:读一下 module 的实现并总结要点");
        bridge->RecordInput(user);
        api::Request request;
        request.model = "demo-model";
        const std::string req1 = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
        bridge->OnRequestSent(req1);
        api::Usage usage;
        usage.input_tokens = 2000 + turn;
        usage.output_tokens = 300 + turn;
        bridge->OnUsageRecorded(req1, usage, true, "resp-" + std::to_string(turn));
        api::Message with_call;
        with_call.role = api::Role::Assistant;
        with_call.content.push_back(api::TextBlock{"我来读源码。"});
        api::ToolUseBlock call;
        call.id = "call-" + std::to_string(turn);
        call.name = "read_file";
        call.input = nlohmann::json{{"path", "src/module_" + std::to_string(turn) + ".cpp"}};
        with_call.content.push_back(std::move(call));
        SCALE_CHECK(bridge->OnOutputCompleted(req1, with_call, "tool_use", "resp-a"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call.id, turn));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call.id, turn));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call.id, turn));
        api::Message results;
        results.role = api::Role::User;
        results.content.push_back(
                api::ToolResultBlock{call.id, "模块实现要点:入口在 Run(),分三段处理,尾段收账。", false});
        bridge->OnToolResultsCommitted("batch-" + std::to_string(turn), results);
        const std::string req2 = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
        bridge->OnRequestSent(req2);
        api::Message answer = TextMessage(
                api::Role::Assistant,
                "总结:module_" + std::to_string(turn) +
                    " 的入口是 Run(),分解析、执行、收账三段;错误走统一回执,不吞异常。");
        SCALE_CHECK(bridge->OnOutputCompleted(req2, answer, "end_turn", "resp-b"));
        bridge->EndTurn(true, false, {});
    }
    ledger.RecordTitleChanged("模块通读:要点与结构", "");
}

// 造 N 场会话:克隆模板(目录名 = 合法 session id;session.json 里
// session_id 同步改写,main.jsonl 逐字节不动)。
std::string StampSessionId(int index) {
    // 2026-01-01 00:00:00 起,每场 +37 秒,散开排序;尾 6 位防撞。
    const std::int64_t base = 1767225600LL + static_cast<std::int64_t>(index) * 37;
    std::time_t seconds = static_cast<std::time_t>(base);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d-S%05d",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                  parts.tm_min, parts.tm_sec, index % 100000);
    return stamp;
}

// 造 N 份记忆主题(schema 3 生产形状)。
void WriteMemoryTopic(const fs::path& facts_dir, int index) {
    const std::string id = "fact.scale-" + std::to_string(index);
    nlohmann::json frontmatter;
    frontmatter["name"] = "scale-" + std::to_string(index);
    frontmatter["description"] = "量级夹具主题 " + std::to_string(index);
    nlohmann::json metadata;
    metadata["schema"] = 3;
    metadata["node_type"] = "memory";
    metadata["type"] = "fact";
    metadata["id"] = id;
    metadata["confidence"] = "verified";
    metadata["status"] = "active";
    metadata["scope"] = nlohmann::json{{"level", "project"}, {"kind", "project"}, {"value", ""}};
    metadata["origin_session_ids"] = nlohmann::json::array();
    metadata["created"] = "2026-01-01T00:00:00Z";
    metadata["modified"] = "2026-01-01T00:00:00Z";
    metadata["last_verified"] = "2026-01-01T00:00:00Z";
    metadata["expires"] = nullptr;
    // 关键词分十个主题族,让召回真做选择,不是全量命中。
    const std::string family = std::to_string(index % 10);
    metadata["keywords"] = nlohmann::json::array({"module" + family, "scale" + family, "baseline"});
    metadata["evidence"] = nlohmann::json::array({nlohmann::json{{"path", "src/module" + family +
                                                                                    ".cpp"},
                                                                 {"symbol", "Run"}}});
    frontmatter["metadata"] = metadata;
    // 不带 fingerprints 段:夹具不指认真实源文件,免得指纹漂移把召回全
    // 拦成 stale(量的是检索与装配成本,不是陈旧判定)。
    const std::string body =
            "模块 module" + family + " 的构建与调用要点:\n\n"
            "- 入口 Run(),分解析、执行、收账三段;第 " +
            std::to_string(index) +
            " 号夹具主题。\n"
            "- 错误统一回执,不吞异常。\n"
            "- 量级夹具:正文长度与生产主题同量级,不掺真实项目内容。\n";
    WriteFileText(facts_dir / ("scale-" + std::to_string(index) + ".md"),
                  "---\n" + frontmatter.dump(2) + "\n---\n\n" + body);
}

std::uintmax_t TreeBytes(const fs::path& root) {
    std::uintmax_t bytes = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return 0;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
            bytes += entry.file_size(ec);
        }
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "用法: workspace_scale_driver <输出.json> [--sessions 1,1000,10000] "
                     "[--memories 1,1000,10000] [--root <临时根>]\n");
        return 1;
    }
    const fs::path output = fs::absolute(argv[1]);
    std::vector<int> session_counts{1, 1000, 10000};
    std::vector<int> memory_counts{1, 1000, 10000};
    fs::path root = fs::temp_directory_path() / "lubancode-ws-scale";
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--sessions" && i + 1 < argc) {
            session_counts = ParseCounts(argv[++i]);
        } else if (flag == "--memories" && i + 1 < argc) {
            memory_counts = ParseCounts(argv[++i]);
        } else if (flag == "--root" && i + 1 < argc) {
            root = fs::path(argv[++i]);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    nlohmann::json report;
    report["driver"] = "workspace_scale_driver";
    report["sessions_scale"] = session_counts;
    report["memories_scale"] = memory_counts;
    report["temp_root"] = root.generic_string();
#ifdef _WIN32
    report["platform"] = "windows";
#else
    report["platform"] = "posix";
#endif

    // ---- 模板会话(真 ledger 写出)----
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo / ".git", ec);
    fs::create_directories(home, ec);
    std::optional<runtime::TrajectorySessionLedger> ledger;
    {
        auto opened = runtime::TrajectorySessionLedger::Open([&] {
            runtime::TrajectorySessionLedger::Options options;
            options.workspaces_root = home / "workspaces";
            options.workspace_root = repo;
            options.launch_cwd = repo.generic_string();
            options.lubancode_version = "scale-driver";
            return options;
        }());
        if (!opened.has_value()) {
            std::fprintf(stderr, "模板会话开账失败: %s\n", opened.error().c_str());
            return 2;
        }
        ledger.emplace(std::move(*opened));
    }
    DriveTemplateTurns(*ledger, 6);
    const std::string template_session_id = ledger->session_id();
    const fs::path template_dir = ledger->session_dir();
    const std::string template_manifest = ReadFileText(template_dir / "session.json");
    const std::string template_main = ReadFileText(template_dir / "main.jsonl");
    const std::size_t template_events =
            std::count(template_main.begin(), template_main.end(), '\n');
    // 封口模板场(克隆的是终态形状)。
    // 账本制:房门从模板场目录上折(sessions 的上两层),不拿 key 拼目录。
    const fs::path workspace_dir = template_dir.parent_path().parent_path();
    const auto identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();
    ledger->CloseSession("exit");
    ledger.reset();
    report["template"] = nlohmann::json{
        {"session_id", template_session_id},
        {"main_events", template_events},
        {"main_bytes", template_main.size()},
    };

    // ---- session list 基线(每档:克隆→冷查→热查)----
    nlohmann::json session_rows = nlohmann::json::array();
    for (int count : session_counts) {
        // 从上一档累积(1→1000→10000 逐档加,不重来)。
        int existing = 0;
        {
            const fs::path sessions = workspace_dir / "sessions";
            if (fs::exists(sessions, ec)) {
                for (const auto& entry : fs::directory_iterator(sessions, ec)) {
                    std::error_code iter_ec;
                    if (entry.is_directory(iter_ec)) {
                        ++existing;
                    }
                }
            }
        }
        for (int index = existing; index < count; ++index) {
            const std::string id = StampSessionId(index);
            const fs::path target = workspace_dir / "sessions" / std::filesystem::path(id);
            fs::create_directories(target, ec);
            std::string manifest = template_manifest;
            const std::size_t pos = manifest.find(template_session_id);
            if (pos != std::string::npos) {
                manifest.replace(pos, template_session_id.size(), id);
            }
            WriteFileText(target / "session.json", manifest);
            WriteFileText(target / "main.jsonl", template_main);
        }
        const auto cold_begin = Clock::now();
        trajectory::SessionIndexQuery query;
        query.current_workspace_key = identity.workspace_key;
        const auto cold_page = trajectory::QueryWorkspaceSessions(home / "workspaces", query);
        const double list_cold_ms = MillisSince(cold_begin);
        const auto hot_begin = Clock::now();
        const auto hot_page = trajectory::QueryWorkspaceSessions(home / "workspaces", query);
        const double list_hot_ms = MillisSince(hot_begin);
        if (cold_page.total != static_cast<std::size_t>(count) ||
            hot_page.total != static_cast<std::size_t>(count)) {
            std::fprintf(stderr, "list 计数不合: 期望 %d, 实得 %zu / %zu\n", count,
                         cold_page.total, hot_page.total);
            return 3;
        }
        session_rows.push_back(nlohmann::json{
            {"sessions", count},
            {"list_cold_ms", list_cold_ms},
            {"list_hot_ms", list_hot_ms},
        });
        std::printf("[sessions %6d] list 冷 %10.1f ms / 热 %10.1f ms\n", count, list_cold_ms,
                    list_hot_ms);
    }
    report["session_rows"] = session_rows;

    // ---- replay/verify 基线(单场模板量级;场次规模对 replay 无关——
    // replay 是单场折叠,成本跟事件数走,这里钉模板场的每场成本)----
    {
        const auto fold_begin = Clock::now();
        const auto fold = trajectory::FoldStreamReplay(
                workspace_dir / "sessions" / std::filesystem::path(template_session_id) /
                "main.jsonl");
        const double fold_ms = MillisSince(fold_begin);
        if (!fold.ok()) {
            std::fprintf(stderr, "模板场折叠失败: %s\n", fold.message.c_str());
            return 4;
        }
        const auto verify_begin = Clock::now();
        const auto verify =
                trajectory::VerifySessionDir(workspace_dir / "sessions" /
                                             std::filesystem::path(template_session_id));
        const double verify_ms = MillisSince(verify_begin);
        if (!verify.ok) {
            std::fprintf(stderr, "模板场验账失败: %s\n", verify.message.c_str());
            return 5;
        }
        report["replay"] = nlohmann::json{
            {"events", template_events},
            {"fold_ms", fold_ms},
            {"verify_ms", verify_ms},
        };
        std::printf("[replay %6zu events] fold %8.1f ms / verify %8.1f ms\n", template_events,
                    fold_ms, verify_ms);
    }

    // ---- memory 基线(每档:写主题→rebuild→冷召→热召)----
    auto make_store = [&] {
        memory::Options options;
        options.global_allowed = true;
        options.enabled = true;
        auto resolved = memory::ResolveProjectIdentity(repo, home);
        if (!resolved.has_value()) {
            std::fprintf(stderr, "记忆身份解析失败: %s\n", resolved.error().c_str());
            std::exit(6);
        }
        return std::make_shared<memory::ProjectMemory>(std::move(*resolved), home, options);
    };
    nlohmann::json memory_rows = nlohmann::json::array();
    for (int count : memory_counts) {
        const fs::path facts = workspace_dir / "memory" / "facts";
        // 逐档累积。
        int existing = 0;
        {
            if (fs::exists(facts, ec)) {
                for (const auto& entry : fs::directory_iterator(facts, ec)) {
                    std::error_code iter_ec;
                    if (entry.is_regular_file(iter_ec)) {
                        ++existing;
                    }
                }
            }
        }
        const auto write_begin = Clock::now();
        for (int index = existing; index < count; ++index) {
            WriteMemoryTopic(facts, index);
        }
        const double write_ms = MillisSince(write_begin);

        // rebuild(/memory rebuild 同一条路)。
        auto store = make_store();
        const auto rebuild_begin = Clock::now();
        if (!memory::RebuildMemoryIndex(store->memory_dir()).has_value()) {
            std::fprintf(stderr, "记忆索引重建失败(档 %d)\n", count);
            return 6;
        }
        const double rebuild_ms = MillisSince(rebuild_begin);

        // 冷召:新实例(目录册/词袋全量装载)。查询词钉 module0 族——
        // 三档里 0 号主题都在,命中数可跨档对比。
        auto cold_store = make_store();
        const auto cold_begin = Clock::now();
        const std::string cold_context = cold_store->BuildTurnContext(
                "module0 的入口在哪,怎么调用", repo);
        const double recall_cold_ms = MillisSince(cold_begin);
        // 热召:同实例二召。
        const auto hot_begin = Clock::now();
        const std::string hot_context =
                cold_store->BuildTurnContext("module0 的入口在哪,怎么调用", repo);
        const double recall_hot_ms = MillisSince(hot_begin);
        if (cold_context.empty() || hot_context.empty()) {
            std::fprintf(stderr, "召回空(档 %d)——夹具或查询对不上,先修夹具\n", count);
            return 7;
        }
        memory_rows.push_back(nlohmann::json{
            {"memories", count},
            {"topics_write_ms", write_ms},
            {"rebuild_ms", rebuild_ms},
            {"recall_cold_ms", recall_cold_ms},
            {"recall_hot_ms", recall_hot_ms},
            {"recall_bytes", hot_context.size()},
        });
        std::printf("[memory  %6d] write %10.1f ms / rebuild %10.1f ms / 召回 冷 %8.1f ms / "
                    "热 %8.1f ms\n",
                    count, write_ms, rebuild_ms, recall_cold_ms, recall_hot_ms);
    }
    report["memory_rows"] = memory_rows;

    report["peak_rss_kb"] = PeakRssKb();
    report["workspace_bytes"] = TreeBytes(workspace_dir);

    WriteFileText(output, report.dump(2) + "\n");
    std::printf("峰值内存 %llu KB,workspace 树 %llu bytes\n",
                static_cast<unsigned long long>(report["peak_rss_kb"].get<std::uint64_t>()),
                static_cast<unsigned long long>(report["workspace_bytes"].get<std::uintmax_t>()));
    std::printf("报告落 %s\n", output.generic_string().c_str());
    return 0;
}
