// 依赖守门测试(显示系统剥离单第八步:清编译边界)。
//
// 单子验收原文:"lubancode_engine 与 lubancode_runtime 源码中搜不到
// std::cin/cout/cerr、ANSI、Theme、ReadLine、ChoiceMenu、TranscriptPainter;
// engine/runtime 不 include cli/* 或 frontend/terminal/*"。CMake target 已
// 按 engine/runtime/core/app 拆开,这里把那条 grep 钉成测试:源文件清单
// 从 CMake 的编译数据库(compile_commands.json)或源目录现扫,命中即败。
//
// 名单的裁量(照单子语境,不机械匹配):
//   - std::cin/cout/cerr/clog:代码里出现即违例(注释里的提及放行——名单
//     管的是行为,不是文档);
//   - ReadLine/ChoiceMenu/TranscriptPainter/ReadChoiceMenu:cli 终端件,
//     engine/runtime 不许认;
//   - platform/console_* 是终端原语层(抽象的 ReadConsoleInput/VT 探测,
//     cli 与 app 消费它,单子的 platform 归 engine)——它是"终端能力的
//     平台抽象"不是"画面决策",从名单豁免;
//   - engine 在编的 cli 叶子白名单:i18n(字符串表)/theme(配色值)/
//     worktree(git 房务)/line_editor(纯逻辑编辑核)——四个零标准流,
//     守门测试同钉。runtime 一律不许 include cli/*。
//
// 扫描目录:src/runtime(全部)、engine 的目录(api/agent/tools/config/
// memory/hooks/mcp/lsp/ptc)。platform 只盯 console 之外的文件。新文件进
// 目录自动入册,不靠手工清单。

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
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

// 抹掉注释再匹配(// 行注释与 /* */ 块注释):名单管行为不管文档。
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

// 找到测试可执行文件旁的源码根:编译期由 CMake 注入 LUBANCODE_SOURCE_DIR
// (见 tests/CMakeLists.txt);没有(发行包里没编这只测试的树)就跳过。
std::filesystem::path SourceRoot() {
#ifdef LUBANCODE_SOURCE_DIR
    return std::filesystem::path(LUBANCODE_SOURCE_DIR);
#else
    return std::filesystem::path();
#endif
}

