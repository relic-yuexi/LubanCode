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
#include <filesystem>
#include <functional>
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
#include "tools/tool_search.hpp"
#include "tools/tool_text.hpp"
#include "tools/todo_tool.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/worktree_tool.hpp"
#include "tools/write_file.hpp"

using lubancode::tools::AgentMessageTool;
using lubancode::tools::AgentTool;
using lubancode::tools::AskUserTool;
using lubancode::tools::BackgroundOutputTool;
using lubancode::tools::ContextReadTool;
using lubancode::tools::ContextSearchTool;
using lubancode::tools::EditFileTool;
using lubancode::tools::ReadFileTool;
using lubancode::tools::RunCommandTool;
using lubancode::tools::SearchTool;
using lubancode::tools::StopBackgroundTool;
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
// 批1 文件工具余量(edit_file/search)与批2 命令族(run_command/
// background_output/stop_background)的迁移断言。改前原文逐字节从 cpp 字面量
// 抄出,与 zh-CN 档(缺省)逐字节钉死;en 档抽特征词验生效。run_command 的
// 平台分档文案(description 与 command/shell 两参数)按编译平台走对应的键,
// POSIX 节只在非 Windows 平台可查。
// 批3-5:代理族(agent/agent_message + persona)、交互(ask_user/todo_write)、
// 外接(lsp/context_search/context_read)同理,原文逐字节抄来。
// ---------------------------------------------------------------------------

namespace {

const char* kEditFileDescBefore =
    "对已有文件做字符串替换:先精确匹配,失败后会有限兼容 CRLF/LF、统一缩进和行尾空白。"
    "old_string 仍须唯一出现(除非把 replace_all 设成 true),多处候选绝不猜。"
    "适合小范围、精准的改动,不适合整篇重写(整篇重写用 write_file)。执行前需要用户确认。";

const char* kSearchDescBefore =
    "在目录或单个文件里搜索,两种模式:mode=\"grep\" 按正则(ECMAScript 语法)搜文件内容,"
    "命中的行按 文件:行号:行内容 返回;mode=\"glob\" 按文件名通配(支持 * ? **)找文件,"
    "返回相对路径列表。默认从当前工作目录开始搜,自动跳过 .git/、build/、"
    "node_modules/ 和二进制文件。结果超过 100 条会截断并注明。";

#ifdef _WIN32
const char* kRunCommandDescBefore =
    "在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
    "shell 参数可选 powershell(默认)或 cmd,分别按对应语法写命令。执行前要经用户确认。"
    "超时会被强制杀掉。"
    "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,"
    "传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;"
    "命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)"
    "查状态/读输出,stop_background 工具收尾。";
#else
const char* kRunCommandDescBefore =
    "在 shell(/bin/sh)里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
    "按 POSIX sh 语法写命令。执行前要经用户确认。超时会被强制杀掉。"
    "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,"
    "传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;"
    "命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)"
    "查状态/读输出,stop_background 工具收尾。";
#endif

const char* kBackgroundOutputDescBefore =
    "查后台命令(run_command run_in_background:true 起的那些)的运行状态和输出。"
    "不给 task_id 就列出全部后台任务的摘要:task_id、状态(运行中/完成/失败/已停止)、"
    "命令、PID、日志文件路径。给 task_id 就返回该任务的详情 + 日志文件尾部 tail_lines 行"
    "(默认 50)。任务还在跑也能读,文件允许边写边读。"
    "起完一个后台命令后,用它查进度/结果,不用自己再拼 tail 命令。";

const char* kStopBackgroundDescBefore =
    "停掉一个后台命令(run_command run_in_background:true 起的)。Windows 上 "
    "TerminateProcess 根进程,POSIX 上 kill 杀整个进程组。已完成的任务不会重复杀。"
    "长命进程(dev server、watch、build)跑够了、或者起错了想收掉,用它。";

}  // namespace

