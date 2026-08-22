// 工具文案按语言分档(模型可见文字抽离 C++ 的基建批):
//   ToolText 三级回退:当前语言档 → zh-CN 档 → 兜底字串;
//   缺省(zh-CN)下试点工具的 description / schema 参数说明与迁移前
//   逐字节一致(零行为变化);
//   LUBANCODE_LANG=en 走 cli::SetLanguage("en") 同一条选择链,出英文。
//
// 批3(代理族:agent/agent_message + persona)、批4(交互:ask_user/todo_write)、
// 批5(外接:lsp/context_search/context_read)同样两条铁律:缺省零变化、
// en 生效。各批的迁移前原文照 cpp 字面量逐字节抄在这里钉死。
//
// 注意:语言是进程级全局状态,LangGuard 兜底还原(同 test_i18n.cpp 的
// 规矩),免得串到别的测试文件头上。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"
#include "lsp/manager.hpp"
#include "tools/agent_message_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/context_tools.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/tool_text.hpp"
#include "tools/todo_tool.hpp"
#include "tools/write_file.hpp"

using lubancode::tools::AgentMessageTool;
using lubancode::tools::AgentTool;
using lubancode::tools::AskUserTool;
using lubancode::tools::ContextReadTool;
using lubancode::tools::ContextSearchTool;
using lubancode::tools::ReadFileTool;
using lubancode::tools::Tool;
using lubancode::tools::ToolText;
using lubancode::tools::WriteFileTool;

namespace {

// 还原语言状态:析构时切回 zh-CN(全局初始值)。
struct LangGuard {
    ~LangGuard() {
        lubancode::cli::SetLanguage("zh-CN");
    }
};

// 迁移前 read_file::description() 的原文,逐字节(从 cpp 字面量原样抄)。
const char* kReadFileDescBefore =
    "读取文件内容,每行前面带上行号(类似 cat -n)。参数 offset/limit 可以只读文件的一部分;"
    "limit 省略时默认最多读 2000 行,单次输出至多约 1MB,超出会截断并标注,可以用 offset "
    "从截断处继续翻页。路径可以是相对路径,也可以是绝对路径。只收 UTF-8 文本(带不带 BOM "
    "都行,BOM 不会混进正文);二进制文件或不是合法 UTF-8 的文件(比如 GBK 编码)会明确报错,"
    "请先转存成 UTF-8 再读。";

const char* kWriteFileDescBefore =
    "把内容写入文件(UTF-8 编码)。文件已存在就整个覆盖,父目录不存在会自动建好。"
    "路径可以是相对路径,也可以是绝对路径。适合新建文件或整篇重写;小范围改动用 "
    "edit_file 更精准。执行前需要用户确认。";

}  // namespace

TEST_CASE("ToolText: 缺省语言(zh-CN)取到 zh-CN 档,与兜底原文逐字节一致") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    CHECK(ToolText("read_file", "description") == kReadFileDescBefore);
    CHECK(ToolText("write_file", "description") == kWriteFileDescBefore);
    CHECK(ToolText("read_file", "param.path") == "要读取的文件路径,相对或绝对均可");
}

TEST_CASE("ToolText: en 语言取 en 档(英文生效)") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    const std::string desc = ToolText("read_file", "description");
    CHECK_FALSE(desc.empty());
    // 英文档的特征词;中文原文里没有的 ASCII 串。
    CHECK(desc.find("Read file contents") == 0);
    CHECK(desc.find("UTF-8") != std::string::npos);
    CHECK(ToolText("write_file", "param.content") ==
          "File content to write (UTF-8); replaces the original file entirely");
}

TEST_CASE("ToolText: 缺键回退 zh-CN,再缺回退兜底字串") {
    LangGuard guard;
    // 没搬进数据文件的工具:en 没有、zh-CN 也没有 → 兜底。
    lubancode::cli::SetLanguage("en");
    CHECK(ToolText("not_migrated_yet", "description", "兜底原文") == "兜底原文");
    CHECK(ToolText("not_migrated_yet", "description") == "");
    // zh-CN 语言下同一查询也走兜底。
    lubancode::cli::SetLanguage("zh-CN");
    CHECK(ToolText("not_migrated_yet", "description", "兜底原文") == "兜底原文");
    // 键不存在但工具存在:zh-CN 档没有这个键 → 兜底。
    CHECK(ToolText("read_file", "param.nonexistent", "兜底原文") == "兜底原文");
}

