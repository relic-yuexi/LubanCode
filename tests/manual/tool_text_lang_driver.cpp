// 工具文案语言档的端到端驱动(不进 ctest,验收/集成验证手动跑):
//   LUBANCODE_LANG=en ./tool_text_lang_driver
// 语言按真实启动链定:LUBANCODE_LANG(空 = 系统探测),与 cli_app.cpp
// 早初始化同一条路。定稿后把已迁移工具的 description、schema 参数说明与
// 子代理 persona 打到 stdout,外加 == 节名 == 定界标记,供逐字节比对:
//   - 缺省(中文机器)输出必须与迁移前逐字节一致;
//   - LUBANCODE_LANG=en 输出必须是英文档原文。
//
// 覆盖:基建批试点(read_file/write_file)、批1 文件工具余量(edit_file/
// search)、批2 命令族(run_command/background_output/stop_background)、
// 批3 代理族(agent/agent_message/persona)、批4 交互(ask_user/todo_write)、
// 批5 外接(lsp/context_search/context_read)、清底批(web_search/web_fetch/
// tool_search/skill、list_sessions/send_session_message/worktree、
// memory_save/programmatic_tool_calling)。


#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"
#include "cli/worktree.hpp"
#include "config/config.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "platform/paths.hpp"  // GetEnvVar:LUBANCODE_LANG,与 cli_app 早初始化同源
#include "ptc/ptc_tool.hpp"
#include "tools/agent_message_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/context_tools.hpp"
#include "tools/edit_file.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/tool_text.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/worktree_tool.hpp"
#include "tools/write_file.hpp"

namespace {

// 只为构造工具,不发请求。
struct NullBackend : lubancode::api::Backend {
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        return {};
    }
};

void DumpParam(const nlohmann::json& schema, const char* prop, const char* tag) {
    std::cout << "== " << tag << " ==\n"
              << schema["properties"][prop]["description"].get<std::string>() << "\n";
}

}  // namespace

