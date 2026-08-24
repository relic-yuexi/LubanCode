#include "api/models.hpp"

#include <chrono>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "cli/i18n.hpp"

namespace lubancode::api {

namespace {

using nlohmann::json;

// data 数组里每个元素抽 id/display_name,id 缺失或不是字符串就跳过那一条
// (宁可少列一个,也不要因为一条脏数据让整个列表解析失败)。display_name
// 缺省时用 id 兜底。
std::vector<ModelInfo> ExtractModelsFromDataArray(const json& data) {
    std::vector<ModelInfo> models;
    for (const auto& item : data) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            continue;
        }
        ModelInfo info;
        info.id = item["id"].get<std::string>();
        if (item.contains("display_name") && item["display_name"].is_string()) {
            info.display_name = item["display_name"].get<std::string>();
        } else {
            info.display_name = info.id;
        }
        models.push_back(std::move(info));
    }
    return models;
}

}  // namespace

std::expected<std::vector<ModelInfo>, std::string> ParseAnthropicModelsResponse(const std::string& json_text) {
    json parsed;
    try {
        parsed = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return std::unexpected(std::string("models 响应不是合法 JSON: ") + e.what());
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_array()) {
        return std::unexpected("models 响应缺少 data 数组(anthropic wire 期望形如 {\"data\":[...]})");
    }
    return ExtractModelsFromDataArray(parsed["data"]);
}

std::expected<std::vector<ModelInfo>, std::string> ParseResponsesModelsResponse(const std::string& json_text) {
    json parsed;
    try {
        parsed = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return std::unexpected(std::string("models 响应不是合法 JSON: ") + e.what());
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_array()) {
        return std::unexpected("models 响应缺少 data 数组(responses wire 期望形如 {\"object\":\"list\",\"data\":[...]})");
    }
    return ExtractModelsFromDataArray(parsed["data"]);
}

std::expected<std::vector<ModelInfo>, std::string> ParseGeminiModelsResponse(const std::string& json_text) {
    json parsed;
    try {
        parsed = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return std::unexpected(std::string("models 响应不是合法 JSON: ") + e.what());
    }
    if (!parsed.is_object() || !parsed.contains("models") || !parsed["models"].is_array()) {
        return std::unexpected(
            "models 响应缺少 models 数组(google-generate-content wire 期望形如 {\"models\":[...]})");
    }
    std::vector<ModelInfo> models;
    for (const auto& item : parsed["models"]) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
            continue;  // 脏条目跳过,别让一条坏数据掀翻整个列表
        }
        std::string name = item["name"].get<std::string>();
        // name 形如 "models/gemini-2.5-flash",去前缀才是能填进配置的 model。
        constexpr const char* kModelsPrefix = "models/";
        if (name.rfind(kModelsPrefix, 0) == 0) {
            name = name.substr(7);
        }
        // 只留支持 generateContent 的条目:embedding / aqa 这些列出来也调不动。
        bool supports_generate = false;
        if (auto methods = item.find("supportedGenerationMethods");
            methods != item.end() && methods->is_array()) {
            for (const auto& method : *methods) {
                if (method.is_string() && method.get<std::string>() == "generateContent") {
                    supports_generate = true;
                    break;
                }
            }
        }
        if (!supports_generate) {
            continue;
        }
        ModelInfo info;
        info.id = name;
        info.display_name = item.contains("displayName") && item["displayName"].is_string()
                                ? item["displayName"].get<std::string>()
                                : name;
        models.push_back(std::move(info));
    }
    return models;
}

