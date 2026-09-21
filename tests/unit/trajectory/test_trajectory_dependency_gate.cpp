// FD-06 依赖方向守门:trajectory 不许 include insights。
//
// 病(架构审查 2026-09-21 FD-06):trajectory/export_projection.hpp 反向
// include insights/redaction.hpp 调 ScanSecrets,insights↔trajectory 成
// 环——轨迹导出本该给分析层供事实,却背上了洞察实现;改报告字段白名单
// 也会触碰轨迹导出的依赖对象。
//
// 修法:共享密钥扫描下沉 privacy/secret_scan 中立件(insights/telemetry/
// trajectory 三家共用)。这道门把"环不许回来"钉成测试:src/trajectory
// 下任何源文件出现 #include "insights/ 即败。insights 反向消费
// trajectory(分析层吃轨迹事实)是正路,不管。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string SlurpFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 抹掉注释再匹配(// 行注释与 /* */ 块注释):门管行为不管文档——注释里
// 提"原 insights 模式表"的搬家史不算违例。
std::string StripComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (in_line_comment) {
            if (source[i] == '\n') {
                in_line_comment = false;
                out += '\n';
            }
            continue;
        }
        if (in_block_comment) {
            if (i + 1 < source.size() && source[i] == '*' && source[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        out += source[i];
    }
    return out;
}

// 编译期由 CMake 注入源码根(见 tests/CMakeLists.txt);发行包里没有
// 源码树就跳过。
std::filesystem::path SourceRoot() {
#ifdef LUBANCODE_SOURCE_DIR
    return std::filesystem::path(LUBANCODE_SOURCE_DIR);
#else
    return std::filesystem::path();
#endif
}

}  // namespace

TEST_CASE("守门:trajectory 源码不再 include insights(FD-06 拆环)") {
    const std::filesystem::path trajectory = SourceRoot() / "src" / "trajectory";
    if (!std::filesystem::exists(trajectory)) {
        return;  // 没有源码树(发行包):这只测试只在源码树里起作用
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(trajectory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && (name.substr(name.size() - 4) == ".cpp" ||
                                name.substr(name.size() - 4) == ".hpp")) {
            files.push_back(entry.path());
        }
    }
    REQUIRE_FALSE(files.empty());
    for (const auto& path : files) {
        const std::string code = StripComments(SlurpFile(path));
        CHECK_MESSAGE(code.find("#include \"insights/") == std::string::npos,
                      (path.string() + " include insights/*(方向反了,共享件走 privacy/)"));
    }
}
