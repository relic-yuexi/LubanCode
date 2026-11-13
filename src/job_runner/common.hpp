#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace lubancode::job_runner {
namespace fs = std::filesystem;
using Json = nlohmann::json;
inline constexpr std::size_t kMaxRequestBytes = 64 * 1024;
inline constexpr std::size_t kMaxLedgerBytes = 32 * 1024 * 1024;

struct Error : std::runtime_error {
    explicit Error(const std::string& code) : std::runtime_error(code) {}
};

std::int64_t NowMs();
std::string RandomHex();
bool HexId(std::string_view value);
bool Identity(std::string_view value);
bool EqualSecret(std::string_view left, std::string_view right);
Json Failure(std::string_view code);
Json ReadJson(const fs::path& path, std::size_t limit = kMaxRequestBytes);
void SaveJson(const fs::path& path, const Json& value);
void PublishJson(const fs::path& directory, const std::string& name, const Json& value);
void PrivateDirectory(const fs::path& directory);
void VerifyPrivateDirectory(const fs::path& directory);
void Keys(const Json& value, std::initializer_list<std::string_view> allowed);
Json NormalizeSpec(const Json& value);
std::vector<std::string> ResolveEnvironment(const Json& spec);
std::string KeyFor(std::string_view session, std::string_view key);
std::string Digest(const Json& value);
bool Terminal(std::string_view state);
std::string ProcessScope();
}  // namespace lubancode::job_runner