TEST_CASE("文件工具批余量: edit_file/search 缺省与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    EditFileTool edit;
    CHECK(edit.description() == kEditFileDescBefore);
    const nlohmann::json es = edit.input_schema();
    CHECK(es["properties"]["path"]["description"] == "要修改的文件路径,相对或绝对均可");
    CHECK(es["properties"]["old_string"]["description"] ==
          "要被替换掉的原文。优先逐字匹配,必要时兼容换行、统一缩进与行尾空白;仍须唯一命中");
    CHECK(es["properties"]["new_string"]["description"] == "替换成的新内容");
    CHECK(es["properties"]["replace_all"]["description"] ==
          "true 就把所有出现的地方都替换掉,不填默认 false(要求唯一命中)");
    CHECK(es["required"] == nlohmann::json::array({"path", "old_string", "new_string"}));

    SearchTool search;
    CHECK(search.description() == kSearchDescBefore);
    const nlohmann::json ss = search.input_schema();
    CHECK(ss["properties"]["mode"]["description"] ==
          "\"grep\" 搜文件内容(正则),\"glob\" 按文件名找文件(通配符)");
    CHECK(ss["properties"]["pattern"]["description"] ==
          "mode=grep 时是 ECMAScript 正则表达式;mode=glob 时是文件名通配符(支持 * ? **)。"
          "不带 '/' 的写法(如 *.md)按文件名匹配,会递归找出整个目录树下所有同名文件,"
          "不管它在哪层子目录里;带 '/' 的写法(如 src/**/*.hpp、docs/**)按相对路径匹配,"
          "'**/' 表示零层或多层目录,写在开头就是'不管在不在根目录都算'。");
    CHECK(ss["properties"]["path"]["description"] ==
          "从哪里开始搜:给目录就递归遍历,给单个文件就只搜这一个。不填默认当前工作目录");
    CHECK(ss["properties"]["glob"]["description"] ==
          "仅 mode=grep 有效:按文件名或路径过滤要搜索的文件,不填就搜所有非二进制文件。"
          "语义跟 pattern 的 glob 写法一样:*.cpp 这种不带 '/' 的按文件名递归匹配任意目录下的文件;"
          "src/**/*.hpp 这种带 '/' 的按相对路径匹配。");
    CHECK(ss["required"] == nlohmann::json::array({"mode", "pattern"}));
}

TEST_CASE("文件工具批余量: en 下 edit_file/search 出英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    EditFileTool edit;
    CHECK(edit.description().find("Replace strings in an existing file") == 0);
    const nlohmann::json es = edit.input_schema();
    CHECK(es["properties"]["old_string"]["description"].get<std::string>().find(
              "The original text to be replaced") == 0);

    SearchTool search;
    CHECK(search.description().find("Search a directory or a single file") == 0);
    const nlohmann::json ss = search.input_schema();
    CHECK(ss["properties"]["pattern"]["description"].get<std::string>().find(
              "With mode=grep this is an ECMAScript regular expression") == 0);
}

TEST_CASE("命令族批: run_command/background_output/stop_background 缺省与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");
    RunCommandTool run;
    CHECK(run.description() == kRunCommandDescBefore);
    const nlohmann::json rs = run.input_schema();
#ifdef _WIN32
    CHECK(rs["properties"]["command"]["description"] ==
          "要执行的命令,按所选 shell 的语法写(默认 PowerShell 语法)");
    CHECK(rs["properties"]["shell"]["description"] == "用哪个 shell 执行,不填默认 powershell");
#else
    CHECK(rs["properties"]["command"]["description"] == "要执行的命令,按 POSIX sh 语法写");
    CHECK(rs["properties"]["shell"]["description"] ==
          "用哪个 shell 执行,本平台只有 sh(/bin/sh);powershell/cmd 是 Windows 专属");