TEST_CASE("ToolText: 语言切换即时生效(每次调用现查,不用重启)") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    CHECK(ToolText("read_file", "description").find("Read file contents") == 0);
    lubancode::cli::SetLanguage("zh-CN");
    CHECK(ToolText("read_file", "description") == kReadFileDescBefore);
    // 再切回去,同一条查询出英文。
    lubancode::cli::SetLanguage("en");
    CHECK(ToolText("read_file", "description").find("Read file contents") == 0);
}

TEST_CASE("试点工具: description()/input_schema() 缺省与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    ReadFileTool read;
    CHECK(read.description() == kReadFileDescBefore);
    const nlohmann::json schema = read.input_schema();
    CHECK(schema["properties"]["path"]["description"] == "要读取的文件路径,相对或绝对均可");
    CHECK(schema["properties"]["offset"]["description"] == "从第几行开始读(从 1 计数),不填就从第 1 行开始");
    CHECK(schema["properties"]["limit"]["description"] == "最多读多少行,不填就读到文件末尾");
    CHECK(schema["required"] == nlohmann::json::array({"path"}));

    WriteFileTool write;
    CHECK(write.description() == kWriteFileDescBefore);
    const nlohmann::json wschema = write.input_schema();
    CHECK(wschema["properties"]["path"]["description"] == "要写入的文件路径,相对或绝对均可");
    CHECK(wschema["properties"]["content"]["description"] == "要写入的文件内容(UTF-8),会整体覆盖原文件");
    CHECK(wschema["required"] == nlohmann::json::array({"path", "content"}));
}

TEST_CASE("试点工具: en 下 description 与 schema 参数说明都是英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    ReadFileTool read;
    CHECK(read.description().find("Read file contents") == 0);
    const nlohmann::json schema = read.input_schema();
    CHECK(schema["properties"]["path"]["description"] == "File path to read; relative or absolute both work");
    CHECK(schema["properties"]["offset"]["description"] ==
          "Line number to start reading from (1-based); omit to start from line 1");

    WriteFileTool write;
    CHECK(write.description().find("Write content to a file") == 0);
    CHECK(write.input_schema()["properties"]["content"]["description"] ==
          "File content to write (UTF-8); replaces the original file entirely");
}

// ---------------------------------------------------------------------------
// 批3:代理族(agent / agent_message + 子代理 persona)。
// 迁移前原文从 origin/main 的 cpp 字面量逐字节抄来,一字不改。
// ---------------------------------------------------------------------------

namespace {

const char* kAgentDescBefore =
    "把独立任务委托给子代理。先想一个 4~16 字(英文 2~6 个词)的语义短标题填 title——名词短语或短命令,能彼此区分,"
    "不要照抄 prompt 首句、不要塞路径清单或套话;再把完整的任务说明写进 prompt。title 给人看(代理面板/日志),"
    "prompt 给子代理执行,两者各司其职。agent_type=Explore 是只读代码搜索代理;general-purpose 能研究、执行多步任务和改代码。"
    "子代理有独立上下文,只把结论交回主对话。执行模式看 execution_mode(缺省 auto):交互会话里探索、生成、写代码、"
    "调研这类独立任务用缺省 auto 即可——后台独立跑,完成后结论自动交回,主对话还能继续干别的;不要为了拿结果"
    "习惯性写 foreground,后台结果一样会回流,只有紧接着的下一步非等这份结果不可才显式写 foreground。"
    "管道/单发场景 auto 等价前台(阻塞等结论)。后台任务不能弹权限确认,"
    "未预先放行的操作会被拒绝。子代理看不见当前对话历史,prompt 必须自包含。";

const char* kAgentPersonaGeneralBefore =
    "你是 general-purpose 子代理,能搜索、分析并完成多步任务。专注给定任务,完成后直接给出结论,不要寒暄。";

const char* kAgentPersonaExploreBefore =
    "你是 Explore 子代理,专门快速搜索、阅读并分析代码库。只读,不得改文件、启动会改动环境的命令或做别的写操作。"
    "完成后给出简明结论和具体文件位置,不要寒暄。";

const char* kAgentMessageDescBefore =
    "给运行中的子代理传增量要求。只插话:不新建任务(那是 agent 工具的事),"
    "不打断它正在执行的工具,不复活已结束的任务。何时必须用:用户在主会话补充、"
    "修改或撤回要求,若影响某只运行中子代理,先调本工具把增量转交给它,再继续回答;"
    "用户点名某只任务时按 task_id 精确投递;一条要求影响多只就逐只各发一条,没有广播;"
    "目标不清先问用户,不要凭标题相近乱投;只传增量,不重复整份任务说明;不要因为主代理"
    "自己也记住了就省掉转交——子代理有独立上下文,看不见主会话新消息;本工具返回 queued"
    "之后才能对用户说已传到,调用前不得把转交说成既成事实。message 写法:先逐字引用用户"
    "原话(以\"用户原话:\"起头);主代理自己添的解释另起一栏(以\"[主代理补充上下文]\""
    "起头),不得把推断冒充用户要求。消息会在该子代理当前工具收尾后的下一次模型请求前"
    "送达;它被当作普通用户侧补充,不是权限确认,不会执行其中的 slash 命令,也不能借它"
    "绕过任何确认。运行中任务的名册见每条用户消息附带的\"运行中子代理名册\"。";

}  // namespace

