#include "config/config.hpp"

#include <cstdlib>
#include <optional>

namespace lubancode::config {

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.minimaxi.com/anthropic";
constexpr const char* kDefaultModel = "MiniMax-M3";

// 读一个环境变量;没设置或者是空串都算"没有"。
std::optional<std::string> GetEnv(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

}  // namespace

std::expected<Config, std::string> LoadFromEnv() {
    Config config;
    config.base_url = GetEnv("ANTHROPIC_BASE_URL").value_or(kDefaultBaseUrl);
    config.model = GetEnv("ANTHROPIC_MODEL").value_or(kDefaultModel);

    auto token = GetEnv("ANTHROPIC_AUTH_TOKEN");
    if (!token.has_value()) {
        return std::unexpected(
            "缺少环境变量 ANTHROPIC_AUTH_TOKEN(认证令牌),没有它没法跟模型对话。\n"
            "先设置好,例如:\n"
            "  export ANTHROPIC_AUTH_TOKEN=sk-xxx\n"
            "再重新运行 lubancode。");
    }
    config.auth_token = std::move(*token);

    return config;
}

}  // namespace lubancode::config