#endif
    CHECK(rs["properties"]["timeout_ms"]["description"] == "超时时间,单位毫秒,不填默认 120000(2 分钟)");
    CHECK(rs["properties"]["run_in_background"]["description"] ==
          "true = 后台运行,不等命令跑完就返回。用于起 dev server、watch 进程这类要跨命令、"
          "跨多轮调用继续存活的长命进程——起完之后你还要接着用别的命令(比如 curl)去验证它;"
          "也用于后台跑一个短任务,不想阻塞当前对话、跑完通知你即可。"
          "spawn 成功后立刻返回结果,内含 task_id、子进程 PID 和一个日志文件路径(该进程的标准"
          "输出/标准错误合并写在这个文件里);命令跑完时,下一次给提示符会打一行完成通知。"
          "之后想看它是否还活着、看它吐了什么,用 background_output 工具(传 task_id)查状态读输出,"
          "要收掉它就用 stop_background 工具。"
          "timeout_ms 参数对这个模式没有意义,会被忽略。不填默认 false(前台执行,等命令跑完拿"
          "完整输出和退出码)。");
    CHECK(rs["properties"]["cwd"]["description"] ==
          "命令的工作目录,相对或绝对均可;不填用当前会话工作目录。目录必须真实存在。"
          "住隔离 worktree 的会话里,指向主 checkout 的目录会被拒绝");
    CHECK(rs["required"] == nlohmann::json::array({"command"}));

    BackgroundOutputTool bg;
    CHECK(bg.description() == kBackgroundOutputDescBefore);
    const nlohmann::json bs = bg.input_schema();
    CHECK(bs["properties"]["task_id"]["description"] ==
          "要查的后台任务 id(run_command 后台返回的那个编号字符串)。"
          "不给就列出所有后台任务的摘要。");
    CHECK(bs["properties"]["tail_lines"]["description"] ==
          "读日志文件的末尾几行,默认 50。给 task_id 时才用;<=0 表示读全文(上限 64KB)。");

    StopBackgroundTool stop;
    CHECK(stop.description() == kStopBackgroundDescBefore);
    CHECK(stop.input_schema()["properties"]["task_id"]["description"] ==
          "要停的后台任务 id(run_command 后台返回的那个编号字符串)。");
    CHECK(stop.input_schema()["required"] == nlohmann::json::array({"task_id"}));
}

TEST_CASE("命令族批: en 下 run_command/background_output/stop_background 出英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");
    RunCommandTool run;
    CHECK(run.description().find("Run one command in a shell") == 0);
    const nlohmann::json rs = run.input_schema();
#ifdef _WIN32
    CHECK(rs["properties"]["shell"]["description"].get<std::string>().find(
              "omit for the default powershell") != std::string::npos);
#else
    CHECK(rs["properties"]["command"]["description"].get<std::string>().find(
              "write it in POSIX sh syntax") != std::string::npos);
#endif
    CHECK(rs["properties"]["run_in_background"]["description"].get<std::string>().find(
              "true = run in the background") == 0);

    BackgroundOutputTool bg;
    CHECK(bg.description().find("Check the status and output of background commands") == 0);
    CHECK(bg.input_schema()["properties"]["tail_lines"]["description"].get<std::string>().find(
              "How many lines to read from the end of the log file") == 0);

    StopBackgroundTool stop;
    CHECK(stop.description().find("Stop a background command") == 0);
    CHECK(stop.input_schema()["properties"]["task_id"]["description"].get<std::string>().find(
              "The background task id to stop") == 0);
}

// ---------------------------------------------------------------------------
// 批3-5 的断言(合并自批3-5 分支)。
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

// ---------------------------------------------------------------------------
// 清底批:余量九件——web_search/web_fetch/tool_search/skill(网络与外挂检索)、
// list_sessions/send_session_message/worktree(会话与房)、memory_save、
// programmatic_tool_calling。改前原文逐字节从 cpp 字面量抄出,与 zh-CN 档
// (缺省)逐字节钉死;en 档抽特征词验生效。PtcTool 的 schema 里
// purpose/script 两参数本就没有 description 字段,只有 description 一个键。
// ---------------------------------------------------------------------------

