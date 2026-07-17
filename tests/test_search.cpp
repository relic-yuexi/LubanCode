// search 工具:grep 模式命中带行号、glob 过滤、上限截断注明、跳过
// build/.git、二进制跳过、中文内容命中;glob 模式按文件名找文件。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "tools/path_utils.hpp"
#include "tools/search.hpp"

using lubancode::tools::PathToUtf8;
using lubancode::tools::SearchTool;
using lubancode::tools::Tool;
using lubancode::tools::Utf8ToPath;

namespace {

// 系统临时目录下一个独立的子目录,用完即删,给单测隔离用。
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_searchtest_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

    // 写一个文件,child 是相对本目录的路径(可以带子目录,自动建好)。
    void WriteFile(const std::string& child, const std::string& content) const {
        const std::filesystem::path full = path_ / Utf8ToPath(child);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary);
        file << content;
    }

    void WriteBinaryFile(const std::string& child) const {
        const std::filesystem::path full = path_ / Utf8ToPath(child);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary);
        char data[] = {'B', 'I', 'N', '\0', 'A', 'R', 'Y'};
        file.write(data, sizeof(data));
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("search grep: 命中带文件名、行号") {
    TempDir dir;
    dir.WriteFile("a.txt", "line one\nneedle here\nline three\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "needle";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("a.txt:2:needle here") != std::string::npos);
}

TEST_CASE("search grep: glob 过滤只搜指定扩展名的文件") {
    TempDir dir;
    dir.WriteFile("keep.cpp", "target_word here\n");
    dir.WriteFile("skip.txt", "target_word here too\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "target_word";
    input["path"] = dir.Utf8Path();
    input["glob"] = "*.cpp";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("keep.cpp") != std::string::npos);
    CHECK(result.content.find("skip.txt") == std::string::npos);
}

TEST_CASE("search grep: 命中超过上限,截断并注明") {
    TempDir dir;
    std::string content;
    for (int i = 0; i < 150; ++i) {
        content += "hit_line\n";
    }
    dir.WriteFile("many.txt", content);

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "hit_line";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    // 100 条上限,超过要注明截断
    int count = 0;
    std::size_t pos = 0;
    while ((pos = result.content.find("hit_line", pos)) != std::string::npos) {
        ++count;
        pos += 8;
    }
    CHECK(count == 100);
    CHECK(result.content.find("截断") != std::string::npos);
}

TEST_CASE("search grep: 跳过 build/ 和 .git/ 目录") {
    TempDir dir;
    dir.WriteFile("build/generated.txt", "secret_marker\n");
    dir.WriteFile(".git/objects/whatever", "secret_marker\n");
    dir.WriteFile("real.txt", "secret_marker\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "secret_marker";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("real.txt") != std::string::npos);
    CHECK(result.content.find("build") == std::string::npos);
    CHECK(result.content.find(".git") == std::string::npos);
}

TEST_CASE("search grep: 二进制文件被跳过,不报错也不崩") {
    TempDir dir;
    dir.WriteBinaryFile("blob.bin");
    dir.WriteFile("text.txt", "normal text\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "BIN";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("blob.bin") == std::string::npos);
}

TEST_CASE("search grep: 中文内容命中") {
    TempDir dir;
    dir.WriteFile("chinese.txt", "第一行\n这里有关键词\n第三行\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "关键词";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("chinese.txt:2:这里有关键词") != std::string::npos);
}

TEST_CASE("search glob: 按文件名通配找文件") {
    TempDir dir;
    dir.WriteFile("foo.cpp", "");
    dir.WriteFile("bar.hpp", "");
    dir.WriteFile("sub/baz.cpp", "");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "glob";
    input["pattern"] = "*.cpp";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("foo.cpp") != std::string::npos);
    CHECK(result.content.find("baz.cpp") != std::string::npos);
    CHECK(result.content.find("bar.hpp") == std::string::npos);
}

TEST_CASE("search: mode 不合法,报错不崩") {
    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "nonsense";
    input["pattern"] = "x";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
}

TEST_CASE("search: 缺少必填参数,报错不崩") {
    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
}