int main() {
    std::string lang;
    if (const auto env = lubancode::platform::GetEnvVar("LUBANCODE_LANG"); env.has_value() && !env->empty()) {
        lang = *env;
    } else {
        lang = lubancode::cli::DetectSystemLanguage();
    }
    lubancode::cli::SetLanguage(lang);
    std::cerr << "[language] " << lang << "\n";

    // ---- 基建批试点:read_file / write_file ----
    lubancode::tools::ReadFileTool read;
    lubancode::tools::WriteFileTool write;

    std::cout << "== read_file.description ==\n" << read.description() << "\n";
    const nlohmann::json rs = read.input_schema();
    DumpParam(rs, "path", "read_file.param.path");
    DumpParam(rs, "offset", "read_file.param.offset");
    DumpParam(rs, "limit", "read_file.param.limit");
    std::cout << "== write_file.description ==\n" << write.description() << "\n";
    const nlohmann::json ws = write.input_schema();
    DumpParam(ws, "path", "write_file.param.path");
    DumpParam(ws, "content", "write_file.param.content");

    // ---- 批3:代理族(agent / agent_message / persona)----
    NullBackend backend;
    lubancode::tools::ToolRegistry registry;
    lubancode::tools::AgentTool agent(backend, registry, "/work/dir");
    std::cout << "== agent.description ==\n" << agent.description() << "\n";
    const nlohmann::json as = agent.input_schema();
    DumpParam(as, "title", "agent.param.title");
    DumpParam(as, "prompt", "agent.param.prompt");
    // max_steps_per_turn 不再出 schema(限步走配置,不给模型旋钮),故不转录。
    DumpParam(as, "agent_type", "agent.param.agent_type");
    DumpParam(as, "execution_mode", "agent.param.execution_mode");
    DumpParam(as, "run_in_background", "agent.param.run_in_background");
    DumpParam(as, "isolation", "agent.param.isolation");
    std::cout << "== agent.persona.general ==\n"
              << lubancode::tools::ToolText("agent", "persona.general",
                                            "你是 general-purpose 子代理,能搜索、分析并完成多步任务。专注给定任务,完成后直接给出结论,不要寒暄。")
              << "\n";
    std::cout << "== agent.persona.explore ==\n"
              << lubancode::tools::ToolText(
                     "agent", "persona.explore",
                     "你是 Explore 子代理,专门快速搜索、阅读并分析代码库。只读,不得改文件、启动会改动环境的命令或做别的写操作。完成后给出简明结论和具体文件位置,不要寒暄。")
              << "\n";

    lubancode::tools::AgentMessageTool message(nullptr);
    std::cout << "== agent_message.description ==\n" << message.description() << "\n";
    const nlohmann::json ms = message.input_schema();
    DumpParam(ms, "task_id", "agent_message.param.task_id");
    DumpParam(ms, "message", "agent_message.param.message");

    // ---- 批4:交互(ask_user / todo_write)----
    lubancode::tools::AskUserTool ask(
        [](const lubancode::tools::AskUserQuestion&) {
            return std::expected<std::vector<std::string>, std::string>{std::vector<std::string>{"a"}};
        });
    std::cout << "== ask_user.description ==\n" << ask.description() << "\n";
    const nlohmann::json ks = ask.input_schema();
    const nlohmann::json& kq = ks["properties"]["questions"]["items"]["properties"];
    std::cout << "== ask_user.param.questions.header ==\n"
              << kq["header"]["description"].get<std::string>() << "\n";
    std::cout << "== ask_user.param.questions.question ==\n"
              << kq["question"]["description"].get<std::string>() << "\n";
    std::cout << "== ask_user.param.questions.multi_select ==\n"
              << kq["multi_select"]["description"].get<std::string>() << "\n";

    lubancode::tools::TodoWriteTool todo(std::make_shared<lubancode::tools::TodoListState>());
    std::cout << "== todo_write.description ==\n" << todo.description() << "\n";
    const nlohmann::json ts = todo.input_schema();
    DumpParam(ts, "items", "todo_write.param.items");
    const nlohmann::json& ti = ts["properties"]["items"]["items"]["properties"];
    std::cout << "== todo_write.param.items.content ==\n"
              << ti["content"]["description"].get<std::string>() << "\n";
    std::cout << "== todo_write.param.items.status ==\n"
              << ti["status"]["description"].get<std::string>() << "\n";

    // ---- 批5:外接(lsp / context_search / context_read)----
    lubancode::lsp::Manager manager({}, "/work/dir");
    lubancode::tools::LspTool lsp(manager);
    std::cout << "== lsp.description ==\n" << lsp.description() << "\n";
    const nlohmann::json ls = lsp.input_schema();
    DumpParam(ls, "mode", "lsp.param.mode");
    DumpParam(ls, "file", "lsp.param.file");
    DumpParam(ls, "line", "lsp.param.line");
    DumpParam(ls, "character", "lsp.param.character");

    lubancode::tools::ContextSearchTool search(nullptr);
    std::cout << "== context_search.description ==\n" << search.description() << "\n";
    const nlohmann::json ss = search.input_schema();
    DumpParam(ss, "artifact_id", "context_search.param.artifact_id");
    DumpParam(ss, "query", "context_search.param.query");
    DumpParam(ss, "max_results", "context_search.param.max_results");

    lubancode::tools::ContextReadTool context_read(nullptr);
    std::cout << "== context_read.description ==\n" << context_read.description() << "\n";
    const nlohmann::json cs = context_read.input_schema();
    DumpParam(cs, "artifact_id", "context_read.param.artifact_id");
    DumpParam(cs, "chunk_id", "context_read.param.chunk_id");
    DumpParam(cs, "line_start", "context_read.param.line_start");
    DumpParam(cs, "line_count", "context_read.param.line_count");

    // ---- 批1 文件工具余量(edit_file / search)与批2 命令族 ----
    // (合并自批1-2 驱动器:变量名避开上文批5 的 search/ss。)
    // run_command 的 schema 随平台分档,输出自然带平台视角——比对时对
    // 同一平台取基准。
    lubancode::tools::EditFileTool edit;
    std::cout << "== edit_file.description ==\n" << edit.description() << "\n";
    const nlohmann::json es = edit.input_schema();
    for (const auto& item : es["properties"].items()) {
        std::cout << "== edit_file.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::SearchTool search_tool;
    std::cout << "== search.description ==\n" << search_tool.description() << "\n";
    const nlohmann::json sjs = search_tool.input_schema();
    for (const auto& item : sjs["properties"].items()) {
        std::cout << "== search.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::RunCommandTool run;
    std::cout << "== run_command.description ==\n" << run.description() << "\n";
    const nlohmann::json rcs = run.input_schema();
    for (const auto& item : rcs["properties"].items()) {
        std::cout << "== run_command.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::BackgroundOutputTool bg;
    std::cout << "== background_output.description ==\n" << bg.description() << "\n";
    const nlohmann::json bos = bg.input_schema();
    for (const auto& item : bos["properties"].items()) {
        std::cout << "== background_output.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::StopBackgroundTool stop;
    std::cout << "== stop_background.description ==\n" << stop.description() << "\n";
    const nlohmann::json sbs = stop.input_schema();
    for (const auto& item : sbs["properties"].items()) {
        std::cout << "== stop_background.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // ---- 清底批:网络与外挂检索(web_search/web_fetch/tool_search/skill)----
    lubancode::config::SearchConfig search_config;
    search_config.provider = "tavily";
    search_config.api_key = "driver";
    lubancode::tools::WebSearchTool web_search(search_config);
    std::cout << "== web_search.description ==\n" << web_search.description() << "\n";
    const nlohmann::json wss = web_search.input_schema();
    for (const auto& item : wss["properties"].items()) {
        std::cout << "== web_search.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::WebFetchTool web_fetch;
    std::cout << "== web_fetch.description ==\n" << web_fetch.description() << "\n";
    const nlohmann::json wfs = web_fetch.input_schema();
    for (const auto& item : wfs["properties"].items()) {
        std::cout << "== web_fetch.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // tool_search / skill / 会话族 / worktree 共用一张注册表也行,但各工具
    // 构造只要各自的轻依赖,分开构造即可。
    lubancode::tools::ToolRegistry clearance_registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    lubancode::tools::ToolSearchTool tool_search(clearance_registry, loaded);
    std::cout << "== tool_search.description ==\n" << tool_search.description() << "\n";
    const nlohmann::json tss = tool_search.input_schema();
    for (const auto& item : tss["properties"].items()) {
        std::cout << "== tool_search.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    lubancode::tools::SkillTool skill(std::vector<lubancode::tools::SkillMeta>{});
    std::cout << "== skill.description ==\n" << skill.description() << "\n";
    const nlohmann::json sks = skill.input_schema();
    for (const auto& item : sks["properties"].items()) {
        std::cout << "== skill.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // ---- 清底批:跨会话族(list_sessions/send_session_message)----
    lubancode::tools::ListSessionsTool list_sessions(
        [] { return std::vector<lubancode::agent::PeerCard>{}; }, "self");
    std::cout << "== list_sessions.description ==\n" << list_sessions.description() << "\n";

    lubancode::tools::SendSessionMessageTool send_session(
        [] { return std::vector<lubancode::agent::PeerCard>{}; },
        [](const lubancode::agent::PeerCard&, const std::string&) {
            return lubancode::agent::PeerDelivery::Delivered;
        });
    std::cout << "== send_session_message.description ==\n" << send_session.description() << "\n";
    const nlohmann::json sms = send_session.input_schema();
    for (const auto& item : sms["properties"].items()) {
        std::cout << "== send_session_message.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // ---- 清底批:worktree(查文案,不起 git)----
    lubancode::cli::WorktreeSession worktree_session;
    lubancode::tools::WorktreeTool worktree(worktree_session, nullptr, nullptr);
    std::cout << "== worktree.description ==\n" << worktree.description() << "\n";
    const nlohmann::json wts = worktree.input_schema();
    for (const auto& item : wts["properties"].items()) {
        std::cout << "== worktree.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // ---- 清底批:memory_save(空配置,只查文案)----
    lubancode::memory::Options memory_options;
    lubancode::memory::ProjectIdentity memory_identity;
    auto memory_store = std::make_shared<lubancode::memory::ProjectMemory>(
        memory_identity, std::filesystem::path("/work/home"), memory_options);
    lubancode::memory::MemorySaveTool memory_save(memory_store);
    std::cout << "== memory_save.description ==\n" << memory_save.description() << "\n";
    const nlohmann::json mss = memory_save.input_schema();
    for (const auto& item : mss["properties"].items()) {
        std::cout << "== memory_save.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }
    // 嵌套一层:scope.kind / scope.value / evidence[].path / evidence[].symbol。
    const nlohmann::json& scope_props = mss["properties"]["scope"]["properties"];
    for (const auto& item : scope_props.items()) {
        std::cout << "== memory_save.param.scope." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }
    const nlohmann::json& evidence_props = mss["properties"]["evidence"]["items"]["properties"];
    for (const auto& item : evidence_props.items()) {
        std::cout << "== memory_save.param.evidence." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }

    // ---- 清底批:programmatic_tool_calling(空注册表,只查文案)----
    // PtcTool 的 purpose/script 两参数只有 minLength,没有 description 字段
    // ——没有就不打印,别硬取(const operator[] 会断言)。
    lubancode::ptc::PtcTool ptc(clearance_registry, nullptr, lubancode::ptc::PtcTool::Config{});
    std::cout << "== programmatic_tool_calling.description ==\n" << ptc.description() << "\n";
    const nlohmann::json pts = ptc.input_schema();
    for (const auto& item : pts["properties"].items()) {
        if (!item.value().contains("description")) {
            continue;
        }
        std::cout << "== programmatic_tool_calling.param." << item.key() << " ==\n"
                  << item.value()["description"].get<std::string>() << "\n";
    }
    return 0;
}