namespace {

const char* kWebSearchDescBefore =
    "网络搜索,返回编号列表(标题/URL/摘要)。适合查最新资讯、找文档地址;拿到 URL 之后"
    "用 web_fetch 抓正文。需要搜好几轮、读好几篇再总结的活,交给 agent 子代理去干。";

const char* kWebFetchDescBefore =
    "抓取一个网页(HTTP GET,跟随重定向)。HTML 会剥掉标签只留正文,普通文本原样返回,"
    "二进制内容不支持。返回内容开头带一行 URL/状态码/类型说明。适合看文档、查资料;"
    "需要深读多个长网页再总结时,把活交给 agent 子代理去做,别把整篇长文堆进主对话。";

const char* kToolSearchDescBefore =
    "按关键词检索延迟挂载的工具(MCP/插件等外挂工具不直接进工具表,只在系统提示的索引段里露名字)。"
    "对工具名和描述做大小写不敏感的分词匹配,命中的工具立即挂载,本轮之后即可直接调用。"
    "当索引段里有你需要的能力、或怀疑有外挂工具能干这件事时,先用这个搜。";

const char* kSkillDescBefore =
    "按名字加载一份已发现的技能(SKILL.md),拿到它的完整使用说明。技能是预先写好的一套具体做法"
    "(比如某种文体的写作规范、某类任务的固定流程),系统提示里列出的技能名/说明跟当前任务对得上时,"
    "先调用这个工具把说明读进来,再照着做。";

const char* kListSessionsDescBefore =
    "列出同一台机器上当前用户开启的其它 Lubancode 会话(不跨机器、不跨用户)。"
    "每场会话给出 peer_id(短 id,发送消息时用来定人)、名字、状态(空闲/忙/等待)、"
    "工作目录。只有在手头的结论会影响另一场活会话时才需要查它;不要闲聊、不要催问成环。";

const char* kSendSessionMessageDescBefore =
    "给同一台机器上另一场 Lubancode 会话递一条纯文本消息(不传文件、不传聊天记录,只递一张字条)。"
    "target 填对方的名字或 peer_id(list_sessions 可查)。对端正忙时消息会在两次工具调用之间送达,"
    "不打断它手头的工具;对端空闲则另起一轮。只有在手头结论会影响另一场活会话时才发送;"
    "不许闲聊,不许催问成环。";

const char* kWorktreeDescBefore =
    "住进隔离的 git worktree 里干活,不碰主 checkout。大改动先 worktree enter(缺省名字自动生成,"
    "基准 fresh=远端默认分支或 head=当前 HEAD),整场会话搬进房里:读写、命令都在房内,"
    "状态行会亮房名;干完 worktree exit keep(留房)或 exit remove(干净才删,脏了要用户确认)。"
    "worktree status 看在不在房里、脏没脏;worktree list 列全部工作树。"
    "别把构建产物提交进房里;房里的改动最终仍要合回主分支。";

const char* kMemorySaveDescBefore =
    "把一条小而稳定的项目事实、用户明确偏好或用户明说的行事纠正排进后台记忆(正式入库,不经待审区)。"
    "只在信息已经由源码、工具结果或用户明说证实时调用;fact 必须在 paths 或 evidence 里"
    "给出可核验证据;feedback 只收用户当场明说的纠正(如版本节奏、验收习惯),confidence 须"
    "user-stated,模型推断不得直写。不要保存当前任务进度、猜测、日志、网页/MCP 原文、密钥或个人数据。"
    "已有同主题时沿用索引里的 id 做更新。自动候选走回合总结,不经过这个工具。";

const char* kPtcDescBefore =
    "编排一段 Python 脚本批量调用已挂载的只读工具(read_file/search 等):写变量、条件、循环、"
    "asyncio.gather 扇出,一段脚本收完把 emit() 的精简摘要送回。适合遍历一批文件、先查 A 再喂 "
    "B/C 的长链、同时派多路只读调用后聚合;短任务直接用普通工具更省。输入给 purpose(一句话"
    "目的,进审计账)与 script(Python 源码,import luban_tools 拿 typed stubs,结尾必须 emit)。";

}  // namespace

