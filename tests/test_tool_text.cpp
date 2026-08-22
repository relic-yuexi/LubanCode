// 工具文案按语言分档(模型可见文字抽离 C++ 的基建批):
//   ToolText 三级回退:当前语言档 → zh-CN 档 → 兜底字串;
//   缺省(zh-CN)下试点工具的 description / schema 参数说明与迁移前
//   逐字节一致(零行为变化);
//   LUBANCODE_LANG=en 走 cli::SetLanguage("en") 同一条选择链,出英文。
//
// 注意:语言是进程级全局状态,LangGuard 兜底还原(同 test_i18n.cpp 的
// 规矩),免得串到别的测试文件头上。

#include <doctest/doctest.h>

#include <string>

#include "cli/i18n.hpp"
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/tool_text.hpp"
#include "tools/write_file.hpp"

using lubancode::tools::BackgroundOutputTool;
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
// 文件工具批余量(edit_file/search)与命令族批(run_command/
// background_output/stop_background)的迁移断言。改前原文逐字节从 cpp 字面量
// 抄出,与 zh-CN 档(缺省)逐字节钉死;en 档抽特征词验生效。run_command 的
// 平台分档文案(description 与 command/shell 两参数)按编译平台走对应的键,
// POSIX 节只在非 Windows 平台可查。
// ---------------------------------------------------------------------------

namespace {

const char* kEditFileDescBefore =
    "对已有文件做字符串替换:先精确匹配,失败后会有限兼容 CRLF/LF、统一缩进和行尾空白。"
    "old_string 仍须唯一出现(除非把 replace_all 设成 true),多处候选绝不猜。"
    "适合小范围、精准的改动,不适合整篇重写(整篇重写用 write_file)。执行前需要用户确认。";

const char* kSearchDescBefore =
    "在目录里搜索,两种模式:mode=\"grep\" 按正则(ECMAScript 语法)搜文件内容,"
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
    CHECK(ss["properties"]["path"]["description"] == "从哪个目录开始搜,不填默认当前工作目录");
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
    CHECK(search.description().find("Search inside a directory") == 0);
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
