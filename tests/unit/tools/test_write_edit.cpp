// write_file:写新文件、自动建父目录、覆盖已有文件时的提示。
//           崩溃级原子(临时文件写全 + 原子换名):写全无残尸、失败
//           不留零 byte 残尸、陈年临时件不碍写不被收走。
// edit_file:唯一命中替换、多处命中报错(报次数)、找不到报错、
//           replace_all 全换、中文内容。

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "tools/edit_file.hpp"
#include "tools/path_utils.hpp"
#include "tools/write_file.hpp"

using lubancode::tools::EditFileTool;
using lubancode::tools::PathToUtf8;
using lubancode::tools::Tool;
using lubancode::tools::Utf8ToPath;
using lubancode::tools::WriteFileTool;

namespace {

// 系统临时目录下一个独立的子目录,用完即删,给单测隔离用。
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_wetest_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string Utf8Path(const std::string& child = "") const {
        std::filesystem::path p = child.empty() ? path_ : path_ / Utf8ToPath(child);
        return PathToUtf8(p);
    }

private:
    std::filesystem::path path_;
};

void WriteFileRaw(const std::string& utf8_path, const std::string& content) {
    std::ofstream file(Utf8ToPath(utf8_path), std::ios::binary);
    file << content;
}

std::string ReadFileRaw(const std::string& utf8_path) {
    std::ifstream file(Utf8ToPath(utf8_path), std::ios::binary);
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

// 目录里所有带 .tmp 尾巴的文件名——write_file 原子写临时件的形状
//(<目标>.<pid>-<序号>.tmp),成功/失败路径都该清干净。
std::vector<std::string> TempLeftovers(const TempDir& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(Utf8ToPath(dir.Utf8Path()), ec)) {
        const std::string name = PathToUtf8(entry.path().filename());
        if (name.find(".tmp") != std::string::npos) {
            found.push_back(name);
        }
    }
    return found;
}

}  // namespace

TEST_CASE("write_file: 写一个全新文件") {
    TempDir dir;
    WriteFileTool tool;

    const std::string path = dir.Utf8Path("hello.txt");
    nlohmann::json input;
    input["path"] = path;
    input["content"] = "你好鲁班";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("覆盖") == std::string::npos);
    CHECK(ReadFileRaw(path) == "你好鲁班");
}

TEST_CASE("write_file: 父目录不存在会自动建好") {
    TempDir dir;
    WriteFileTool tool;

    const std::string path = dir.Utf8Path("a/b/c/deep.txt");
    nlohmann::json input;
    input["path"] = path;
    input["content"] = "深层文件";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(ReadFileRaw(path) == "深层文件");
}

TEST_CASE("write_file: 覆盖已有文件会在结果里提示") {
    TempDir dir;
    WriteFileTool tool;
    const std::string path = dir.Utf8Path("exist.txt");

    nlohmann::json first;
    first["path"] = path;
    first["content"] = "第一版";
    const Tool::Result first_result = tool.execute(first);
    CHECK_FALSE(first_result.is_error);
    CHECK(first_result.content.find("覆盖") == std::string::npos);

    nlohmann::json second;
    second["path"] = path;
    second["content"] = "第二版";
    const Tool::Result second_result = tool.execute(second);
    CHECK_FALSE(second_result.is_error);
    CHECK(second_result.content.find("覆盖了原有文件") != std::string::npos);
    CHECK(ReadFileRaw(path) == "第二版");
}

TEST_CASE("write_file: 缺少必填参数,报错不崩") {
    WriteFileTool tool;
    nlohmann::json input;
    input["path"] = "whatever.txt";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
}

TEST_CASE("edit_file: 唯一命中,替换成功") {
    TempDir dir;
    const std::string path = dir.Utf8Path("edit1.txt");
    WriteFileRaw(path, "hello world");

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "world";
    input["new_string"] = "lubancode";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("1") != std::string::npos);
    CHECK(ReadFileRaw(path) == "hello lubancode");
}

TEST_CASE("edit_file: 多处命中且未开 replace_all,报错并说明次数") {
    TempDir dir;
    const std::string path = dir.Utf8Path("edit2.txt");
    WriteFileRaw(path, "foo foo foo");

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "foo";
    input["new_string"] = "bar";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("3") != std::string::npos);
    // 文件应该没被改动
    CHECK(ReadFileRaw(path) == "foo foo foo");
}

TEST_CASE("edit_file: 找不到 old_string,报错") {
    TempDir dir;
    const std::string path = dir.Utf8Path("edit3.txt");
    WriteFileRaw(path, "content without target");

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "找不到的字符串";
    input["new_string"] = "无所谓";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("read_file") != std::string::npos);
    CHECK(result.content.find("不要原样重复调用") != std::string::npos);
}