TEST_CASE("清底批: 缺省(zh-CN)与改前一字不差") {
    LangGuard guard;
    lubancode::cli::SetLanguage("zh-CN");

    lubancode::config::SearchConfig search_config;
    search_config.provider = "tavily";
    search_config.api_key = "test";
    lubancode::tools::WebSearchTool web_search(search_config);
    CHECK(web_search.description() == kWebSearchDescBefore);
    const nlohmann::json wss = web_search.input_schema();
    CHECK(wss["properties"]["query"]["description"] == "搜索关键词或问题");
    CHECK(wss["properties"]["count"]["description"] == "想要几条结果,不填默认 5,上限 10");
    CHECK(wss["required"] == nlohmann::json::array({"query"}));

    lubancode::tools::WebFetchTool web_fetch;
    CHECK(web_fetch.description() == kWebFetchDescBefore);
    const nlohmann::json wfs = web_fetch.input_schema();
    CHECK(wfs["properties"]["url"]["description"] == "要抓取的完整 URL,必须以 http:// 或 https:// 开头");
    CHECK(wfs["properties"]["max_bytes"]["description"] ==
          "返回正文的字节数上限,超出截断并标注。不填默认 102400(100KB)");
    CHECK(wfs["required"] == nlohmann::json::array({"url"}));

    lubancode::tools::ToolRegistry clearance_registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    lubancode::tools::ToolSearchTool tool_search(clearance_registry, loaded);
    CHECK(tool_search.description() == kToolSearchDescBefore);
    const nlohmann::json tss = tool_search.input_schema();
    CHECK(tss["properties"]["query"]["description"] ==
          "关键词,空格分隔多个词;对延迟工具的名字和描述做大小写不敏感的子串匹配,"
          "按命中词数排序。");
    CHECK(tss["properties"]["limit"]["description"] == "最多返回并挂载几个,不填默认 5。");
    CHECK(tss["required"] == nlohmann::json::array({"query"}));

    lubancode::tools::SkillTool skill(std::vector<lubancode::tools::SkillMeta>{});
    CHECK(skill.description() == kSkillDescBefore);
    CHECK(skill.input_schema()["properties"]["name"]["description"] ==
          "要加载的技能名,跟系统提示里列出的名字一致");
    CHECK(skill.input_schema()["required"] == nlohmann::json::array({"name"}));

    lubancode::tools::ListSessionsTool list_sessions(
        [] { return std::vector<lubancode::agent::PeerCard>{}; }, "self");
    CHECK(list_sessions.description() == kListSessionsDescBefore);
    CHECK(list_sessions.input_schema() == nlohmann::json::object());

    lubancode::tools::SendSessionMessageTool send_session(
        [] { return std::vector<lubancode::agent::PeerCard>{}; },
        [](const lubancode::agent::PeerCard&, const std::string&) {
            return lubancode::agent::PeerDelivery::Delivered;
        });
    CHECK(send_session.description() == kSendSessionMessageDescBefore);
    const nlohmann::json sms = send_session.input_schema();
    CHECK(sms["properties"]["target"]["description"] == "对方会话的名字或 peer_id");
    CHECK(sms["properties"]["text"]["description"] == "纯文本正文");
    CHECK(sms["required"] == nlohmann::json::array({"target", "text"}));

    lubancode::cli::WorktreeSession worktree_session;
    lubancode::tools::WorktreeTool worktree(worktree_session, nullptr, nullptr);
    CHECK(worktree.description() == kWorktreeDescBefore);
    const nlohmann::json wts = worktree.input_schema();
    CHECK(wts["properties"]["action"]["description"] ==
          "enter=建房或进已有房(整场会话搬进去);status=房内状态;list=列工作树;"
          "exit=搬回原处(配 mode)");
    CHECK(wts["properties"]["name"]["description"] ==
          "enter 时的房名(字母数字-_),不填自动生成;也可传已有 worktree 的名字或路径,"
          "园子(.lubancode/worktrees)之外的房要先经用户确认");
    CHECK(wts["properties"]["base"]["description"] ==
          "enter 建新房的基准:fresh=远端默认分支(缺省,fetch 5 秒封顶失败回落本地);"
          "head=当前 HEAD");
    CHECK(wts["properties"]["mode"]["description"] ==
          "exit 的方式:keep=房留在盘上;remove=干净才删(脏了必须用户确认,别替用户点头)");
    CHECK(wts["required"] == nlohmann::json::array({"action"}));

    lubancode::memory::Options memory_options;
    lubancode::memory::ProjectIdentity memory_identity;
    auto memory_store = std::make_shared<lubancode::memory::ProjectMemory>(
        memory_identity, std::filesystem::path("D:\\never\\used"), memory_options);
    lubancode::memory::MemorySaveTool memory_save(memory_store);
    CHECK(memory_save.description() == kMemorySaveDescBefore);
    const nlohmann::json mss = memory_save.input_schema();
    CHECK(mss["properties"]["kind"]["description"] ==
          "fact=可核验的项目事实；preference=用户明确说出的本项目偏好；"
          "feedback=用户明说的行事纠正(须 user-stated)");
    CHECK(mss["properties"]["id"]["description"] == "可选。更新已有记忆时用索引里的稳定 id");
    CHECK(mss["properties"]["title"]["description"] == "一个可独立更新的短主题");
    CHECK(mss["properties"]["summary"]["description"] == "索引里的一行摘要");
    CHECK(mss["properties"]["content"]["description"] ==
          "精炼正文，写事实、证据与注意事项，不抄大段源码");
    CHECK(mss["properties"]["keywords"]["description"] ==
          "函数名、类名、命令等精确检索词，最多 16 项");
    CHECK(mss["properties"]["paths"]["description"] ==
          "支撑事实的项目内相对路径，最多 24 项；fact 必填至少一项");
    CHECK(mss["properties"]["confidence"]["description"] ==
          "user-stated=用户明说的偏好；verified=已核验的事实；"
          "inferred=推断(只该出现在待审候选，不该走本工具)");
    CHECK(mss["properties"]["scope"]["description"] == "可选。当前工作目录不在范围内时不注入，防串味");
    CHECK(mss["properties"]["scope"]["properties"]["kind"]["description"] ==
          "记忆适用的范围；subtree/path 须配 value；"
          "user=跨项目用户记忆(仅 preference/feedback，"
          "不得带项目路径证据，须全局授权 memory.user_enabled)");
    CHECK(mss["properties"]["scope"]["properties"]["value"]["description"] ==
          "项目内相对路径(subtree/path 时必填)");
    CHECK(mss["properties"]["evidence"]["description"] == "可选。可核验证据，最多 24 项；fact 建议给出");
    CHECK(mss["properties"]["evidence"]["items"]["properties"]["path"]["description"] == "项目内相对路径");
    CHECK(mss["properties"]["evidence"]["items"]["properties"]["symbol"]["description"] ==
          "可选:函数/类/配置键");
    CHECK(mss["properties"]["expires_at"]["description"] ==
          "可选。临时规约的到期日(YYYY-MM-DD 或 ISO 时间);到期后不再召回");
    CHECK(mss["required"] == nlohmann::json::array({"kind", "title", "summary", "content"}));

    lubancode::ptc::PtcTool ptc(clearance_registry, nullptr, lubancode::ptc::PtcTool::Config{});
    CHECK(ptc.description() == kPtcDescBefore);
}

