// AGENTS.md 作用域单 P0/P1 的 Resolver 册:结构化解析(文档/链/指纹/诊断)
// 与旧字符串 loader 的零退化投影。钉的账:
//   1. ResolveForPath 机械顺序 root -> target parent,嵌套 AGENTS.md 从
//      仓库根也能解析到(单子 §5.1 的病灶);
//   2. 同层 override 压 AGENTS、空文件跳过、空 override 回落(机械表与
//      旧 loader 同一张);
//   3. 指纹按作用域文档内容寻址:同 scope 兄弟文件同指纹,内容一变指纹
//      即变(写前闸的失效机制);
//   4. LoadProjectInstructions 是 ResolveForPath 的逐字节投影(零退化);
//   5. 诊断:空文件与同层遮蔽分账;
//   6. P1 分型:坏 UTF-8 / 读错(read seam 注入)分开报,不装成空文件;
//   7. P1 超限:truncated + dropped_for_budget + over_budget 诊断,不静默;
//   8. P1 缓存:path+size+mtime 快筛,外部编辑下一次 Resolve 即重读;
//   9. P2 fallback 名单 / 全局层 / 迁移提示;
//  10. P1 展示面:FormatInstructionChainLines 的账目表。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "config/project_instructions.hpp"

namespace {

class TempProject {
public:
    TempProject() {
        path = std::filesystem::temp_directory_path() /
               ("lubancode_resolver_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path / ".git");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void Write(const std::filesystem::path& relative, const std::string& content) {
        std::filesystem::create_directories((path / relative).parent_path());
        std::ofstream file(path / relative, std::ios::binary);
        file << content;
    }

    std::filesystem::path path;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("resolver walks root to target parent for nested agents files") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "src rule");
    project.Write("src/parser/AGENTS.md", "parser rule");
    project.Write("src/parser/token.cpp", "int main(){}");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/parser/token.cpp");

    REQUIRE(chain.documents.size() == 3);
    CHECK(chain.project_root == project.path);
    // 机械顺序:root -> target parent,离目标最近的在最后。
    CHECK(chain.documents[0].scope_dir == project.path);
    CHECK(chain.documents[1].scope_dir == project.path / "src");
    CHECK(chain.documents[2].scope_dir == project.path / "src/parser");
    CHECK(chain.documents[0].content == "root rule");
    CHECK(chain.documents[2].content == "parser rule");
    CHECK(chain.sources.size() == 3);
    // 拼接投影:root 在前、nearest 在后(与旧 loader 的次序一致)。
    CHECK(Contains(chain.content, "root rule"));
    CHECK(Contains(chain.content, "parser rule"));
    CHECK(chain.content.find("root rule") < chain.content.find("parser rule"));
    CHECK(!chain.truncated);
}

TEST_CASE("resolver takes the target directory itself when target is a directory") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("pkg/AGENTS.md", "pkg rule");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "pkg");
    REQUIRE(chain.documents.size() == 2);
    CHECK(chain.documents.back().scope_dir == project.path / "pkg");
}

TEST_CASE("same scope siblings share fingerprint; content change invalidates it") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/parser/AGENTS.md", "parser rule");
    project.Write("src/parser/token.cpp", "x");
    project.Write("src/parser/lexer.cpp", "y");
    project.Write("src/net/AGENTS.md", "net rule");
    project.Write("src/net/conn.cpp", "z");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto token_chain = resolver.ResolveForPath(project.path / "src/parser/token.cpp");
    const auto lexer_chain = resolver.ResolveForPath(project.path / "src/parser/lexer.cpp");
    const auto net_chain = resolver.ResolveForPath(project.path / "src/net/conn.cpp");

    // 同 scope 兄弟:同指纹(§7.4 分组依据)。
    CHECK(token_chain.fingerprint == lexer_chain.fingerprint);
    // 不同 scope:不同指纹。
    CHECK(token_chain.fingerprint != net_chain.fingerprint);
    // 无自有 AGENTS.md 的子树:链只剩根那份,与带嵌套层的链不同指纹。
    project.Write("docs/notes.md", "n");
    const auto docs_chain = resolver.ResolveForPath(project.path / "docs/notes.md");
    REQUIRE(docs_chain.documents.size() == 1);  // 只有根
    CHECK(docs_chain.fingerprint != token_chain.fingerprint);
    CHECK(docs_chain.fingerprint != net_chain.fingerprint);

    // 内容一变,指纹即变——旧确认自然作废。
    project.Write("src/parser/AGENTS.md", "parser rule v2");
    const auto changed = resolver.ResolveForPath(project.path / "src/parser/lexer.cpp");
    CHECK(changed.fingerprint != token_chain.fingerprint);
}

