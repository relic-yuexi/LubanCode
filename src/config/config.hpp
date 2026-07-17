// 从环境变量读配置。密钥绝不硬编码进源码,只认环境变量。

#pragma once

#include <expected>
#include <string>

namespace lubancode::config {

// 说哪种"方言"跟模型对话:Anthropic 的 Messages API,还是 OpenAI 的
// Responses API。由环境变量 LUBANCODE_WIRE 决定,默认 anthropic。
enum class Wire { Anthropic, Responses };

struct Config {
    Wire wire = Wire::Anthropic;

    // wire == Anthropic 时:base_url 读 ANTHROPIC_BASE_URL(默认
    //   https://api.minimaxi.com/anthropic),auth_token 读 ANTHROPIC_AUTH_TOKEN
    //   (必填),model 读 ANTHROPIC_MODEL(默认 MiniMax-M3)。
    // wire == Responses 时:base_url 读 OPENAI_BASE_URL(默认
    //   https://api.minimaxi.com/v1),auth_token 读 OPENAI_API_KEY(必填),
    //   model 读 OPENAI_MODEL(默认 MiniMax-M3)。
    std::string base_url;
    std::string auth_token;
    std::string model;
};

// 缺了必填的 ANTHROPIC_AUTH_TOKEN 时,返回一段人能看懂的错误提示。
std::expected<Config, std::string> LoadFromEnv();

}  // namespace lubancode::config