std::vector<std::filesystem::path> CollectSources(const std::vector<std::string>& dirs) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path root = SourceRoot();
    if (root.empty()) {
        return out;
    }
    for (const std::string& dir : dirs) {
        const std::filesystem::path full = root / dir;
        if (!std::filesystem::exists(full)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(full)) {
            if (entry.is_regular_file()) {
                const std::string name = entry.path().filename().string();
                if (name.size() > 4 && (name.substr(name.size() - 4) == ".cpp" ||
                                        name.substr(name.size() - 4) == ".hpp")) {
                    out.push_back(entry.path());
                }
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("守门:engine 与 runtime 源码里搜不到终端件(单子验收原文)") {
    const std::filesystem::path root = SourceRoot();
    if (!std::filesystem::exists(root / "src")) {
        // 开发树之外(便携包)没有源码:这只测试只在源码树里起作用。
        return;
    }

    // runtime 全目录 + engine 的各目录(i18n/theme/worktree/line_editor 四
    // 叶子在 cli/ 目录,由第二组单独盯;platform 的 console_* 是终端原语层,
    // 豁免——见文件头裁量)。sessions/peers/skills 是骨架拆解批七从 agent/
    // 迁出的 engine 域目录,同入册。
    const std::vector<std::filesystem::path> gate_files = CollectSources({
        "src/runtime", "src/api", "src/agent", "src/tools", "src/config", "src/memory",
        "src/hooks", "src/mcp", "src/lsp", "src/ptc", "src/sessions", "src/peers",
        "src/skills",
    });
    REQUIRE_FALSE(gate_files.empty());

    for (const auto& path : gate_files) {
        const std::string code = StripComments(SlurpFile(path));
        // 标准流:cin/cout/cerr/clog 直接命中即违例(std::cout 与 cout 都算,
        // 只认 "std::" 前缀以防误伤自家标识符)。
        CHECK_MESSAGE(code.find("std::cin") == std::string::npos,
                      (path.string() + " 含 std::cin"));
        CHECK_MESSAGE(code.find("std::cout") == std::string::npos,
                      (path.string() + " 含 std::cout"));
        CHECK_MESSAGE(code.find("std::cerr") == std::string::npos,
                      (path.string() + " 含 std::cerr"));
        CHECK_MESSAGE(code.find("std::clog") == std::string::npos,
                      (path.string() + " 含 std::clog"));
        // 终端交互件(名单原文)。
        CHECK_MESSAGE(code.find("ReadLine(") == std::string::npos,
                      (path.string() + " 调 ReadLine"));
        CHECK_MESSAGE(code.find("ReadChoiceMenu") == std::string::npos,
                      (path.string() + " 认 ReadChoiceMenu"));
        CHECK_MESSAGE(code.find("ChoiceMenu") == std::string::npos,
                      (path.string() + " 认 ChoiceMenu"));
        CHECK_MESSAGE(code.find("TranscriptPainter") == std::string::npos,
                      (path.string() + " 认 TranscriptPainter"));
        // include 边界:runtime 一律不许 include cli/*;engine 只许四叶子
        // (i18n/theme/worktree/line_editor),其余 cli 头都是终端件。
        const bool is_runtime = path.string().find("src/runtime") != std::string::npos;
        const std::size_t cli_include = code.find("#include \"cli/");
        if (cli_include != std::string::npos) {
            // 找出全部 cli include,逐个对白名单。
            std::size_t pos = cli_include;
            while (pos != std::string::npos) {
                const std::size_t start = pos + std::string("#include \"cli/").size();
                const std::size_t end = code.find('"', start);
                const std::string header = code.substr(start, end - start);
                const bool leaf = header == "i18n.hpp" || header == "theme.hpp" ||
                                  header == "worktree.hpp" || header == "line_editor.hpp";
                CHECK_MESSAGE(!is_runtime, (path.string() + " (runtime) include cli/" + header));
                CHECK_MESSAGE(leaf, (path.string() + " (engine) include 终端件 cli/" + header));
                pos = code.find("#include \"cli/", end);
            }
        }
        // app/frontend:两层都不许。
        CHECK_MESSAGE(code.find("#include \"app/") == std::string::npos,
                      (path.string() + " include app/*(方向反了)"));
    }
}

TEST_CASE("守门:四叶子自身零标准流(i18n/theme/worktree/line_editor)") {
    const std::vector<std::filesystem::path> leaves = [&] {
        std::vector<std::filesystem::path> out;
        const std::filesystem::path root = SourceRoot();
        if (root.empty()) {
            return out;
        }
        for (const std::string& name : {"i18n", "theme", "worktree", "line_editor"}) {
            const std::filesystem::path hpp = root / "src" / "cli" / (name + ".hpp");
            const std::filesystem::path cpp = root / "src" / "cli" / (name + ".cpp");
            if (std::filesystem::exists(hpp)) out.push_back(hpp);
            if (std::filesystem::exists(cpp)) out.push_back(cpp);
        }
        return out;
    }();
    if (leaves.empty()) {
        return;  // 没有源码树(发行包):这只测试不起作用
    }
    for (const auto& path : leaves) {
        const std::string code = StripComments(SlurpFile(path));
        CHECK_MESSAGE(code.find("std::cin") == std::string::npos, (path.string() + " 含 std::cin"));
        CHECK_MESSAGE(code.find("std::cout") == std::string::npos, (path.string() + " 含 std::cout"));
        CHECK_MESSAGE(code.find("std::cerr") == std::string::npos, (path.string() + " 含 std::cerr"));
    }
}

// 骨架拆解反弹·问题 3 的验收线:src/app/wirings/ 是纯装配根,目录下的
// 文件不许有 TermOut()/TermErr()/ReadLine()/ReadChoiceMenu() 这类直接终端
// IO——要说话就产事件/回调,由装配层(interactive_session_wiring)画。
// 名单裁量:plan_session_wiring 与 peer_session_wiring 是 /plan、/peers 的
// 命令交互本体(整改单问题 3 的位置清单不含这两只,另单处理),挂白名单
// 放行;其余文件(含以后新进的)命中即败,防新代码继续往错的地方搬。
TEST_CASE("守门:wirings/ 装配根不做直接终端 IO(整改单问题 3)") {
    const std::filesystem::path wirings = SourceRoot() / "src" / "app" / "wirings";
    if (!std::filesystem::exists(wirings)) {
        return;  // 没有源码树(发行包):这只测试不起作用
    }
    const std::vector<std::string> allowlist = {
        "plan_session_wiring.cpp", "plan_session_wiring.hpp",
        "peer_session_wiring.cpp", "peer_session_wiring.hpp",
    };
    for (const auto& entry : std::filesystem::directory_iterator(wirings)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() < 5 || (name.substr(name.size() - 4) != ".cpp" && name.substr(name.size() - 4) != ".hpp")) {
            continue;
        }
        if (std::find(allowlist.begin(), allowlist.end(), name) != allowlist.end()) {
            continue;
        }
        const std::string code = StripComments(SlurpFile(entry.path()));
        CHECK_MESSAGE(code.find("TermOut(") == std::string::npos,
                      (entry.path().string() + " 调 TermOut(装配根不画终端)"));
        CHECK_MESSAGE(code.find("TermErr(") == std::string::npos,
                      (entry.path().string() + " 调 TermErr(装配根不画终端)"));
        CHECK_MESSAGE(code.find("ReadLine(") == std::string::npos,
                      (entry.path().string() + " 调 ReadLine(装配根不读终端)"));
        CHECK_MESSAGE(code.find("ReadChoiceMenu(") == std::string::npos,
                      (entry.path().string() + " 调 ReadChoiceMenu(装配根不读终端)"));
    }
}