// AgentTool 构造只要 backend 与工具表(不真跑):最小实现,不发请求。
struct NullBackend : lubancode::api::Backend {
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        return {};
    }
};

TEST_CASE("批3 代理族: 缺省(zh-CN)description 与 persona 逐字节回到改前原文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    NullBackend backend;
    lubancode::tools::ToolRegistry registry;
    AgentTool agent(backend, registry, "/work/dir");
    CHECK(agent.description() == kAgentDescBefore);
    CHECK(ToolText("agent", "persona.general") == kAgentPersonaGeneralBefore);
    CHECK(ToolText("agent", "persona.explore") == kAgentPersonaExploreBefore);

    const nlohmann::json schema = agent.input_schema();
    CHECK(schema["properties"]["title"]["description"] ==
          "任务短标题,必填。给人看的语义字段:中文 4~16 字、英文 2~6 个词,名词短语或短命令,能与其他任务区分。"
          "不得照抄 prompt 首句,不得含路径清单/验收全文/换行/制表符,硬上限 40 显示列。先概括 title,再写完整 prompt。");
    CHECK(schema["properties"]["prompt"]["description"] ==
          "交给子代理的任务描述,必须自包含——子代理看不见主对话历史,任务目标、范围、期望的输出形式都要写清楚。");
    CHECK(schema["properties"]["agent_type"]["description"] ==
          "子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作。默认 general-purpose。");
    CHECK(schema["properties"]["isolation"]["description"] ==
          "worktree = 给子代理单独开一间 git worktree 隔离房干活:写不碰主 checkout(文件/命令/git 三道闸拦),"
          "干完没改动房自动删,有改动则保留并在结果里附房路径与分支,由主代理或用户收尾。"
          "改代码的多步任务建议带上;只读摸排不必。缺省 none。");
    CHECK(schema["required"] == nlohmann::json::array({"title", "prompt"}));

    AgentMessageTool message(nullptr);
    CHECK(message.description() == kAgentMessageDescBefore);
    const nlohmann::json mschema = message.input_schema();
    CHECK(mschema["properties"]["task_id"]["description"] ==
          "运行中子代理的稳定任务号(见\"运行中子代理名册\"里的 #N)。");
    CHECK(mschema["properties"]["message"]["description"] ==
          "送给该任务的增量要求;写清改了什么、为何改、验收受何影响。先逐字引用用户原话"
          "(\"用户原话:\"起头),主代理自己的解释另起一栏(\"[主代理补充上下文]\"起头)。");
    CHECK(mschema["required"] == nlohmann::json::array({"task_id", "message"}));
}