TEST_CASE("edit_file: CRLF 文件可用 LF old_string 命中,并保持 CRLF") {
    TempDir dir;
    const std::string path = dir.Utf8Path("crlf.txt");
    WriteFileRaw(path, "alpha\r\nbeta\r\ngamma\r\n");

    EditFileTool tool;
    nlohmann::json input{{"path", path}, {"old_string", "alpha\nbeta"}, {"new_string", "one\ntwo"}};
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("换行归一") != std::string::npos);
    CHECK(ReadFileRaw(path) == "one\r\ntwo\r\ngamma\r\n");
}

TEST_CASE("edit_file: 统一缩进和行尾空白不同仍可唯一命中") {
    TempDir dir;
    const std::string path = dir.Utf8Path("indent.txt");
    WriteFileRaw(path, "void f() {\n    if (ready) {   \n        run();\t\n    }\n}\n");

    EditFileTool tool;
    nlohmann::json input{{"path", path},
                         {"old_string", "if (ready) {\n    run();\n}"},
                         {"new_string", "if (ready) {\n    finish();\n}"}};
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("缩进/行尾空白归一") != std::string::npos);
    CHECK(ReadFileRaw(path) == "void f() {\n    if (ready) {\n        finish();\n    }\n}\n");
}

TEST_CASE("edit_file: 宽松匹配多处时拒绝猜测并报起始行") {
    TempDir dir;
    const std::string path = dir.Utf8Path("ambiguous.txt");
    WriteFileRaw(path, "  call();  \n  next();\n\n    call();\t\n    next();\n");

    EditFileTool tool;
    nlohmann::json input{{"path", path},
                         {"old_string", "call();\nnext();"},
                         {"new_string", "done();\nnext();"}};
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("2 处") != std::string::npos);
    CHECK(result.content.find("起始行") != std::string::npos);
    CHECK(ReadFileRaw(path) == "  call();  \n  next();\n\n    call();\t\n    next();\n");
}

TEST_CASE("edit_file: replace_all 全部替换") {
    TempDir dir;
    const std::string path = dir.Utf8Path("edit4.txt");
    WriteFileRaw(path, "foo foo foo");

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "foo";
    input["new_string"] = "bar";
    input["replace_all"] = true;
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("3") != std::string::npos);
    CHECK(ReadFileRaw(path) == "bar bar bar");
}

TEST_CASE("edit_file: 中文内容替换") {
    TempDir dir;
    const std::string path = dir.Utf8Path("edit5.txt");
    WriteFileRaw(path, "你好鲁班,鲁班很棒");

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "鲁班";
    input["new_string"] = "匠祖";
    input["replace_all"] = true;
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(ReadFileRaw(path) == "你好匠祖,匠祖很棒");
}

TEST_CASE("edit_file: 文件不存在,报错不崩") {
    EditFileTool tool;
    nlohmann::json input;
    input["path"] = "D:/lubancode/这个文件肯定不存在_edit_xyz.txt";
    input["old_string"] = "a";
    input["new_string"] = "b";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
}

TEST_CASE("edit_file: replace_all 传成字符串,返回 is_error,不抛异常") {
    TempDir dir;
    const std::string path = dir.Utf8Path("replall.txt");
    {
        std::ofstream f(lubancode::tools::Utf8ToPath(path), std::ios::binary);
        f << "aaa";
    }

    EditFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    input["old_string"] = "a";
    input["new_string"] = "b";
    input["replace_all"] = "true";  // 该是布尔,给成字符串
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
    CHECK(result.content.find("replace_all") != std::string::npos);
}