TEST_CASE("resolver keeps the same-directory override mechanics and diagnostics") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "shadowed rule");
    project.Write("src/AGENTS.override.md", "near rule");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/deep/file.cpp");
    std::filesystem::create_directories(project.path / "src/deep");

    REQUIRE(chain.documents.size() == 2);
    CHECK(chain.documents[1].is_override);
    CHECK(chain.documents[1].content == "near rule");
    CHECK(!Contains(chain.content, "shadowed rule"));
    // 遮蔽有账。
    bool saw_shadow = false;
    for (const auto& note : chain.diagnostics) {
        if (note.code == "shadowed_same_directory") {
            saw_shadow = true;
        }
    }
    CHECK(saw_shadow);

    // 空 override 回落 AGENTS.md(与旧 loader 同一条机械表)。
    project.Write("other/AGENTS.override.md", "   \n");
    project.Write("other/AGENTS.md", "fallback rule");
    const auto fallback = resolver.ResolveForPath(project.path / "other/x.cpp");
    REQUIRE(fallback.documents.size() == 2);
    CHECK(fallback.documents[1].content == "fallback rule");
    CHECK(!fallback.documents[1].is_override);
    bool saw_empty = false;
    for (const auto& note : fallback.diagnostics) {
        if (note.code == "empty_skipped") {
            saw_empty = true;
        }
    }
    CHECK(saw_empty);

    // 空文件跳过:只有空 AGENTS.md 的层不出文档。
    project.Write("hollow/AGENTS.md", "  \n");
    const auto hollow = resolver.ResolveForPath(project.path / "hollow/x.cpp");
    REQUIRE(hollow.documents.size() == 1);  // 只剩根
    CHECK(hollow.documents[0].content == "root rule");
}

TEST_CASE("load project instructions is a byte-identical projection of the resolver") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "src rule");
    project.Write("src/AGENTS.override.md", "near rule");

    // 零退化钉子:同一输入,旧口与 Resolver 投影逐字节一致(帽内与超帽
    // 两种情形都钉)。
    const auto loaded = lubancode::config::LoadProjectInstructions(project.path / "src");
    const auto chain = lubancode::config::ProjectInstructionResolver().ResolveForPath(project.path / "src");
    CHECK(loaded.content == chain.content);
    CHECK(loaded.sources == chain.sources);
    CHECK(loaded.project_root == chain.project_root);
    CHECK(loaded.truncated == chain.truncated);

    // 超帽截断的投影也对齐。
    const auto capped_loaded = lubancode::config::LoadProjectInstructions(project.path / "src", 64);
    const auto capped_chain = lubancode::config::ProjectInstructionResolver(64).ResolveForPath(project.path / "src");
    CHECK(capped_loaded.content == capped_chain.content);
    CHECK(capped_loaded.truncated);
    CHECK(capped_chain.truncated);
}