TEST_CASE("批3 代理族: en 下 description、参数说明与 persona 都出英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    NullBackend backend;
    lubancode::tools::ToolRegistry registry;
    AgentTool agent(backend, registry, "/work/dir");
    CHECK(agent.description().find("Delegate independent tasks to subagents") == 0);
    CHECK(agent.input_schema()["properties"]["agent_type"]["description"] ==
          "Subagent type: Explore is read-only search and analysis; general-purpose can perform multi-step operations. "
          "Default general-purpose.");
    // persona 键同样走 en 档:Explore 只读人格的英文特征句。
    CHECK(ToolText("agent", "persona.explore").find("You are an Explore subagent") == 0);
    CHECK(ToolText("agent", "persona.general").find("You are a general-purpose subagent") == 0);

    AgentMessageTool message(nullptr);
    CHECK(message.description().find("Hand incremental requirements to a running subagent") == 0);
    CHECK(message.input_schema()["properties"]["task_id"]["description"] ==
          "Stable task number of the running subagent (see #N in the \"running subagents roster\").");
}

// ---------------------------------------------------------------------------
// 批4:交互(ask_user / todo_write)。
// ---------------------------------------------------------------------------

namespace {

const char* kAskUserDescBefore =
    "当任务缺少会改变实现方向的用户选择时,用选择题向用户询问。一次可问 1 到 4 题,"
    "每题给 2 到 4 个备选项;界面会自动追加“自己填写”。不要拿它询问可自行查明的细节。";

const char* kTodoWriteDescBefore =
    "维护本次会话的待办清单,整表替换(每次调用都要传完整的清单,不是增量更新——"
    "漏掉的项这次就没了)。多步骤任务开工前先列一份清单,每完成一步就把对应项的 "
    "status 改成 completed 再整表传一次,让用户能看到进度。items 传空数组表示清空清单。";

}  // namespace

TEST_CASE("批4 交互: 缺省(zh-CN)与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    AskUserTool ask([](const lubancode::tools::AskUserQuestion&) {
        return std::expected<std::vector<std::string>, std::string>{std::vector<std::string>{"a"}};
    });
    CHECK(ask.description() == kAskUserDescBefore);
    const nlohmann::json aschema = ask.input_schema();
    const nlohmann::json& qprops = aschema["properties"]["questions"]["items"]["properties"];
    CHECK(qprops["header"]["description"] == "简短题头,建议不超过 12 个字");
    CHECK(qprops["question"]["description"] == "完整问题");
    CHECK(qprops["multi_select"]["description"] == "是否允许多选,默认 false");
    CHECK(aschema["required"] == nlohmann::json::array({"questions"}));

    lubancode::tools::TodoWriteTool todo(std::make_shared<lubancode::tools::TodoListState>());
    CHECK(todo.description() == kTodoWriteDescBefore);
    const nlohmann::json tschema = todo.input_schema();
    CHECK(tschema["properties"]["items"]["description"] ==
          "完整的待办清单,整表替换(不是增量更新,每次都传全量列表)");
    const nlohmann::json& iprops = tschema["properties"]["items"]["items"]["properties"];
    CHECK(iprops["content"]["description"] == "这一项要做的事,一句话说清楚");
    CHECK(iprops["status"]["description"] == "这一项当前的状态");
    CHECK(iprops["status"]["enum"] == nlohmann::json::array({"pending", "in_progress", "completed"}));
}

TEST_CASE("批4 交互: en 下 description 与参数说明都是英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    AskUserTool ask([](const lubancode::tools::AskUserQuestion&) {
        return std::expected<std::vector<std::string>, std::string>{std::vector<std::string>{"a"}};
    });
    CHECK(ask.description().find("When a task hinges on a user choice") == 0);
    const nlohmann::json aschema = ask.input_schema();
    CHECK(aschema["properties"]["questions"]["items"]["properties"]["question"]["description"] ==
          "The full question");

    lubancode::tools::TodoWriteTool todo(std::make_shared<lubancode::tools::TodoListState>());
    CHECK(todo.description().find("Maintain the todo list for this session") == 0);
    CHECK(todo.input_schema()["properties"]["items"]["description"] ==
          "The complete todo list, replaced wholesale (not an incremental update; pass the full list every time)");
}

// ---------------------------------------------------------------------------
// 批5:外接(lsp / context_search / context_read)。
// ---------------------------------------------------------------------------

