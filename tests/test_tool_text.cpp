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
#include "tools/read_file.hpp"
#include "tools/tool_text.hpp"
#include "tools/write_file.hpp"

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