TEST_CASE("resolver handles targets outside any git repository") {
    // 无 .git 的目录:目标父目录自己当根(与 FindProjectRoot 的 fallback
    // 同一规矩),不往上偷读别人的 AGENTS.md。
    const auto outside = std::filesystem::temp_directory_path() /
                         ("lubancode_resolver_outside_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(outside / "sub");
    {
        std::ofstream file(outside / "sub/AGENTS.md", std::ios::binary);
        file << "orphan rule";
    }
    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(outside / "sub/x.cpp");
    REQUIRE(chain.documents.size() == 1);
    CHECK(chain.documents[0].content == "orphan rule");
    std::error_code ec;
    std::filesystem::remove_all(outside, ec);
}

// ---------------------------------------------------------------------------
// P1-3:分型诊断——坏 UTF-8、读错不再静默降成"没指令"。
// ---------------------------------------------------------------------------

bool HasDiagnostic(const lubancode::config::InstructionChain& chain, const std::string& code) {
    for (const auto& note : chain.diagnostics) {
        if (note.code == code) {
            return true;
        }
    }
    return false;
}

TEST_CASE("invalid utf8 content is rejected with its own diagnostic code") {
    TempProject project;
    project.Write("AGENTS.md", "good rule");
    {
        // 0xFF 是非法 UTF-8 起始字节:内容必须被拒收,不得进提示词。
        std::filesystem::create_directories(project.path / "src");
        std::ofstream file(project.path / "src/AGENTS.md", std::ios::binary);
        file << "\xFF\xFE broken";
    }
    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/x.cpp");
    // 坏文件不参选,层上只剩根那份;但账分明:invalid_utf8 有名有姓。
    REQUIRE(chain.documents.size() == 1);
    CHECK(HasDiagnostic(chain, "invalid_utf8"));
    CHECK(!Contains(chain.content, "broken"));
    // 与"空文件"不同账:empty_skipped 不该出现(文件不空,是坏)。
    CHECK_FALSE(HasDiagnostic(chain, "empty_skipped"));
}

TEST_CASE("read errors are reported separately from empty files via the reader seam") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "src rule");

    // 读 seam 注入失败:src 层读不动。期望:read_error 分账、该层不参选、
    // 不冒充空文件(empty_skipped 不出现)。
    lubancode::config::ProjectInstructionResolverOptions options;
    options.file_reader = [&project](const std::filesystem::path& path) -> std::optional<std::string> {
        if (path == project.path / "src/AGENTS.md") {
            return std::nullopt;  // 模拟权限/短暂 I/O 错
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };
    const lubancode::config::ProjectInstructionResolver resolver(std::move(options));
    const auto chain = resolver.ResolveForPath(project.path / "src/x.cpp");
    REQUIRE(chain.documents.size() == 1);  // 只剩根
    CHECK(HasDiagnostic(chain, "read_error"));
    CHECK_FALSE(HasDiagnostic(chain, "empty_skipped"));
}

// ---------------------------------------------------------------------------
// P1-4:超限分账——截断不再静默。
// ---------------------------------------------------------------------------

TEST_CASE("over-budget projection records truncated, dropped documents and a diagnostic") {
    TempProject project;
    project.Write("AGENTS.md", std::string(400, 'r'));            // 根:中等
    project.Write("mid/AGENTS.md", "mid rule");                   // 中:小
    project.Write("mid/deep/AGENTS.md", std::string(100000, 'd'));  // 近处:超大

    // 帽 1000:根 + mid 装得下,deep 远超余量——deep 被腰斩,后面没有
    // 整份掉落的文档(路径长短不影响这份判定:deep 的 10 万字节远盖过
    // 标题差)。截断、over_budget 诊断都要亮出来。
    const lubancode::config::ProjectInstructionResolver resolver(1000);
    const auto chain = resolver.ResolveForPath(project.path / "mid/deep/x.cpp");
    CHECK(chain.truncated);
    CHECK(HasDiagnostic(chain, "over_budget"));
    CHECK(chain.documents.size() == 3);         // 文档账还是三份(全文无截断)
    CHECK(chain.dropped_for_budget.empty());    // 腰斩的是 deep,没有整份掉落
    CHECK(chain.sources.back() == project.path / "mid/deep/AGENTS.md");
    CHECK(Contains(chain.content, "mid rule"));

    // 另一棵树:根超大,后面两份整份都进不来——记进 dropped_for_budget。
    TempProject topheavy;
    topheavy.Write("AGENTS.md", std::string(5000, 'r'));
    topheavy.Write("mid/AGENTS.md", "mid rule");
    topheavy.Write("mid/deep/AGENTS.md", "deep rule");
    const lubancode::config::ProjectInstructionResolver tight(1000);
    const auto clipped = tight.ResolveForPath(topheavy.path / "mid/deep/x.cpp");
    CHECK(clipped.truncated);
    CHECK(HasDiagnostic(clipped, "over_budget"));
    REQUIRE(clipped.dropped_for_budget.size() == 2);
    CHECK(clipped.dropped_for_budget[0] == topheavy.path / "mid/AGENTS.md");
    CHECK(clipped.dropped_for_budget[1] == topheavy.path / "mid/deep/AGENTS.md");
    CHECK(!Contains(clipped.content, "mid rule"));
    CHECK(!Contains(clipped.content, "deep rule"));

    // /instructions 的账目表:截断状态明说,不冒充"全部已加载"。
    const auto lines = lubancode::config::FormatInstructionChainLines(clipped, 1000);
    bool saw_truncated = false;
    bool saw_complete = false;
    for (const std::string& line : lines) {
        if (line.find("截断") != std::string::npos && line.find("状态") != std::string::npos) {
            saw_truncated = true;
        }
        if (line.find("状态: 完整") != std::string::npos) {
            saw_complete = true;
        }
    }
    CHECK(saw_truncated);
    CHECK_FALSE(saw_complete);
}

