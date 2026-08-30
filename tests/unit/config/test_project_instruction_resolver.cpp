// AGENTS.md 作用域单 P0 的 Resolver 册:结构化解析(文档/链/指纹/诊断)
// 与旧字符串 loader 的零退化投影。钉的账:
//   1. ResolveForPath 机械顺序 root -> target parent,嵌套 AGENTS.md 从
//      仓库根也能解析到(单子 §5.1 的病灶);
//   2. 同层 override 压 AGENTS、空文件跳过、空 override 回落(机械表与
//      旧 loader 同一张);
//   3. 指纹按作用域文档内容寻址:同 scope 兄弟文件同指纹,内容一变指纹
//      即变(写前闸的失效机制);
//   4. LoadProjectInstructions 是 ResolveForPath 的逐字节投影(零退化);
//   5. 诊断:空文件与同层遮蔽分账。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
