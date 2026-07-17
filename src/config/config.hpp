// 从环境变量读配置。密钥绝不硬编码进源码,只认环境变量。

#pragma once

#include <expected>
#include <string>

namespace lubancode::config {

struct Config {
    std::string base_url;   // ANTHROPIC_BASE_URL,默认 https://api.minimaxi.com/anthropic
    std::string auth_token;  // ANTHROPIC_AUTH_TOKEN,必填
    std::string model;      // ANTHROPIC_MODEL,默认 MiniMax-M3
};

// 缺了必填的 ANTHROPIC_AUTH_TOKEN 时,返回一段人能看懂的错误提示。
std::expected<Config, std::string> LoadFromEnv();

}  // namespace lubancode::config