std::string ModelsUrl(config::Wire wire, const std::string& base_url) {
    if (wire == config::Wire::Anthropic) {
        // base_url 已带 /v1 结尾时不重复再补(否则 /v1/v1/models)。
        if (base_url.size() >= 3 && base_url.compare(base_url.size() - 3, 3, "/v1") == 0) {
            return base_url + "/models";
        }
        return base_url + "/v1/models";
    }
    if (wire == config::Wire::GoogleGenerateContent) {
        // 已带 /v1beta 结尾时不重复再补(否则 /v1beta/v1beta/models)。
        if (base_url.size() >= 7 && base_url.compare(base_url.size() - 7, 7, "/v1beta") == 0) {
            return base_url + "/models";
        }
        return base_url + "/v1beta/models";
    }
    return base_url + "/models";
}

std::map<std::string, std::string> ModelsRequestHeaders(
    config::Wire wire, const std::string& api_key, const std::map<std::string, std::string>& extra_headers) {
    std::map<std::string, std::string> headers;
    // gemini wire 的 native 鉴权头是 x-goog-api-key,不是 Authorization
    // Bearer(与 api/gemini/client.cpp 的 GeminiBaseHeaders 同一份规矩)。
    if (!api_key.empty()) {
        if (wire == config::Wire::GoogleGenerateContent) {
            headers["x-goog-api-key"] = api_key;
        } else {
            headers["Authorization"] = "Bearer " + api_key;
        }
    }
    for (const auto& [name, value] : extra_headers) {
        if (value.empty()) {
            headers.erase(name);
        } else {
            headers[name] = value;
        }
    }
    return headers;
}

std::expected<std::vector<ModelInfo>, Error> ListModels(config::Wire wire, const std::string& base_url,
                                                          const std::string& api_key, int connect_timeout_ms,
                                                          int request_timeout_secs,
                                                          const std::map<std::string, std::string>& extra_headers) {
    const std::string url = ModelsUrl(wire, base_url);

    // M11:非流式请求,直接给连接超时 + 整体超时(cpr::Timeout 是总时长上限,
    // 跟 send_stream 那条"不设总超时,只设空闲读超时"的路数不一样——这里
    // 响应体小,回复"很长"的顾虑不存在)。
    // 鉴权头出自 ModelsRequestHeaders:按 wire 挑 Bearer / x-goog-api-key,
    // key 为空时彻底不带,不发空头。
    const std::map<std::string, std::string> header_map = ModelsRequestHeaders(wire, api_key, extra_headers);
    cpr::Header headers;
    for (const auto& [name, value] : header_map) {
        headers[name] = value;
    }
    cpr::Response response =
        cpr::Get(cpr::Url{url}, headers,
                 cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms)},
                 cpr::Timeout{std::chrono::seconds(request_timeout_secs)});

    if (response.error) {
        std::string message;
        if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
            message = cli::trf("error.network.request_timeout", request_timeout_secs);
        } else if (response.error.code == cpr::ErrorCode::COULDNT_CONNECT ||
                   response.error.code == cpr::ErrorCode::COULDNT_RESOLVE_HOST ||
                   response.error.code == cpr::ErrorCode::COULDNT_RESOLVE_PROXY) {
            message = cli::trf("error.network.connect_failed", response.error.message);
        } else {
            message = response.error.message;
        }
        return std::unexpected(Error{ErrorKind::Network, message, 0});
    }

    const int status = static_cast<int>(response.status_code);
    if (status < 200 || status >= 300) {
        std::string message = !response.text.empty() ? response.text : "服务端返回了非 200 状态码,但响应体是空的";
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), status});
    }

    // 按 wire 挑解析函数(gemini 的形状见 ParseGeminiModelsResponse 注释)。
    const auto parsed = [wire, &text = response.text]() {
        if (wire == config::Wire::Anthropic) {
            return ParseAnthropicModelsResponse(text);
        }
        if (wire == config::Wire::GoogleGenerateContent) {
            return ParseGeminiModelsResponse(text);
        }
        return ParseResponsesModelsResponse(text);
    }();
    if (!parsed.has_value()) {
        return std::unexpected(Error{ErrorKind::Parse, parsed.error(), status});
    }
    return *parsed;
}

}  // namespace lubancode::api
