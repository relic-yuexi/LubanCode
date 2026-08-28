// 观察边界(真机实测 P2-5,"子代理吞自己日志"):
//   - search 默认不搜边界内文件(.evidence 子树、运行时登记的子代理日志
//     目录);"日志含日志路径"的两层嵌套夹具,递归膨胀场景断言默认不吞。
//   - path 逐字点名(单文件/边界内目录根)才搜得到,单文件 grep 正文前
//     给一行体积提示。
//   - read_file 点名读边界内文件:放行,正文前一行体积提示,超过 256KB
//     劝阻。
//   - ObservationBoundary 登记账:AddExcludedDir/Reset/Contains 与 .evidence
//     名字口径(名字口径不随 Reset 清)。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "tools/observation_filter.hpp"
#include "tools/path_utils.hpp"
#include "tools/read_file.hpp"
#include "tools/search.hpp"

using lubancode::tools::ObservationBoundary;
using lubancode::tools::ObservationReadNotice;
using lubancode::tools::PathInObservationBoundary;
using lubancode::tools::PathToUtf8;
using lubancode::tools::ReadFileTool;
using lubancode::tools::SearchTool;
using lubancode::tools::Tool;
using lubancode::tools::Utf8ToPath;

namespace {

// 系统临时目录下一个独立的子目录,用完即删,给单测隔离用。
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_obstest_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::filesystem::path Path(const std::string& child = "") const {
        return child.empty() ? path_ : path_ / Utf8ToPath(child);
    }

    std::string Utf8Path(const std::string& child = "") const {
        return PathToUtf8(Path(child));
    }

    // 写一个文件,child 是相对本目录的路径(可以带子目录,自动建好)。
    void WriteFile(const std::string& child, const std::string& content) const {
        const std::filesystem::path full = Path(child);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary);
        file << content;
    }

private:
    std::filesystem::path path_;
};

