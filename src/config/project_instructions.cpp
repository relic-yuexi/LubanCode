#include "config/project_instructions.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace lubancode::config {

namespace {

std::filesystem::path AbsoluteNormal(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

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
    ProjectInstructions result;
    result.project_root = FindProjectRoot(cwd);
    if (max_bytes == 0) {
        result.truncated = true;
        return result;
    }

    for (const std::filesystem::path& dir : DirectoriesRootToCwd(result.project_root, cwd)) {
        const std::filesystem::path override_file = dir / "AGENTS.override.md";
        const std::filesystem::path agents_file = dir / "AGENTS.md";
        std::filesystem::path chosen;
        std::string text;
        if (RegularFile(override_file)) {
            text = ReadTrimmed(override_file);
            if (!text.empty()) {
                chosen = override_file;
            }
        }
        if (chosen.empty() && RegularFile(agents_file)) {
            text = ReadTrimmed(agents_file);
            if (!text.empty()) {
                chosen = agents_file;
            }
        }
        if (text.empty()) {
            continue;
        }

        const std::string heading = "## Instructions from " + PathUtf8(chosen) + "\n\n";
        const std::string separator = result.content.empty() ? std::string() : "\n\n";
        const std::size_t needed = separator.size() + heading.size() + text.size();
        if (result.content.size() + needed > max_bytes) {
            const std::size_t remaining = max_bytes > result.content.size() ? max_bytes - result.content.size() : 0;
            const std::string combined = separator + heading + text;
            result.content.append(combined, 0, Utf8PrefixLength(combined, remaining));
            result.truncated = true;
            result.sources.push_back(chosen);
            break;
        }
        result.content += separator + heading + text;
        result.sources.push_back(chosen);
    }
    if (!result.content.empty()) {
        result.content = "# Project Instructions\n\n"
                         "Follow these repository instructions. Files nearer the working directory take precedence.\n\n" +
                         result.content;
    }
    return result;
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