namespace {

const char* kLspDescBefore =
    "用 LSP 语言服务器做语义查询:mode=definition 查定义(需要 line/character),"
    "mode=references 查引用(需要 line/character),mode=symbols 列文件里的符号,"
    "mode=diagnostics 看文件的诊断(错误/警告)。line/character 是 1 基,跟编辑器显示一致。"
    "只有 config 的 lsp 段配置过的语言(按文件扩展名路由)才能查。";

const char* kContextSearchDescBefore =
    "在先前工具输出的落盘全文(artifact)里按关键词检索。工具结果太长时,请求里只留"
    "[artifact aNNNN ...] 引用(头尾预览);预览不够就用本工具搜全文,拿命中行号与块 id,"
    "再用 context_read 读出上下文。不可把预览的省略号当全文。";

const char* kContextReadDescBefore =
    "按稳定 id 读先前工具输出落盘全文(artifact)的一段:给 chunk_id(context_search 命中给的)"
    "或 line_start(1 起)+line_count。单次最多 32 KiB,超了会拒绝并给可用范围。"
    "全文真本按 sha256 校验,hash 不合的内容不会被供给。";

}  // namespace

TEST_CASE("批5 外接: 缺省(zh-CN)与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    lubancode::lsp::Manager manager({}, "D:\\proj");  // 空配置:只查文案,不起服务器
    lubancode::tools::LspTool lsp(manager);
    CHECK(lsp.description() == kLspDescBefore);
    const nlohmann::json lschema = lsp.input_schema();
    CHECK(lschema["properties"]["mode"]["description"] == "查询类型");
    CHECK(lschema["properties"]["file"]["description"] == "要查询的文件路径(相对或绝对)");
    CHECK(lschema["properties"]["line"]["description"] == "行号,1 基(definition/references 必填)");
    CHECK(lschema["properties"]["character"]["description"] == "列号,1 基(definition/references 必填)");
    CHECK(lschema["required"] == nlohmann::json::array({"mode", "file"}));

    ContextSearchTool search(nullptr);
    CHECK(search.description() == kContextSearchDescBefore);
    const nlohmann::json sschema = search.input_schema();
    CHECK(sschema["properties"]["artifact_id"]["description"] == "[artifact aNNNN ...] 标记里的 aNNNN");
    CHECK(sschema["properties"]["query"]["description"] == "关键词(ASCII 大小写不敏感,中文按原文)");
    CHECK(sschema["properties"]["max_results"]["description"] == "最多回几条命中(默认 8)");
    CHECK(sschema["required"] == nlohmann::json::array({"artifact_id", "query"}));

    ContextReadTool read(nullptr);
    CHECK(read.description() == kContextReadDescBefore);
    const nlohmann::json rschema = read.input_schema();
    CHECK(rschema["properties"]["artifact_id"]["description"] == "[artifact aNNNN ...] 标记里的 aNNNN");
    CHECK(rschema["properties"]["chunk_id"]["description"] == "块 id(如 c0003);给了就按块读");
    CHECK(rschema["properties"]["line_start"]["description"] == "起始行(1 起;与 chunk_id 二选一)");
    CHECK(rschema["properties"]["line_count"]["description"] == "读几行;0 = 读到结尾");
    CHECK(rschema["required"] == nlohmann::json::array({"artifact_id"}));
}

TEST_CASE("批5 外接: en 下 description 与参数说明都是英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    lubancode::lsp::Manager manager({}, "D:\\proj");
    lubancode::tools::LspTool lsp(manager);
    CHECK(lsp.description().find("Run semantic queries through an LSP language server") == 0);
    const nlohmann::json lschema = lsp.input_schema();
    CHECK(lschema["properties"]["mode"]["description"] == "Query type");
    CHECK(lschema["properties"]["line"]["description"] ==
          "Line number, 1-based (required for definition/references)");

    ContextSearchTool search(nullptr);
    CHECK(search.description().find("Search the spilled full text of earlier tool outputs") == 0);
    CHECK(search.input_schema()["properties"]["max_results"]["description"] ==
          "Maximum number of hits to return (default 8)");

    ContextReadTool read(nullptr);
    CHECK(read.description().find("Read a segment of the spilled full text") == 0);
    CHECK(read.input_schema()["properties"]["chunk_id"]["description"] ==
          "Chunk id (e.g. c0003); when given, read by chunk");
}