// ---------------------------------------------------------------------------
// P1-5/P1-6:缓存 stat 快筛 + 外部编辑的惰性发现。
// ---------------------------------------------------------------------------

TEST_CASE("cache screens by stat and rediscovers external edits on the next resolve") {
    TempProject project;
    project.Write("AGENTS.md", "v1 rule");
    project.Write("src/AGENTS.md", "src v1");

    lubancode::config::ProjectInstructionResolver resolver;
    const auto first = resolver.ResolveForPath(project.path / "src/x.cpp");
    // 成功读过的文档进缓存(根 + src 两份)。
    CHECK(resolver.cached_documents() == 2);
    const auto again = resolver.ResolveForPath(project.path / "src/x.cpp");
    CHECK(again.fingerprint == first.fingerprint);  // stat 未变:同账
    CHECK(resolver.cached_documents() == 2);

    // 外部编辑(P1-6:不常驻监听,下一次 Resolve 的 stat 快筛即发现):
    // mtime/size 变了 → 重读 → 新指纹,旧确认自然作废。
    project.Write("src/AGENTS.md", "src v2 with more words");
    const auto edited = resolver.ResolveForPath(project.path / "src/x.cpp");
    CHECK(edited.fingerprint != first.fingerprint);
    CHECK(edited.documents.back().content == "src v2 with more words");
    CHECK(Contains(edited.content, "src v2 with more words"));
}

// ---------------------------------------------------------------------------
// P2-2:fallback 文件名(显式配置才生效,主名永远优先)。
// ---------------------------------------------------------------------------

TEST_CASE("fallback filenames are consulted only when both primary names miss") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "src primary");
    project.Write("pkg/TEAM.md", "team rule");
    project.Write("pkg/NOTES.md", "notes rule");
    project.Write("pkg2/NOTES.md", "pkg2 notes");

    lubancode::config::ProjectInstructionResolverOptions options;
    options.fallback_filenames = {"TEAM.md", "NOTES.md"};
    const lubancode::config::ProjectInstructionResolver resolver(std::move(options));

    // 有 AGENTS.md 的层:主名优先,fallback 不掺和(TEAM.md 不在本层也没
    // 有诊断)。
    const auto primary = resolver.ResolveForPath(project.path / "src/x.cpp");
    REQUIRE(primary.documents.size() == 2);
    CHECK(primary.documents.back().content == "src primary");
    CHECK(primary.documents.back().is_fallback == false);

    // 主名都没命中的层:名单按序取第一份非空(TEAM.md 压过 NOTES.md)。
    const auto fallback = resolver.ResolveForPath(project.path / "pkg/x.cpp");
    REQUIRE(fallback.documents.size() == 2);
    CHECK(fallback.documents.back().content == "team rule");
    CHECK(fallback.documents.back().is_fallback);
    CHECK(HasDiagnostic(fallback, "fallback_used"));

    // 名单第一份不在、第二份在:取第二份(pkg2 只有 NOTES.md)。
    const auto second = resolver.ResolveForPath(project.path / "pkg2/x.cpp");
    REQUIRE(second.documents.size() == 2);
    CHECK(second.documents.back().content == "pkg2 notes");
    CHECK(second.documents.back().is_fallback);

    // 默认构造(没配名单):fallback 文件视而不见,零退化。
    const lubancode::config::ProjectInstructionResolver plain;
    const auto legacy = plain.ResolveForPath(project.path / "pkg/x.cpp");
    REQUIRE(legacy.documents.size() == 1);  // 只有根
}