TEST_CASE("清底批: en 下 description 与参数说明都是英文") {
    LangGuard guard;
    lubancode::cli::SetLanguage("en");

    lubancode::config::SearchConfig search_config;
    search_config.provider = "tavily";
    search_config.api_key = "test";
    lubancode::tools::WebSearchTool web_search(search_config);
    CHECK(web_search.description().find("Web search; returns a numbered list") == 0);
    const nlohmann::json wss = web_search.input_schema();
    CHECK(wss["properties"]["query"]["description"] == "Search keywords or question");
    CHECK(wss["properties"]["count"]["description"].get<std::string>().find(
              "omit for the default 5") != std::string::npos);

    lubancode::tools::WebFetchTool web_fetch;
    CHECK(web_fetch.description().find("Fetch a web page") == 0);
    CHECK(web_fetch.input_schema()["properties"]["url"]["description"].get<std::string>().find(
              "must start with http:// or https://") != std::string::npos);

    lubancode::tools::ToolRegistry clearance_registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    lubancode::tools::ToolSearchTool tool_search(clearance_registry, loaded);
    CHECK(tool_search.description().find("Search deferred-mounted tools by keyword") == 0);
    CHECK(tool_search.input_schema()["properties"]["limit"]["description"].get<std::string>().find(
              "omit for the default 5") != std::string::npos);

    lubancode::tools::SkillTool skill(std::vector<lubancode::tools::SkillMeta>{});
    CHECK(skill.description().find("Load a discovered skill") == 0);
    CHECK(skill.input_schema()["properties"]["name"]["description"].get<std::string>().find(
              "the same name as listed in the system prompt") != std::string::npos);

    lubancode::tools::ListSessionsTool list_sessions(
        [] { return std::vector<lubancode::agent::PeerCard>{}; }, "self");
    CHECK(list_sessions.description().find(
              "List the other Lubancode sessions the current user has open") == 0);

    lubancode::tools::SendSessionMessageTool send_session(
        [] { return std::vector<lubancode::agent::PeerCard>{}; },
        [](const lubancode::agent::PeerCard&, const std::string&) {
            return lubancode::agent::PeerDelivery::Delivered;
        });
    CHECK(send_session.description().find("Hand a plain-text message to another Lubancode session") == 0);
    CHECK(send_session.input_schema()["properties"]["text"]["description"] == "Plain-text body");

    lubancode::cli::WorktreeSession worktree_session;
    lubancode::tools::WorktreeTool worktree(worktree_session, nullptr, nullptr);
    CHECK(worktree.description().find("Work inside an isolated git worktree") == 0);
    CHECK(worktree.input_schema()["properties"]["mode"]["description"].get<std::string>().find(
              "deleted only if clean") != std::string::npos);

    lubancode::memory::Options memory_options;
    lubancode::memory::ProjectIdentity memory_identity;
    auto memory_store = std::make_shared<lubancode::memory::ProjectMemory>(
        memory_identity, std::filesystem::path("D:\\never\\used"), memory_options);
    lubancode::memory::MemorySaveTool memory_save(memory_store);
    CHECK(memory_save.description().find("Queue a small, stable project fact") == 0);
    const nlohmann::json mss = memory_save.input_schema();
    CHECK(mss["properties"]["kind"]["description"].get<std::string>().find(
              "fact=a verifiable project fact") == 0);
    CHECK(mss["properties"]["scope"]["properties"]["kind"]["description"].get<std::string>().find(
              "requires the global authorization memory.user_enabled") != std::string::npos);

    lubancode::ptc::PtcTool ptc(clearance_registry, nullptr, lubancode::ptc::PtcTool::Config{});
    CHECK(ptc.description().find("Orchestrate a Python script to call mounted read-only tools") == 0);
}
