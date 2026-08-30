#include "config/project_instructions.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "hooks/hash.hpp"  // Sha256Hex:文档与链指纹的原料

namespace lubancode::config {

namespace {

std::filesystem::path AbsoluteNormal(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

}  // namespace

// 项目根发现:头文件里导出(自定义 Agent 单阶段 1 起,Agent Catalog 的
// 项目层也从这里起算),实现原样。
std::filesystem::path FindProjectRoot(const std::filesystem::path& cwd) {
    std::filesystem::path current = AbsoluteNormal(cwd);
    std::error_code ec;
    if (!std::filesystem::is_directory(current, ec)) {
        current = current.parent_path();
    }
    const std::filesystem::path fallback = current;
    while (!current.empty()) {
        ec.clear();
        if (std::filesystem::exists(current / ".git", ec) && !ec) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }
        current = parent;
    }
    return fallback;
}

namespace {

std::string ReadTrimmed(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    std::size_t begin = 0;
    while (begin < text.size() && whitespace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && whitespace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool RegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::size_t Utf8PrefixLength(const std::string& text, std::size_t limit) {
    std::size_t length = (std::min)(limit, text.size());
    if (length == text.size()) {
        return length;
    }
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

std::vector<std::filesystem::path> DirectoriesRootToCwd(const std::filesystem::path& root,
                                                         const std::filesystem::path& cwd) {
    std::vector<std::filesystem::path> reversed;
    std::filesystem::path current = AbsoluteNormal(cwd);
    while (!current.empty()) {
        reversed.push_back(current);
        if (current == root) {
            break;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }
        current = parent;
    }
    if (reversed.empty() || reversed.back() != root) {
        return {root};
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::string BuildScaffold(const std::filesystem::path& root) {
    std::vector<std::string> build;
    std::vector<std::string> test;
    bool commands_are_shell = true;
    if (RegularFile(root / "CMakeLists.txt")) {
        build = {"cmake -S . -B build", "cmake --build build"};
        test = {"ctest --test-dir build --output-on-failure"};
    } else if (RegularFile(root / "package.json")) {
        build = {"npm install", "npm run build"};
        test = {"npm test"};
    } else if (RegularFile(root / "Cargo.toml")) {
        build = {"cargo build"};
        test = {"cargo test"};
    } else if (RegularFile(root / "go.mod")) {
        build = {"go build ./..."};
        test = {"go test ./..."};
    } else if (RegularFile(root / "pyproject.toml") || RegularFile(root / "pytest.ini")) {
        build = {"python -m pip install -e ."};
        test = {"python -m pytest"};
    } else if (RegularFile(root / "Makefile") || RegularFile(root / "makefile")) {
        build = {"make"};
        test = {"make test"};
    } else {
        commands_are_shell = false;
        build = {"Read `README.md` and repository configuration, then record the supported build command here."};
        test = {"Run the narrowest relevant test first, then the full suite before handoff."};
    }

    std::ostringstream out;
    out << "# Repository Guidelines\n\n"
        << "## Project Layout\n\n"
        << "- Read `README.md` and nearby source files before changing code.\n"
        << "- Keep changes inside the module that owns the behavior.\n"
        << "- Do not edit generated files unless the repository documents that workflow.\n\n"
        << "## Build and Test\n\n";
    for (const std::string& command : build) {
        out << "- Build: " << (commands_are_shell ? "`" : "") << command
            << (commands_are_shell ? "`" : "") << "\n";
    }
    for (const std::string& command : test) {
        out << "- Test: " << (commands_are_shell ? "`" : "") << command
            << (commands_are_shell ? "`" : "") << "\n";
    }
    out << "\n## Working Agreements\n\n"
        << "- Preserve existing style and public behavior unless the task calls for a change.\n"
        << "- Keep patches focused. Leave unrelated user changes in place.\n"
        << "- Add or update tests when behavior changes.\n"
        << "- Report the commands run and any checks that could not be completed.\n";
    return out.str();
}

}  // namespace

ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd, std::size_t max_bytes) {
    // 零退化投影(AGENTS.md 作用域单 P0):字符串 loader 不再自持解析逻辑,
    // 同一只 Resolver 出账。root->cwd 的目录序、同层 override、空文件跳过、
    // 32 KiB 帽与拼接格式逐字节照旧——既有测试(unit.config.project_
    // instructions)钉的就是这份投影。
    const InstructionChain chain = ProjectInstructionResolver(max_bytes).ResolveForPath(cwd);
    ProjectInstructions result;
    result.project_root = chain.project_root;
    result.sources = chain.sources;
    result.content = chain.content;
    result.truncated = chain.truncated;
    return result;
}

// ---------------------------------------------------------------------------
// 结构化 Resolver(单子 §六/§七 P0)
// ---------------------------------------------------------------------------

namespace {

// 链指纹:项目根 + 每份文档的(scope_dir, 正文摘要)依序入料。指纹认
// "哪几份规则管这个作用域",不认目标文件本身——同一最近 AGENTS.md 之下
// 的兄弟文件指纹相同(§7.4 按 fingerprint 分组的依据);文档内容一变,
// 指纹即变,旧确认作废。
std::string ChainFingerprint(const std::filesystem::path& root, const std::vector<InstructionDocument>& docs) {
    std::string material;
    material += "root:" + PathUtf8(root) + "\n";
    for (const InstructionDocument& doc : docs) {
        material += "doc:" + PathUtf8(doc.scope_dir) + ":" + doc.sha256 + "\n";
    }
    return hooks::Sha256Hex(material);
}

}  // namespace

InstructionChain ProjectInstructionResolver::ResolveForPath(const std::filesystem::path& target) const {
    InstructionChain chain;
    // 目标归一:文件取父目录起链,目录取自身(§12.1 两种输入)。
    std::error_code ec;
    std::filesystem::path anchor = AbsoluteNormal(target);
    if (!std::filesystem::is_directory(anchor, ec)) {
        anchor = anchor.parent_path();
    }
    chain.target_path = AbsoluteNormal(target);
    chain.project_root = FindProjectRoot(anchor);

    // 每层选文档:非空 AGENTS.override.md 压过非空 AGENTS.md,两者皆空跳过
    // ——与旧 loader 同一张机械表,顺带记空文件/遮蔽两笔诊断。
    for (const std::filesystem::path& dir : DirectoriesRootToCwd(chain.project_root, anchor)) {
        const std::filesystem::path override_file = dir / "AGENTS.override.md";
        const std::filesystem::path agents_file = dir / "AGENTS.md";
        bool has_override = false;
        std::string override_text;
        std::string agents_text;
        if (RegularFile(override_file)) {
            has_override = true;
            override_text = ReadTrimmed(override_file);
        }
        if (RegularFile(agents_file)) {
            agents_text = ReadTrimmed(agents_file);
        }

        if (has_override) {
            if (override_text.empty()) {
                InstructionDiagnostic note;
                note.path = override_file;
                note.code = "empty_skipped";
                note.message = "AGENTS.override.md 为空,跳过并回落同层 AGENTS.md";
                chain.diagnostics.push_back(std::move(note));
            } else if (!agents_text.empty()) {
                InstructionDiagnostic note;
                note.path = agents_file;
                note.code = "shadowed_same_directory";
                note.message = "同层存在非空 AGENTS.override.md,这份 AGENTS.md 被遮蔽";
                chain.diagnostics.push_back(std::move(note));
            }
        }
        if (agents_text.empty() && RegularFile(agents_file)) {
            InstructionDiagnostic note;
            note.path = agents_file;
            note.code = "empty_skipped";
            note.message = "AGENTS.md 为空,跳过";
            chain.diagnostics.push_back(std::move(note));
        }

        const bool use_override = has_override && !override_text.empty();
        const std::string& text = use_override ? override_text : agents_text;
        if (text.empty()) {
            continue;
        }
        InstructionDocument doc;
        doc.source_path = use_override ? override_file : agents_file;
        doc.scope_dir = dir;
        doc.is_override = use_override;
        doc.content = text;
        doc.sha256 = hooks::Sha256Hex(text);
        doc.bytes = text.size();
        chain.documents.push_back(std::move(doc));
    }

    chain.fingerprint = ChainFingerprint(chain.project_root, chain.documents);

    // 旧口径的 content 投影:格式、字节帽、截断点与从前一字不差(单测
    // unit.config.project_instructions 的四条用例钉住这层兼容)。
    if (max_bytes_ == 0) {
        chain.truncated = true;  // 与旧 loader 同口径:零预算即视作截断
    } else {
        for (const InstructionDocument& doc : chain.documents) {
            const std::string heading = "## Instructions from " + PathUtf8(doc.source_path) + "\n\n";
            const std::string separator = chain.content.empty() ? std::string() : "\n\n";
            const std::size_t needed = separator.size() + heading.size() + doc.content.size();
            if (chain.content.size() + needed > max_bytes_) {
                const std::size_t remaining =
                    max_bytes_ > chain.content.size() ? max_bytes_ - chain.content.size() : 0;
                const std::string combined = separator + heading + doc.content;
                chain.content.append(combined, 0, Utf8PrefixLength(combined, remaining));
                chain.truncated = true;
                chain.sources.push_back(doc.source_path);
                break;
            }
            chain.content += separator + heading + doc.content;
            chain.sources.push_back(doc.source_path);
        }
    }
    if (!chain.content.empty()) {
        chain.content = "# Project Instructions\n\n"
                        "Follow these repository instructions. Files nearer the working directory take precedence.\n\n" +
                        chain.content;
    }
    return chain;
}

InitProjectInstructionsResult InitializeProjectInstructions(const std::filesystem::path& cwd) {
    InitProjectInstructionsResult result;
    const std::filesystem::path root = FindProjectRoot(cwd);
    const std::filesystem::path override_file = root / "AGENTS.override.md";
    const std::filesystem::path agents_file = root / "AGENTS.md";
    if (RegularFile(override_file)) {
        result.status = InitProjectInstructionsStatus::AlreadyExists;
        result.path = override_file;
        return result;
    }
    if (RegularFile(agents_file)) {
        result.status = InitProjectInstructionsStatus::AlreadyExists;
        result.path = agents_file;
        return result;
    }

    std::ofstream file(agents_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        result.path = agents_file;
        result.error = "cannot open file for writing";
        return result;
    }
    file << BuildScaffold(root);
    if (!file.good()) {
        result.path = agents_file;
        result.error = "failed while writing file";
        return result;
    }
    result.status = InitProjectInstructionsStatus::Created;
    result.path = agents_file;
    return result;
}

}  // namespace lubancode::config