// ---------------------------------------------------------------------------
// P2-1:全局层(~/.lubancode/AGENTS.md)——优先级最低,项目层能盖过。
// ---------------------------------------------------------------------------

TEST_CASE("global layer sits below the project root and joins the fingerprint") {
    TempProject project;
    project.Write("global/AGENTS.md", "global working rule");
    project.Write("repo/AGENTS.md", "repo rule");
    project.Write("repo/src/AGENTS.md", "src rule");

    lubancode::config::ProjectInstructionResolverOptions options;
    options.global_instructions_path = project.path / "global/AGENTS.md";

    const lubancode::config::ProjectInstructionResolver with_global(std::move(options));
    const auto chain = with_global.ResolveForPath(project.path / "repo/src/x.cpp");
    REQUIRE(chain.documents.size() == 3);
    CHECK(chain.documents[0].is_global);                    // 垫在最前
    CHECK(chain.documents[0].content == "global working rule");
    CHECK(chain.documents[2].content == "src rule");        // nearest 照旧
    // 投影里全局层在最前(最先拼、最先被帽挤),项目层在后能盖过。
    CHECK(Contains(chain.content, "global working rule"));
    CHECK(chain.content.find("global working rule") < chain.content.find("repo rule"));

    // 指纹认全局层:同一目标,带不带全局层指纹不同(闸会重新握手)。
    const lubancode::config::ProjectInstructionResolver without_global;
    CHECK(without_global.ResolveForPath(project.path / "repo/src/x.cpp").fingerprint != chain.fingerprint);

    // 指向不存在的文件:零层,行为与默认构造一字不差。
    lubancode::config::ProjectInstructionResolverOptions missing;
    missing.global_instructions_path = project.path / "nowhere/AGENTS.md";
    const lubancode::config::ProjectInstructionResolver missing_global(std::move(missing));
    CHECK(missing_global.ResolveForPath(project.path / "repo/src/x.cpp").fingerprint ==
          without_global.ResolveForPath(project.path / "repo/src/x.cpp").fingerprint);
}

// ---------------------------------------------------------------------------
// P2-3:迁移提示——发现别家规则文件只提示,不自动读。
// ---------------------------------------------------------------------------

TEST_CASE("foreign rule files produce migration hints but are never read") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("CLAUDE.md", "claude-only rule");
    project.Write("src/GEMINI.md", "gemini-only rule");
    project.Write("src/AGENT.md", "typo file");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/x.cpp");
    REQUIRE(chain.documents.size() == 1);  // 只有根 AGENTS.md 进链
    CHECK(HasDiagnostic(chain, "migration_hint"));
    CHECK(!Contains(chain.content, "claude-only rule"));
    CHECK(!Contains(chain.content, "gemini-only rule"));

    const auto notes = lubancode::config::FormatInstructionDiagnosticLines(chain);
    bool saw_claude = false;
    for (const std::string& line : notes) {
        if (line.find("CLAUDE.md") != std::string::npos) {
            saw_claude = true;
        }
    }
    CHECK(saw_claude);
}

// ---------------------------------------------------------------------------
// P1-1:展示面——账目表逐 source 亮账,不泄正文。
// ---------------------------------------------------------------------------