TEST_CASE("write_file 不观测取消旗:cancel 升着也整篇写完(空文件假设的钉子)") {
    // 主会话输出预留占坑单 §五 P3 的验证结论钉在这:取消是协作式的,只在
    // 轮/工具边界被观察,write_file 的工具体不读 ToolExecutionContext::
    // cancel——ESC 掐不进"建文件"与"写内容"之间,不存在"取消窗口夹出空
    // 文件"的通路(事故现场 write_file 压根没跑,轨迹零工具事件)。另:
    // 崩溃级原子(临时文件写全 + 原子换名)已由 write_file 原子写单修好,
    // 见下方原子落盘三景;取消盲这枚钉子照旧钉着。
    TempDir dir;
    WriteFileTool tool;
    // 走基类引用调两参 execute(派生类的单参声明会把基类重载藏起来):
    // 与 loop 的 RunOneTool 同一条路,cancel 旗随上下文递到工具门口。
    lubancode::tools::Tool& tool_face = tool;
    std::atomic<bool> cancel{true};  // 旗预先升着:工具体照样整篇落盘
    nlohmann::json input;
    input["path"] = dir.Utf8Path("todo_pin.md");
    const std::string body = "# 设计单" + std::string(1, '\n') + "正文一枚,足够长以示完整。" + std::string(1, '\n');
    input["content"] = body;
    const Tool::Result result = tool_face.execute(input, lubancode::tools::ToolExecutionContext{&cancel, ""});
    CHECK_FALSE(result.is_error);
    std::ifstream in(Utf8ToPath(dir.Utf8Path("todo_pin.md")), std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    CHECK(buffer.str() == body);
}

TEST_CASE("write_file: 原子落盘——写全换名,目录里无临时件残尸") {
    // write_file 原子写单的三态验收之"新文完整"腿:覆盖路与新建路都
    // 走"临时文件写全 → 原子换名",落成后目录里不该有任何 .tmp 尾巴。
    TempDir dir;
    WriteFileTool tool;
    const std::string old_body(2048, 'o');
    const std::string new_body(4096, 'n');  // 过一页的整份,半截一眼可辨
    const std::string path = dir.Utf8Path("atomic.txt");

    WriteFileRaw(path, old_body);
    nlohmann::json input;
    input["path"] = path;
    input["content"] = new_body;
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(ReadFileRaw(path) == new_body);
    CHECK(TempLeftovers(dir).empty());

    const std::string fresh = dir.Utf8Path("fresh_atomic.txt");
    nlohmann::json fresh_input;
    fresh_input["path"] = fresh;
    fresh_input["content"] = new_body;
    const Tool::Result fresh_result = tool.execute(fresh_input);
    CHECK_FALSE(fresh_result.is_error);
    CHECK(ReadFileRaw(fresh) == new_body);
    CHECK(TempLeftovers(dir).empty());
}

TEST_CASE("write_file: 临时文件打不开(超长文件名)——明确报错,目标不留残尸") {
    // 三态验收之"明确错误"腿。故障注入沿用 atomic_write 册的最低成本
    // 手法:文件名分量超长(300 字符),两平台 fopen 都开不动临时件
    // (踩 atomic.tmp_open_failed 格)。老 ofstream 时代这句就是"打不开
    // 文件写";换原子写后口径照旧,外加两条新保证:目标压根不出现
    // (新建不留零字节残尸——事故现场那只空 todo 文件的形状),目录里
    // 没有临时尾巴。
    TempDir dir;
    WriteFileTool tool;
    const std::string long_name(300, 'n');
    const std::string path = dir.Utf8Path(long_name);
    nlohmann::json input;
    input["path"] = path;
    input["content"] = "写不进去的正文";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("打不开文件写") != std::string::npos);
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(Utf8ToPath(path), ec));
    CHECK(TempLeftovers(dir).empty());
}

TEST_CASE("write_file: 陈年临时件不碍写,也不被越权收走") {
    // 孤儿规矩:模拟硬崩孤儿(同目录同前缀的 tmp 尾巴)留在现场——后续
    // 写入照常落成;无主之物本工具不动,后缀可辨、无害,不替人扫尾。
    TempDir dir;
    WriteFileTool tool;
    const std::string path = dir.Utf8Path("has_orphan.txt");
    const std::string orphan1 = dir.Utf8Path("has_orphan.txt.tmp");
    const std::string orphan2 = dir.Utf8Path("has_orphan.txt.99999-42.tmp");
    WriteFileRaw(orphan1, "stale");
    WriteFileRaw(orphan2, "stale-too");

    nlohmann::json input;
    input["path"] = path;
    input["content"] = "新正文整份";
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(ReadFileRaw(path) == "新正文整份");
    CHECK(ReadFileRaw(orphan1) == "stale");
    CHECK(ReadFileRaw(orphan2) == "stale-too");
    CHECK(TempLeftovers(dir).size() == 2);
}

#ifdef _WIN32
TEST_CASE("write_file: 目标被占换名不成——旧文完好,无临时尾巴(Windows 专属)") {
    // 三态验收的"旧文完好"腿。注入:目标被另一句柄占着(MSVC 文件流默
    // 认不带 FILE_SHARE_DELETE),MoveFileExW 换不上去,踩 atomic.
    // replace_failed 格。断言:报错点明"原文件保持原样",旧文一个字节
    // 没动,临时件删净。POSIX rename 不看目标句柄,同一景在那两平台是
    // 合法成功,不在此测。
    TempDir dir;
    WriteFileTool tool;
    const std::string path = dir.Utf8Path("occupied.txt");
    const std::string old_body = "旧文整份,一个字都不能少";
    WriteFileRaw(path, old_body);
    std::ifstream holder(Utf8ToPath(path), std::ios::binary);
    REQUIRE(holder.is_open());

    nlohmann::json input;
    input["path"] = path;
    input["content"] = "新文想换上来";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("原文件保持原样") != std::string::npos);
    holder.close();
    CHECK(ReadFileRaw(path) == old_body);
    CHECK(TempLeftovers(dir).empty());
}
#endif
