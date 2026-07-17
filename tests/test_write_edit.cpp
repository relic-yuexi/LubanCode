// write_file:写新文件、自动建父目录、覆盖已有文件时的提示。
// edit_file:唯一命中替换、多处命中报错(报次数)、找不到报错、
//           replace_all 全换、中文内容。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
    CHECK_FALSE(result.content.empty());
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