TEST_CASE("format chain lines lists every source with kind, bytes and nearest marker") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.override.md", "near override rule");
    std::filesystem::create_directories(project.path / "src/deep");

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/deep/x.cpp");
    const auto lines = lubancode::config::FormatInstructionChainLines(chain, resolver.max_bytes());

    std::string joined;
    for (const std::string& line : lines) {
        joined += line + "\n";
    }
    // 每份文档一行:路径、类型标签、字节数、摘要前 8 位;nearest 有标注。
    CHECK(Contains(joined, "AGENTS.md"));
    CHECK(Contains(joined, "[OVERRIDE]"));
    CHECK(Contains(joined, "sha256:"));
    CHECK(Contains(joined, "离目标最近"));
    CHECK(Contains(joined, "指纹"));
    CHECK(Contains(joined, "状态: 完整"));
    // 不泄正文(单子 §12.5)。
    CHECK(!Contains(joined, "root rule"));
    CHECK(!Contains(joined, "near override rule"));

    // 空链也有交代,不装看不见。
    const TempProject empty_project;
    const auto empty_chain = resolver.ResolveForPath(empty_project.path / "x.cpp");
    const auto empty_lines = lubancode::config::FormatInstructionChainLines(empty_chain, resolver.max_bytes());
    bool saw_empty_note = false;
    for (const std::string& line : empty_lines) {
        if (line.find("无——链上没有任何指令文档") != std::string::npos) {
            saw_empty_note = true;
        }
    }
    CHECK(saw_empty_note);
}

// ---------------------------------------------------------------------------
// §10.3:symlink 边界——项目内允许、项目外拒读、断链明报。
// (symlink 创建在 Windows 需要权限/开发者模式,创建不了就整案跳过,
//  不硬凑。)
// ---------------------------------------------------------------------------

TEST_CASE("symlinked instruction files respect the project boundary") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("inside.md", "inside target");
    std::filesystem::create_directories(project.path / "src");

    std::error_code ec;
    std::filesystem::create_symlink(project.path / "inside.md", project.path / "src/AGENTS.md", ec);
    if (ec) {
        // 本机不让建 symlink(Windows 无权限/非开发者模式):这案没法就地
        // 验,如实跳过。WSL/POSIX 侧 syntax 与行为同册。
        return;
    }

    const lubancode::config::ProjectInstructionResolver resolver;
    const auto chain = resolver.ResolveForPath(project.path / "src/x.cpp");
    REQUIRE(chain.documents.size() == 2);
    // 链到项目内:允许,hash 按真实文件内容,scope 按 link 所在目录。
    CHECK(chain.documents.back().content == "inside target");
    CHECK(chain.documents.back().scope_dir == project.path / "src");
    CHECK(HasDiagnostic(chain, "symlink_inside_project"));

    // 项目外目标:拒读,不把外面的正文拉进来。
    const auto outside = std::filesystem::temp_directory_path() /
                         ("lubancode_resolver_foreign_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(outside);
    {
        std::ofstream file(outside / "evil.md", std::ios::binary);
        file << "outside smuggled rule";
    }
    std::filesystem::create_symlink(outside / "evil.md", project.path / "AGENTS.override.md", ec);
    if (!ec) {
        const auto screened = resolver.ResolveForPath(project.path / "x.cpp");
        CHECK(HasDiagnostic(screened, "symlink_outside_project"));
        CHECK(!Contains(screened.content, "outside smuggled rule"));
        // 根层被拒后回落同层 AGENTS.md。
        REQUIRE(screened.documents.size() == 1);
        CHECK(screened.documents[0].content == "root rule");
    }

    // 断链:明报,层上当作没有这份。
    std::filesystem::remove(project.path / "src/AGENTS.md", ec);
    std::filesystem::create_symlink(project.path / "nowhere.md", project.path / "src/AGENTS.md", ec);
    if (!ec) {
        const auto broken = resolver.ResolveForPath(project.path / "src/x.cpp");
        CHECK(HasDiagnostic(broken, "symlink_broken"));
        REQUIRE(broken.documents.size() == 1);  // 只剩根
    }

    std::error_code cleanup;
    std::filesystem::remove_all(outside, cleanup);
}
