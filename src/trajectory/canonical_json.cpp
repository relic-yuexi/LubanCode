#include "trajectory/canonical_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace lubancode::trajectory {
namespace {

void AppendEscaped(std::string* out, std::string_view text) {
    out->push_back('"');
    for (const char byte : text) {
        const unsigned char c = static_cast<unsigned char>(byte);
        switch (c) {
            case '"':
                out->append("\\\"");
                break;
            case '\\':
                out->append("\\\\");
                break;
            case '\b':
                out->append("\\b");
                break;
            case '\f':
                out->append("\\f");
                break;
            case '\n':
                out->append("\\n");
                break;
            case '\r':
                out->append("\\r");
                break;
            case '\t':
                out->append("\\t");
                break;
            default:
                if (c < 0x20) {
                    // 控制字符统一 \u00xx(小写 hex,钉死字节)。
                    static constexpr char kHex[] = "0123456789abcdef";
                    out->append("\\u00");
                    out->push_back(kHex[(c >> 4) & 0xF]);
                    out->push_back(kHex[c & 0xF]);
                } else {
                    out->push_back(byte);
                }
                break;
        }
    }
    out->push_back('"');
}

// 浮点文本:nlohmann 3.11.3 的 Grisu2 最短表示,同一 vendored 版本跨平台
// 确定性输出(标准库 to_chars 的最短表示允许实现间字节差异,不用)。
std::string FormatFloat(double value) {
    return nlohmann::json(value).dump();
}

bool DumpValue(const nlohmann::json& value, std::string* out, std::string* error_code,
               std::string* message) {
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            out->append("null");
            return true;
        case nlohmann::json::value_t::boolean:
            out->append(value.get<bool>() ? "true" : "false");
            return true;
        case nlohmann::json::value_t::number_integer:
            out->append(std::to_string(value.get<std::int64_t>()));
            return true;
        case nlohmann::json::value_t::number_unsigned:
            out->append(std::to_string(value.get<std::uint64_t>()));
            return true;
        case nlohmann::json::value_t::number_float: {
            const double number = value.get<double>();
            if (!std::isfinite(number)) {
                *error_code = "canonical_json.nan_or_inf";
                *message = "规范 JSON 拒绝 NaN/Inf";
                return false;
            }
            out->append(FormatFloat(number));
            return true;
        }
        case nlohmann::json::value_t::string: {
            const std::string& text = value.get_ref<const std::string&>();
            if (!IsValidUtf8(text)) {
                *error_code = "canonical_json.invalid_utf8";
                *message = "字符串含非法 UTF-8";
                return false;
            }
            AppendEscaped(out, text);
            return true;
        }
        case nlohmann::json::value_t::array: {
            out->push_back('[');
            bool first = true;
            for (const auto& item : value) {
                if (!first) {
                    out->push_back(',');
                }
                first = false;
                if (!DumpValue(item, out, error_code, message)) {
                    return false;
                }
            }
            out->push_back(']');
            return true;
        }
        case nlohmann::json::value_t::object: {
            // 显式收集 key 并按字节序排序:不依赖 json 对象容器的内部序,
            // ordered_json 输入也能出同一份规范字节。
            std::vector<std::reference_wrapper<const std::string>> keys;
            keys.reserve(value.size());
            for (auto it = value.begin(); it != value.end(); ++it) {
                keys.emplace_back(it.key());
            }
            std::sort(keys.begin(), keys.end(),
                      [](const auto& a, const auto& b) { return a.get().compare(b.get()) < 0; });
            out->push_back('{');
            bool first = true;
            for (const auto& key_ref : keys) {
                if (!first) {
                    out->push_back(',');
                }
                first = false;
                AppendEscaped(out, key_ref.get());
                out->push_back(':');
                if (!DumpValue(value.at(key_ref.get()), out, error_code, message)) {
                    return false;
                }
            }
            out->push_back('}');
            return true;
        }
        case nlohmann::json::value_t::binary:
            *error_code = "canonical_json.binary_unsupported";
            *message = "规范 JSON 拒绝二进制值";
            return false;
        case nlohmann::json::value_t::discarded:
        default:
            *error_code = "canonical_json.discarded";
            *message = "传入未初始化的 json 值";
            return false;
    }
}

}  // namespace

bool IsValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char min_value = 0;
        unsigned char max_value = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            min_value = 0x80;
            max_value = 0xBF;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            min_value = 0x80;
            max_value = 0xBF;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            min_value = 0x80;
            max_value = 0xBF;
        } else {
            return false;  // 孤立项或过长编码。
        }
        if (i + length > text.size()) {
            return false;
        }
        std::uint32_t code_point = lead & (0xFF >> (length + 1));
        for (std::size_t j = 1; j < length; ++j) {
            const unsigned char cont = static_cast<unsigned char>(text[i + j]);
            if (cont < min_value || cont > max_value) {
                return false;
            }
            code_point = (code_point << 6) | (cont & 0x3F);
        }
        // 拒绝过长编码与代理区、越界码点。
        const std::uint32_t kMinCodePoint[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (code_point < kMinCodePoint[length] || code_point > 0x10FFFF ||
            (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

std::expected<std::string, std::string> CanonicalJsonDump(const nlohmann::json& value) {
    std::string out;
    out.reserve(256);
    std::string error_code;
    std::string message;
    if (!DumpValue(value, &out, &error_code, &message)) {
        return std::unexpected(error_code + ": " + message);
    }
    return out;
}

}  // namespace lubancode::trajectory
