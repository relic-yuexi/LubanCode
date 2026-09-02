// 存储 v2 P0-6 静态审计守门(单子第 7 条):源码与文档里,除迁移说明与
// 迁移器本体,不得再出现旧格式的生产路径字样——
//   ~/.lubancode/sessions、~/.lubancode/trajectories、~/.lubancode/projects
// 一旦有新代码把旧目录写回生产路(配置默认值、目录扫描、错误提示指路),
// 这里当场红。
//
// 白名单(合法持有旧字样):
//   - src/workspace/storage_migrator.*      迁移器主体(旧格式解析只活在
//     它内部,单子 §7.4 隔离边界);
//   - tools/legacy-storage-migrator/*       独立封存体;
//   - docs/getting-started/storage-migration.md  迁移说明(告诉用户旧数据
//     在哪、怎么迁,这页就是干这个的);
//   - docs/development/workspace-storage-v2/*    开发文档(设计/合同的
//     历史叙述);
//   - 本测试册自身。
//
// 扫描目录:src/(生产代码)与 docs/(用户/开发文档)。测试目录不扫——
// 旧档夹具(tests/fixtures/workspace/legacy)与迁移器测试册是迁移器的
// 验收输入,不是生产路径。
//
// 口径与 test_app_boundary_gate 同款:源码先剥注释再匹配(名单管行为
// 不管文档);文档整文匹配(文档本身就是文案)。

#include <doctest/doctest.h>

#include <algorithm>
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

// 抹掉 // 行注释与 /* */ 块注释(源码册用;文档册整文匹配)。
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

std::filesystem::path SourceRoot() {
#ifdef LUBANCODE_SOURCE_DIR
    return std::filesystem::path(LUBANCODE_SOURCE_DIR);
#else
    return std::filesystem::path();
#endif
}

// 相对路径(通用分隔符)是否命中白名单前缀。
bool Whitelisted(const std::string& relative) {
    static const char* kPrefixes[] = {
        "src/workspace/storage_migrator.",
        "tools/legacy-storage-migrator/",
        "docs/getting-started/storage-migration.md",
        "docs/development/workspace-storage-v2/",
        "tests/unit/workspace/test_legacy_storage_gate.cpp",
    };
    for (const char* prefix : kPrefixes) {
        if (relative.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

struct Hit {
    std::string file;
    int line = 0;
    std::string text;
};

// 扫一个目录树;strip_comments 控制源码/文档两种口径。
std::vector<Hit> ScanTree(const std::string& tree, bool strip_comments) {
    std::vector<Hit> hits;
    const std::filesystem::path root = SourceRoot();
    if (root.empty()) {
        return hits;
    }
    const std::filesystem::path full = root / tree;
    if (!std::filesystem::exists(full)) {
        return hits;
    }
    static const char* kNeedles[] = {
        ".lubancode/sessions",
        ".lubancode/trajectories",
        ".lubancode/projects",
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(full)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".md" && ext != ".cmake" &&
            ext != ".txt") {
            continue;
        }
        const std::string relative =
            entry.path().lexically_relative(root).generic_string();
        if (Whitelisted(relative)) {
            continue;
        }
        const std::string content = strip_comments ? StripComments(SlurpFile(entry.path()))
                                                   : SlurpFile(entry.path());
        // 逐行定位,报文给行号。
        std::size_t line = 1;
        std::size_t begin = 0;
        while (begin <= content.size()) {
            const std::size_t end = content.find('\n', begin);
            const std::string_view line_text(
                content.data() + begin,
                (end == std::string::npos ? content.size() : end) - begin);
            for (const char* needle : kNeedles) {
                if (line_text.find(needle) != std::string_view::npos) {
                    Hit hit;
                    hit.file = relative;
                    hit.line = static_cast<int>(line);
                    hit.text = std::string(line_text.substr(0, 120));
                    hits.push_back(std::move(hit));
                }
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
            ++line;
        }
    }
    return hits;
}

}  // namespace

TEST_CASE("P0-6 静态审计:src/ 生产代码不再持有旧目录路径(白名单外)") {
    const auto hits = ScanTree("src", /*strip_comments=*/true);
    for (const Hit& hit : hits) {
        std::printf("  [legacy-path] %s:%d: %s\n", hit.file.c_str(), hit.line, hit.text.c_str());
    }
    CHECK(hits.empty());
}

TEST_CASE("P0-6 静态审计:docs/ 不再持有旧目录路径(迁移说明与开发文档外)") {
    const auto hits = ScanTree("docs", /*strip_comments=*/false);
    for (const Hit& hit : hits) {
        std::printf("  [legacy-path] %s:%d: %s\n", hit.file.c_str(), hit.line, hit.text.c_str());
    }
    CHECK(hits.empty());
}

TEST_CASE("P0-6 静态审计:旧平铺件源文件确已删除") {
    const std::filesystem::path root = SourceRoot();
    CHECK_FALSE(std::filesystem::exists(root / "src" / "sessions" / "session_store.cpp"));
    CHECK_FALSE(std::filesystem::exists(root / "src" / "sessions" / "session_store.hpp"));
    CHECK_FALSE(std::filesystem::exists(root / "src" / "sessions" / "session_catalog.cpp"));
    CHECK_FALSE(std::filesystem::exists(root / "src" / "sessions" / "session_lifecycle.cpp"));
    // goal 事件纯函数留任(goal_coordinator/adapters 仍消费)。
    CHECK(std::filesystem::exists(root / "src" / "sessions" / "goal_session.cpp"));
}