// 每册测试起手清一次进程级登记账,免得别册(或本册上一段)留下的目录
// 串场;.evidence 名字口径是常开的,不受影响。
struct BoundaryResetGuard {
    BoundaryResetGuard() { ObservationBoundary::Instance().Reset(); }
    ~BoundaryResetGuard() { ObservationBoundary::Instance().Reset(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// ObservationBoundary 本体
// ---------------------------------------------------------------------------

TEST_CASE("observation: 名字口径——.evidence 段即边界,Reset 不清名字口径") {
    BoundaryResetGuard guard;
    ObservationBoundary::Instance().Reset();
    TempDir dir;

    CHECK(PathInObservationBoundary(dir.Path(".evidence/subagents/subagent-1.log")));
    CHECK(PathInObservationBoundary(dir.Path(".evidence/notes.md")));
    CHECK_FALSE(PathInObservationBoundary(dir.Path("src/main.cpp")));
    CHECK_FALSE(PathInObservationBoundary(dir.Path("docs/evidence.md")));  // 名字要逐字 .evidence

    // 登记一枚非 .evidence 名字的日志目录:Contains 收;Reset 只清登记账。
    const std::filesystem::path logs = dir.Path("runtime-logs");
    std::filesystem::create_directories(logs);
    ObservationBoundary::Instance().AddExcludedDir(logs);
    CHECK(PathInObservationBoundary(logs / "subagent-9.log"));
    CHECK_FALSE(PathInObservationBoundary(dir.Path("src/main.cpp")));
    ObservationBoundary::Instance().Reset();
    CHECK_FALSE(PathInObservationBoundary(logs / "subagent-9.log"));
    CHECK(PathInObservationBoundary(dir.Path(".evidence/x.log")));  // 名字口径还在
}

TEST_CASE("observation: 提示行——边界内给体积,超 256KB 劝阻,边界外不给") {
    BoundaryResetGuard guard;
    TempDir dir;
    const std::filesystem::path inside = dir.Path(".evidence/subagents/subagent-1.log");
    dir.WriteFile(".evidence/subagents/subagent-1.log", "small");

    CHECK(ObservationReadNotice(dir.Path("src/main.cpp"), 10).empty());
    const std::string small_notice = ObservationReadNotice(inside, 5);
    CHECK(small_notice.find("5 字节") != std::string::npos);
    CHECK(small_notice.find("照常读取") != std::string::npos);
    CHECK(small_notice.find("劝阻") == std::string::npos);

    const std::string big_notice = ObservationReadNotice(inside, 300 * 1024);
    CHECK(big_notice.find("307200 字节") != std::string::npos);
    CHECK(big_notice.find("offset/limit") != std::string::npos);  // 劝阻并指路
}

// ---------------------------------------------------------------------------
// search:默认过滤与显式点名
// ---------------------------------------------------------------------------

TEST_CASE("observation search: 默认不吞 .evidence,普通文件照搜") {
    BoundaryResetGuard guard;
    TempDir dir;
    dir.WriteFile("src/main.cpp", "NEEDLE in source\n");
    dir.WriteFile(".evidence/subagents/subagent-1.log", "NEEDLE in subagent log\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "NEEDLE";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("src/main.cpp") != std::string::npos);
    CHECK(result.content.find("subagent-1.log") == std::string::npos);
}

TEST_CASE("observation search: 递归膨胀夹具——日志含日志路径,两层嵌套默认不吞") {
    BoundaryResetGuard guard;
    TempDir dir;
    // 两层嵌套:外层日志里逐字写着内层日志的路径(真机实测里,日志装着
    // 自己的工具流,工具流里又引用下一次搜索的路径),两层都带钩子词。
    const std::string inner_path = dir.Utf8Path(".evidence/subagents/subagent-2.log");
    const std::string outer_path = dir.Utf8Path(".evidence/subagents/subagent-1.log");
    dir.WriteFile(".evidence/subagents/subagent-1.log",
                  "tool_use search {\"pattern\": \"NEEDLE\"}\nnext: " + inner_path + "\nNEEDLE\n");
    dir.WriteFile(".evidence/subagents/subagent-2.log",
                  "tool_use read_file {\"path\": \"" + outer_path + "\"}\nNEEDLE\n");
    dir.WriteFile("README.md", "NEEDLE in readme\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "NEEDLE";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("README.md") != std::string::npos);
    CHECK(result.content.find("subagent-1.log") == std::string::npos);
    CHECK(result.content.find("subagent-2.log") == std::string::npos);

    // glob 同一口径:默认列不出边界内的文件。
    nlohmann::json glob_input;
    glob_input["mode"] = "glob";
    glob_input["pattern"] = "*.log";
    glob_input["path"] = dir.Utf8Path();
    const Tool::Result glob_result = tool.execute(glob_input);
    CHECK_FALSE(glob_result.is_error);
    CHECK(glob_result.content.find("subagent-1.log") == std::string::npos);
    CHECK(glob_result.content.find("subagent-2.log") == std::string::npos);

    // 显式点名单文件:搜得到,正文前一行体积提示。
    nlohmann::json named;
    named["mode"] = "grep";
    named["pattern"] = "NEEDLE";
    named["path"] = inner_path;
    const Tool::Result named_result = tool.execute(named);
    CHECK_FALSE(named_result.is_error);
    CHECK(named_result.content.find("subagent-2.log:2:NEEDLE") != std::string::npos);
    CHECK(named_result.content.find("观察边界") != std::string::npos);
    CHECK(named_result.content.find("字节") != std::string::npos);

    // 显式点名目录根(根在边界内):照常搜。
    nlohmann::json named_dir;
    named_dir["mode"] = "grep";
    named_dir["pattern"] = "NEEDLE";
    named_dir["path"] = dir.Utf8Path(".evidence/subagents");
    const Tool::Result named_dir_result = tool.execute(named_dir);
    CHECK_FALSE(named_dir_result.is_error);
    CHECK(named_dir_result.content.find("subagent-1.log") != std::string::npos);
    CHECK(named_dir_result.content.find("subagent-2.log") != std::string::npos);
}

TEST_CASE("observation search: 运行时登记的日志目录也默认不吞(不管叫什么名字)") {
    BoundaryResetGuard guard;
    TempDir dir;
    // 复刻真机场景:LUBANCODE_DEBUG_SUBAGENT 指进项目内一个不叫 .evidence
    // 的目录(TraceBackend 开日志时会 AddExcludedDir 登记它)。
    const std::filesystem::path debug_dir = dir.Path("debug-dump/subagents");
    std::filesystem::create_directories(debug_dir);
    ObservationBoundary::Instance().AddExcludedDir(dir.Path("debug-dump"));
    dir.WriteFile("debug-dump/subagents/subagent-1.log", "NEEDLE swallowed own log\n");
    dir.WriteFile("app.js", "NEEDLE in app\n");

    SearchTool tool;
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "NEEDLE";
    input["path"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("app.js") != std::string::npos);
    CHECK(result.content.find("subagent-1.log") == std::string::npos);

    // 点名单文件:放行并带提示。
    nlohmann::json named;
    named["mode"] = "grep";
    named["pattern"] = "NEEDLE";
    named["path"] = PathToUtf8(debug_dir / "subagent-1.log");
    const Tool::Result named_result = tool.execute(named);
    CHECK_FALSE(named_result.is_error);
    CHECK(named_result.content.find("NEEDLE swallowed") != std::string::npos);
    CHECK(named_result.content.find("观察边界") != std::string::npos);
}

// ---------------------------------------------------------------------------
// read_file:点名放行 + 体积提示
// ---------------------------------------------------------------------------

TEST_CASE("observation read_file: 点名边界内文件放行,正文前一行体积提示") {
    BoundaryResetGuard guard;
    TempDir dir;
    dir.WriteFile(".evidence/subagents/subagent-1.log", "line one\nline two\n");

    ReadFileTool tool;
    nlohmann::json input;
    input["path"] = dir.Utf8Path(".evidence/subagents/subagent-1.log");
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    // 提示在最前,一行;正文照常带行号。
    CHECK(result.content.rfind("[观察边界] ", 0) == 0);
    const std::size_t first_newline = result.content.find('\n');
    CHECK(result.content.substr(0, first_newline).find("18 字节") != std::string::npos);
    CHECK(result.content.find("     2\tline two") != std::string::npos);

    // 边界外文件没有提示。
    dir.WriteFile("plain.txt", "hello\n");
    nlohmann::json plain;
    plain["path"] = dir.Utf8Path("plain.txt");
    const Tool::Result plain_result = tool.execute(plain);
    CHECK_FALSE(plain_result.is_error);
    CHECK(plain_result.content.find("观察边界") == std::string::npos);
}

TEST_CASE("observation read_file: 超过 256KB 的观察记录,提示行劝阻并指路分段") {
    BoundaryResetGuard guard;
    TempDir dir;
    std::string big;
    big.reserve(300 * 1024);
    while (big.size() < 300 * 1024) {
        big += "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n";
    }
    dir.WriteFile(".evidence/subagents/subagent-1.log", big);

    ReadFileTool tool;
    nlohmann::json input;
    input["path"] = dir.Utf8Path(".evidence/subagents/subagent-1.log");
    input["limit"] = 3;  // 少读几行,断言聚焦提示行
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    const std::size_t first_newline = result.content.find('\n');
    const std::string notice = result.content.substr(0, first_newline);
    CHECK(notice.find("观察边界") != std::string::npos);
    CHECK(notice.find("offset/limit") != std::string::npos);
    CHECK(notice.find("307") != std::string::npos);  // 300KB+ 的字节数
    CHECK(notice.find("照常读取") == std::string::npos);
}
