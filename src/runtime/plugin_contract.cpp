// 冻结合同的实现:manifest 强校验、${plugin_dir} 结构化替换、进程协议
// 帧的序列化/解析、入参 Schema 子集校验。全部纯函数,不碰进程、不碰
// Lua、不碰 DLL——那是 runtime 实现层的事。
#include "runtime/plugin_contract.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::runtime {

namespace {

// 字节偏移 → 1 起的行列(语法错的精确定位用;偏移越界按文末算)。
void OffsetToLineColumn(std::string_view text, std::size_t offset, int& line, int& column) {
    line = 1;
    column = 1;
    const std::size_t limit = std::min(offset, text.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
}

// 在原文里找 "key"(带引号)首现,折算行列。字段级错误的 best-effort 定位:
// nlohmann 的 DOM 不存位置,嵌套同名键可能命中首个——定位不到或找不着都
// 置 0,不猜。
void LocateJsonKey(std::string_view text, std::string_view key, int& line, int& column) {
    line = 0;
    column = 0;
    std::string needle;
    needle.reserve(key.size() + 2);
    needle += '"';
    needle.append(key);
    needle += '"';
    const std::size_t hit = text.find(needle);
    if (hit == std::string_view::npos) {
        return;
    }
    OffsetToLineColumn(text, hit, line, column);
}

PluginManifestIssue MakeIssue(PluginManifestIssueCode code, const std::string& message, std::string_view text,
                              std::string_view locate_key = {}) {
    PluginManifestIssue issue;
    issue.code = code;
    issue.message = message;
    if (!locate_key.empty()) {
        LocateJsonKey(text, locate_key, issue.line, issue.column);
    }
    return issue;
}

// 环境变量名/env 键的规矩(dotenv 子集同款):[A-Za-z_][A-Za-z0-9_]*。
bool IsValidEnvName(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    const char first = name.front();
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// 点分四段 IPv4 字面量(每段 0-255,不许前导零宽化)。
bool IsIpv4Literal(std::string_view host) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            parts.push_back(host.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 4) {
        return false;
    }
    for (const std::string_view part : parts) {
        if (part.empty() || part.size() > 3) {
            return false;
        }
        if (!std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        int value = 0;
        for (const char c : part) {
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return false;
        }
        if (part.size() > 1 && part[0] == '0') {
            return false;  // "01" 不是规范 IPv4 段
        }
    }
    return true;
}

bool IsIpLiteralHost(std::string_view host) {
    if (host.find(':') != std::string_view::npos) {
        return true;  // IPv6 字面量(含方括号剥除后的冒号形态)
    }
    if (!host.empty() && host.front() == '[' && !host.empty() && host.back() == ']') {
        return true;
    }
    return IsIpv4Literal(host);
}

// ---------------------------------------------------------------------------
// Punycode 编码(RFC 3492 的 encode 方向;IDNA 规范化的受限子集——只做
// 非 ASCII 标签的 punycode 编码,不做 UTS46 全表映射/禁用字表)。
// 失败(码点超 BMP 上限、delta 溢出)返回 false。
// ---------------------------------------------------------------------------
bool PunycodeEncodeLabel(std::u32string_view input, std::string& out) {
    constexpr std::int32_t kBase = 36, kTMin = 1, kTMax = 26, kSkew = 38, kDamp = 700;
    constexpr std::int32_t kInitialBias = 72, kInitialN = 128;
    const auto digit_value = [](std::int32_t digit) -> char {
        // RFC 3492 §5:数字 0-25 是 'a'-'z',26-35 是 '2'-'7'(没有大写)。
        if (digit < 26) return static_cast<char>('a' + digit);
        return static_cast<char>('2' + (digit - 26));
    };
    const auto adapt = [](std::int32_t delta, std::int32_t numpoints, bool firsttime) -> std::int32_t {
        delta = firsttime ? delta / kDamp : delta / 2;
        delta += delta / numpoints;
        std::int32_t k = 0;
        while (delta > ((kBase - kTMin) * kTMax) / 2) {
            delta /= kBase - kTMin;
            k += kBase;
        }
        return k + (((kBase - kTMin + 1) * delta) / (delta + kSkew));
    };

    std::size_t basic_end = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (static_cast<std::uint32_t>(input[i]) < 128u) {
            ++basic_end;
            out += static_cast<char>(input[i]);
        }
    }
    if (basic_end == input.size()) {
        return true;  // 纯 ASCII 标签,原样
    }
    // 分隔符:有基字符(b > 0)才输出一个 '-'(RFC 3492 §6.3;纯非 ASCII
    // 标签直接跟数字序列,否则 xn-- 里会多出一个连字符)。
    if (basic_end > 0) {
        out += "-";
    }
    std::int32_t n = kInitialN;
    std::int32_t delta = 0;
    std::int32_t bias = kInitialBias;
    std::size_t h = basic_end;
    const std::size_t total = input.size();
    while (h < total) {
        std::int32_t m = 0x10FFFF + 1;
        for (const char32_t c : input) {
            const std::int32_t v = static_cast<std::int32_t>(c);
            if (v >= n && v < m) {
                m = v;
            }
        }
        delta += (m - n) * static_cast<std::int32_t>(h + 1);
        if (delta < 0) {
            return false;
        }
        n = m;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const std::int32_t v = static_cast<std::int32_t>(input[i]);
            if (v < n) {
                if (++delta < 0) {
                    return false;
                }
            } else if (v == n) {
                std::int32_t q = delta;
                for (std::int32_t k = kBase;; k += kBase) {
                    const std::int32_t t = k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
                    if (q < t) {
                        break;
                    }
                    out += digit_value(t + ((q - t) % (kBase - t)));
                    q = (q - t) / (kBase - t);
                }
                out += digit_value(q);
                bias = adapt(delta, static_cast<std::int32_t>(h + 1), h == basic_end);
                delta = 0;
                ++h;
            }
        }
        ++delta;
        ++n;
    }
    return true;
}

// UTF-8 解成 UTF-32(platform::IsValidUtf8 已验过合法性;这里只解)。
std::u32string Utf8ToUtf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        char32_t cp = lead;
        if ((lead & 0x80) == 0) {
            len = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0F;
        } else {
            len = 4;
            cp = lead & 0x07;
        }
        for (std::size_t j = 1; j < len && i + j < text.size(); ++j) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3F);
        }
        out += cp;
        i += len;
    }
    return out;
}

// id/name/version/language 的代码点规矩:ASCII 字母/数字/下划线/连字符,
// 首字符须字母或数字(不许 _- 开头,防误当标志位)。
bool CodePointAllowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// 从 JSON 拿一个非空字符串字段;拿不到/不是串/空串都报人话。manifest 是
// 合同,缺项即坏,不悄悄给默认值。
std::expected<std::string, std::string> RequireString(const nlohmann::json& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end()) {
        return std::unexpected(std::string("缺字段 ") + key);
    }
    if (!it->is_string()) {
        return std::unexpected(std::string(key) + " 字段必须是字符串");
    }
    const std::string value = it->get<std::string>();
    if (value.empty()) {
        return std::unexpected(std::string(key) + " 字段是空串");
    }
    return value;
}

// 拿可选字符串字段;没写返回 nullopt,写了但不是字符串 = 坏。
std::expected<std::optional<std::string>, std::string> OptionalString(const nlohmann::json& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end() || it->is_null()) {
        return std::optional<std::string>{};
    }
    if (!it->is_string()) {
        return std::unexpected(std::string(key) + " 字段必须是字符串");
    }
    return it->get<std::string>();
}

// 入参 schema 的静态强校验(manifest 加载期跑一遍;声明怪也拒)。
// 校验的是"这份 schema 是不是我们认得的子集、形状对不对",不是拿它去
// 验任何输入。递归,深度上限防作者写出千层嵌套。
std::optional<std::string> ValidateSchemaShape(const nlohmann::json& schema, int depth) {
    if (depth > 32) {
        return "schema 嵌套超过 32 层";
    }
    if (!schema.is_object()) {
        return "schema 必须是 object";
    }
    // type:认得的七种之一,或者 type 数组(v1 不认——JSON Schema 允许,我们
    // 的验证子集不实现联合类型,写了就明说)。
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        if (t.is_string()) {
            const std::string type_name = t.get<std::string>();
            if (type_name != "object" && type_name != "array" && type_name != "string" && type_name != "number" &&
                type_name != "integer" && type_name != "boolean" && type_name != "null") {
                return "type 不在认得的集合里: " + type_name;
            }
        } else {
            return "type 必须是单个字符串(联合类型 v1 不认)";
        }
    }
    if (schema.contains("properties") && !schema["properties"].is_object()) {
        return "properties 必须是 object";
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            return "required 必须是数组";
        }
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) {
                return "required 数组里的元素必须是字符串";
            }
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        return "enum 必须是数组";
    }
    if (schema.contains("items")) {
        const auto& items = schema["items"];
        if (!items.is_object()) {
            return "items 必须是 object(联合 items v1 不认)";
        }
        if (const auto inner = ValidateSchemaShape(items, depth + 1); inner.has_value()) {
            return "items." + *inner;
        }
    }
    if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean() &&
        !schema["additionalProperties"].is_object()) {
        return "additionalProperties 必须是布尔或 object";
    }
    // 数值/长度界:得是数字(或长度得是非负整数)。
    for (const char* numeric_key : {"minimum", "maximum"}) {
        if (schema.contains(numeric_key) && !schema[numeric_key].is_number()) {
            return std::string(numeric_key) + " 必须是数字";
        }
    }
    for (const char* length_key : {"minLength", "maxLength", "minItems", "maxItems"}) {
        if (schema.contains(length_key)) {
            const auto& v = schema[length_key];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0) {
                return std::string(length_key) + " 必须是非负整数";
            }
        }
    }
    // 递归 properties。
    if (schema.contains("properties")) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            if (const auto inner = ValidateSchemaShape(it.value(), depth + 1); inner.has_value()) {
                return "properties." + it.key() + ": " + *inner;
            }
        }
    }
    if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object()) {
        if (const auto inner = ValidateSchemaShape(schema["additionalProperties"], depth + 1); inner.has_value()) {
            return "additionalProperties: " + *inner;
        }
    }
    return std::nullopt;
}

// 路径 b 是否在目录 a 之内(含 a 本身)。两边都先 canonical 的事由调用方
// 做;这里只做词法前缀比较(分界必须是目录分隔符,不许 /foo/bar 匹配
// /foo/barbar)。
bool PathIsInside(const std::filesystem::path& dir, const std::filesystem::path& candidate) {
    auto dir_it = dir.begin();
    auto cand_it = candidate.begin();
    for (; dir_it != dir.end(); ++dir_it, ++cand_it) {
        if (cand_it == candidate.end() || *dir_it != *cand_it) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool IsValidPluginIdentifier(std::string_view value, std::size_t max_len) {
    if (value.empty() || value.size() > max_len) {
        return false;
    }
    const char first = value.front();
    if (!std::isalnum(static_cast<unsigned char>(first))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) { return CodePointAllowed(c); });
}

// version 的规矩:id 之外再收 '.'(语义版本 1.0.0 是常态);同样不许
// shell 元字符、不许非 ASCII。
bool IsValidPluginVersion(std::string_view value, std::size_t max_len) {
    if (value.empty() || value.size() > max_len) {
        return false;
    }
    const char first = value.front();
    if (!std::isalnum(static_cast<unsigned char>(first))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](char c) { return CodePointAllowed(c) || c == '.'; });
}

std::string BuildPluginToolName(std::string_view plugin_id, std::string_view tool_name) {
    std::string out;
    out.reserve(plugin_id.size() + tool_name.size() + 11);
    out += "plugin__";
    out += plugin_id;
    out += "__";
    out += tool_name;
    return out;
}

namespace {

constexpr char kWireHex[] = "0123456789ABCDEF";

bool WireIdByteAllowed(unsigned char byte) {
    return CodePointAllowed(static_cast<char>(byte));
}

int HexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string AppendDisplayComponentId(std::string_view package_id, std::string_view local_id) {
    std::string out;
    out.reserve(package_id.size() + 1 + local_id.size());
    out += package_id;
    out += '.';
    out += local_id;
    return out;
}

}  // namespace

std::string EncodeToolWireId(std::string_view component_id) {
    std::string out;
    out.reserve(component_id.size());
    for (const unsigned char byte : component_id) {
        if (WireIdByteAllowed(byte)) {
            out += static_cast<char>(byte);
        } else {
            out += '%';
            out += kWireHex[byte >> 4];
            out += kWireHex[byte & 0x0F];
        }
    }
    return out;
}

std::optional<std::string> DecodeToolWireId(std::string_view encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(encoded[i]);
        if (byte == '%') {
            if (i + 2 >= encoded.size()) return std::nullopt;
            const int high = HexDigitValue(encoded[i + 1]);
            const int low = HexDigitValue(encoded[i + 2]);
            if (high < 0 || low < 0) return std::nullopt;
            out += static_cast<char>((high << 4) | low);
            i += 2;
        } else {
            out += static_cast<char>(byte);
        }
    }
    return out;
}

std::string BuildPackagedToolWireName(std::string_view kind_prefix, std::string_view package_id,
                                      std::string_view local_id, std::string_view tool) {
    std::string out;
    const std::string display = AppendDisplayComponentId(package_id, local_id);
    out.reserve(kind_prefix.size() + display.size() * 3 + tool.size() + 4);
    out += kind_prefix;
    out += "__";
    out += EncodeToolWireId(display);
    out += "__";
    out += tool;
    return out;
}

std::string BuildPackagedToolDisplayName(std::string_view kind_prefix, std::string_view package_id,
                                         std::string_view local_id, std::string_view tool) {
    std::string out;
    const std::string display = AppendDisplayComponentId(package_id, local_id);
    out.reserve(kind_prefix.size() + display.size() + tool.size() + 4);
    out += kind_prefix;
    out += "__";
    out += display;
    out += "__";
    out += tool;
    return out;
}

std::expected<std::string, std::string> ExpandPluginDirPlaceholder(std::string_view text,
                                                                   const std::filesystem::path& plugin_dir) {
    constexpr std::string_view kPlaceholder = "${plugin_dir}";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t hit = text.find(kPlaceholder, pos);
        if (hit == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, hit - pos));
        // 目录的 UTF-8 写法走 u8 通道(GBK 机器上 path 窄口是 ACP,
        // .string() 会抛/坏字节,见 paths.hpp 的规矩)。
        const std::u8string u8 = plugin_dir.u8string();
        out.append(reinterpret_cast<const char*>(u8.data()), u8.size());
        pos = hit + kPlaceholder.size();
    }
    return out;
}

std::expected<std::string, std::string> NormalizeDnsHost(std::string_view raw_host) {
    // 剥方括号(IPv6 的 URL 写法在 URL 层已剥;这里兜一道)。
    std::string_view host = raw_host;
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    // 去末尾点(FQDN 的根点)。
    while (host.size() >= 2 && host.back() == '.') {
        host.remove_suffix(1);
    }
    if (host.empty() || host == ".") {
        return std::unexpected("host 是空名");
    }
    if (host.find('@') != std::string_view::npos) {
        return std::unexpected("host 不收用户信息段(@): " + std::string(host));
    }
    if (host.find('*') != std::string_view::npos) {
        return std::unexpected("host 不收通配符: " + std::string(host));
    }
    if (IsIpLiteralHost(host)) {
        return std::unexpected("host 不收 IP 字面量,第一版只要精确 DNS 名: " + std::string(host));
    }
    if (host.size() > 253) {
        return std::unexpected("host 超过 253 字符");
    }
    // ASCII 小写化(非 ASCII 码点的大写折叠不做——受限 IDNA,非 ASCII 标签
    // 直接进 punycode 编码)。
    std::string lowered;
    lowered.reserve(host.size());
    for (const char c : host) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // 逐标签:非 ASCII → punycode;ASCII → 标签规矩([a-z0-9-],不以前后
    // 连字符收尾,1..63)。
    std::string normalized;
    std::size_t start = 0;
    std::size_t dot_count = 0;
    while (start <= lowered.size()) {
        const std::size_t dot = lowered.find('.', start);
        const std::string_view label =
            std::string_view(lowered).substr(start, dot == std::string::npos ? lowered.size() - start : dot - start);
        if (label.empty()) {
            return std::unexpected("host 有空标签: " + std::string(host));
        }
        bool ascii_only = true;
        for (const char c : label) {
            if (static_cast<unsigned char>(c) >= 0x80) {
                ascii_only = false;
                break;
            }
        }
        std::string encoded;
        if (ascii_only) {
            if (label.size() > 63) {
                return std::unexpected("host 标签超过 63 字符: " + std::string(label));
            }
            for (const char c : label) {
                const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool ok = (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') || lc == '-';
                if (!ok) {
                    return std::unexpected("host 标签字符不认得: " + std::string(label));
                }
                if (lc == '-' && (encoded.empty())) {
                    return std::unexpected("host 标签以连字符开头: " + std::string(label));
                }
                encoded += lc;
            }
            if (!encoded.empty() && encoded.back() == '-') {
                return std::unexpected("host 标签以连字符收尾: " + std::string(label));
            }
        } else {
            // 非 ASCII 标签:先验 UTF-8,再 punycode(IDNA ToASCII:xn-- 前缀
            // + RFC 3492 编码体)。
            if (!platform::IsValidUtf8(std::string(label))) {
                return std::unexpected("host 标签不是合法 UTF-8");
            }
            std::string punycode;
            if (!PunycodeEncodeLabel(Utf8ToUtf32(label), punycode)) {
                return std::unexpected("host 的 IDNA 规范化失败: " + std::string(label));
            }
            encoded = "xn--";
            encoded += punycode;
            if (encoded.size() > 63) {
                return std::unexpected("host 标签 IDNA 编码后超过 63 字符: " + std::string(label));
            }
        }
        if (!normalized.empty()) {
            normalized += '.';
        }
        normalized += encoded;
        ++dot_count;
        if (dot_count > 8) {
            return std::unexpected("host 标签数超过 8,不像正经 DNS 名");
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    // 至少一段点(拒单标签内网名;"localhost" 单标签在此一并落网)。
    if (normalized.find('.') == std::string::npos) {
        return std::unexpected("host 须是至少两段的 DNS 名(不收单标签名): " + normalized);
    }
    // .local 是 mDNS,明确拒。
    if (normalized.size() > 6 && normalized.compare(normalized.size() - 6, 6, ".local") == 0) {
        return std::unexpected("host 不收 .local(mDNS): " + normalized);
    }
    return normalized;
}

EffectiveHttpLimits ApplyHttpLimits(const HttpLimits& declared) {
    EffectiveHttpLimits out;
    if (declared.url_bytes.has_value()) out.url_bytes = *declared.url_bytes;
    if (declared.request_header_bytes.has_value()) out.request_header_bytes = *declared.request_header_bytes;
    if (declared.request_body_bytes.has_value()) out.request_body_bytes = *declared.request_body_bytes;
    if (declared.response_header_bytes.has_value()) out.response_header_bytes = *declared.response_header_bytes;
    if (declared.response_body_bytes.has_value()) out.response_body_bytes = *declared.response_body_bytes;
    if (declared.timeout_ms.has_value()) out.timeout_ms = *declared.timeout_ms;
    return out;
}

std::string_view PluginManifestIssueCodeName(PluginManifestIssueCode code) {
    switch (code) {
        case PluginManifestIssueCode::JsonSyntax:
            return "json_syntax";
        case PluginManifestIssueCode::TopLevelNotObject:
            return "top_level_not_object";
        case PluginManifestIssueCode::VersionMissing:
            return "version_missing";
        case PluginManifestIssueCode::VersionUnsupported:
            return "version_unsupported";
        case PluginManifestIssueCode::EmbeddedLuaNeedsV2:
            return "embedded_lua_needs_v2";
        case PluginManifestIssueCode::V2KindUnsupported:
            return "v2_kind_unsupported";
        case PluginManifestIssueCode::FieldMissing:
            return "field_missing";
        case PluginManifestIssueCode::FieldInvalid:
            return "field_invalid";
        case PluginManifestIssueCode::EntryInvalid:
            return "entry_invalid";
        case PluginManifestIssueCode::HostInvalid:
            return "host_invalid";
        case PluginManifestIssueCode::LimitInvalid:
            return "limit_invalid";
        case PluginManifestIssueCode::SecretInvalid:
            return "secret_invalid";
        case PluginManifestIssueCode::DuplicateEntry:
            return "duplicate_entry";
    }
    return "field_invalid";
}

std::string PluginManifestIssue::Format() const {
    std::string out = "plugin.json";
    if (line > 0) {
        out += ":" + std::to_string(line);
        if (column > 0) {
            out += ":" + std::to_string(column);
        }
        out += " ";
    }
    out += "[";
    out += PluginManifestIssueCodeName(code);
    out += "] ";
    out += message;
    return out;
}

namespace {

// v2 的 runtime.entry 校验:只收插件目录内普通 .lua 文件(§5.3)。
std::optional<PluginManifestIssue> ValidateV2Entry(const std::string& entry,
                                                   const std::filesystem::path& canonical_dir,
                                                   std::string_view text) {
    if (entry.empty()) {
        return MakeIssue(PluginManifestIssueCode::FieldMissing, "runtime.entry 必填(v2)", text, "entry");
    }
    if (entry.front() == '/' || entry.front() == '\\') {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 须是相对插件根的路径: " + entry,
                         text, "entry");
    }
    if (entry.find("..") != std::string::npos) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 不收 .. 上跳段: " + entry, text,
                         "entry");
    }
    if (entry.find("${") != std::string::npos) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 不收占位符: " + entry, text, "entry");
    }
    const std::filesystem::path entry_path =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(entry.data()), entry.size()));
    const std::string extension = entry_path.extension().string();
    std::string extension_lower;
    for (const char c : extension) {
        extension_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (extension_lower != ".lua") {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 只收 .lua 文件: " + entry, text,
                         "entry");
    }
    const std::filesystem::path joined = canonical_dir / entry_path;
    // symlink 第一版明拒(免得加载后才越界)。
    std::error_code ec;
    const auto link_status = std::filesystem::symlink_status(joined, ec);
    if (!ec && std::filesystem::is_symlink(link_status)) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 是 symlink,第一版明拒: " + entry,
                         text, "entry");
    }
    const std::filesystem::path canonical_entry = std::filesystem::weakly_canonical(joined, ec);
    if (ec) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 路径解析失败: " + entry, text,
                         "entry");
    }
    if (!PathIsInside(canonical_dir, canonical_entry)) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid,
                         "runtime.entry canonical 后逃出了插件根: " + platform::PathToUtf8(canonical_entry), text,
                         "entry");
    }
    if (!std::filesystem::is_regular_file(canonical_entry, ec) || ec) {
        return MakeIssue(PluginManifestIssueCode::EntryInvalid, "runtime.entry 不是插件目录里的普通文件: " + entry,
                         text, "entry");
    }
    return std::nullopt;
}

// v2 permissions.network[] 逐项校验。
std::optional<PluginManifestIssue> ParseV2Network(const nlohmann::json& perms_json, PluginManifest& manifest,
                                                  std::string_view text) {
    const auto network_it = perms_json.find("network");
    if (network_it == perms_json.end() || network_it->is_null()) {
        return std::nullopt;  // 未声明 = 空 network 账,HTTP 一律 network_not_declared
    }
    if (!network_it->is_array()) {
        return MakeIssue(PluginManifestIssueCode::FieldInvalid, "v2 的 permissions.network 必须是数组(v1 的布尔记账已废)",
                         text, "network");
    }
    std::set<std::string> seen_destinations;
    for (std::size_t i = 0; i < network_it->size(); ++i) {
        const auto& item = (*network_it)[i];
        const std::string where = "permissions.network[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + " 必须是 object", text, "network");
        }
        NetworkPermission permission;
        // scheme:缺省 https;第一版只收 https。
        const auto scheme_it = item.find("scheme");
        if (scheme_it != item.end() && !scheme_it->is_null()) {
            if (!scheme_it->is_string()) {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + ".scheme 必须是字符串", text,
                                 "network");
            }
            std::string scheme = scheme_it->get<std::string>();
            for (char& c : scheme) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (scheme != "https") {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                 where + ".scheme 第一版只收 https(明文 HTTP 不做): " + scheme, text, "network");
            }
            permission.scheme = scheme;
        }
        // host:必填,精确 DNS 名,规范化后记账。
        const auto host_it = item.find("host");
        if (host_it == item.end() || !host_it->is_string()) {
            return MakeIssue(PluginManifestIssueCode::FieldMissing, where + ".host 必填(精确 DNS 名)", text,
                             "network");
        }
        auto host = NormalizeDnsHost(host_it->get<std::string>());
        if (!host.has_value()) {
            return MakeIssue(PluginManifestIssueCode::HostInvalid, where + ".host " + host.error(), text, "host");
        }
        permission.host = std::move(*host);
        // port:缺省 443;第一版只收 443。
        const auto port_it = item.find("port");
        if (port_it != item.end() && !port_it->is_null()) {
            if (!port_it->is_number_integer()) {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + ".port 必须是整数", text, "network");
            }
            if (port_it->get<int>() != 443) {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                 where + ".port 第一版只收 443: " + port_it->dump(), text, "network");
            }
            permission.port = port_it->get<int>();
        }
        // methods:必填非空;只收 GET/POST;大写化后去重。
        const auto methods_it = item.find("methods");
        if (methods_it == item.end() || !methods_it->is_array() || methods_it->empty()) {
            return MakeIssue(PluginManifestIssueCode::FieldMissing,
                             where + ".methods 必填且是非空数组(GET/POST)", text, "methods");
        }
        std::set<std::string> seen_methods;
        for (const auto& method : *methods_it) {
            if (!method.is_string()) {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + ".methods 的元素必须是字符串", text,
                                 "methods");
            }
            std::string m = method.get<std::string>();
            for (char& c : m) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (m != "GET" && m != "POST") {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                 where + ".methods 只收 GET/POST: " + m, text, "methods");
            }
            if (seen_methods.insert(m).second) {
                permission.methods.push_back(std::move(m));
            }
        }
        const std::string destination_key = permission.scheme + "://" + permission.host + ":" +
                                            std::to_string(permission.port);
        if (!seen_destinations.insert(destination_key).second) {
            return MakeIssue(PluginManifestIssueCode::DuplicateEntry,
                             where + " 与已声明目的地重复(" + destination_key + "),请合并声明", text, "network");
        }
        manifest.network_permissions.push_back(std::move(permission));
    }
    return std::nullopt;
}

// v2 permissions.secrets[] 逐项校验。
std::optional<PluginManifestIssue> ParseV2Secrets(const nlohmann::json& perms_json, PluginManifest& manifest,
                                                  std::string_view text) {
    const auto secrets_it = perms_json.find("secrets");
    if (secrets_it == perms_json.end() || secrets_it->is_null()) {
        return std::nullopt;
    }
    if (!secrets_it->is_array()) {
        return MakeIssue(PluginManifestIssueCode::FieldInvalid, "v2 的 permissions.secrets 必须是数组", text,
                         "secrets");
    }
    std::set<std::string> seen_ids;
    std::set<std::string> seen_envs;
    for (std::size_t i = 0; i < secrets_it->size(); ++i) {
        const auto& item = (*secrets_it)[i];
        const std::string where = "permissions.secrets[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + " 必须是 object", text, "secrets");
        }
        // inline value/default/${env:...} 全拒(§5.4:Secret 不写进 manifest)。
        for (const char* forbidden : {"value", "default"}) {
            if (item.contains(forbidden)) {
                return MakeIssue(PluginManifestIssueCode::SecretInvalid,
                                 where + " 不收 " + forbidden + "(manifest 不放 Secret 明文,值由宿主在调用期解析)",
                                 text, forbidden);
            }
        }
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string()) {
            return MakeIssue(PluginManifestIssueCode::FieldMissing, where + ".id 必填(Lua 侧逻辑名)", text, "secrets");
        }
        const std::string id = id_it->get<std::string>();
        if (!IsValidPluginIdentifier(id, 64)) {
            return MakeIssue(PluginManifestIssueCode::SecretInvalid,
                             where + ".id 只能是字母数字_-、字母数字开头、至多 64 字符: " + id, text, "secrets");
        }
        if (!seen_ids.insert(id).second) {
            return MakeIssue(PluginManifestIssueCode::DuplicateEntry, "Secret 逻辑 id 重复: " + id, text, "secrets");
        }
        const auto env_it = item.find("env");
        if (env_it == item.end() || !env_it->is_string()) {
            return MakeIssue(PluginManifestIssueCode::FieldMissing, where + ".env 必填(允许解析的环境变量名)", text,
                             "secrets");
        }
        const std::string env = env_it->get<std::string>();
        if (!IsValidEnvName(env)) {
            return MakeIssue(PluginManifestIssueCode::SecretInvalid,
                             where + ".env 只能是 [A-Za-z_][A-Za-z0-9_]*: " + env, text, "secrets");
        }
        if (env.find("${") != std::string::npos) {
            return MakeIssue(PluginManifestIssueCode::SecretInvalid, where + ".env 不收 ${...} 展开", text, "secrets");
        }
        if (!seen_envs.insert(env).second) {
            return MakeIssue(PluginManifestIssueCode::DuplicateEntry,
                             "Secret env 名重复(" + env + "):一个变量只供一个逻辑 id", text, "secrets");
        }
        SecretDeclaration declaration;
        declaration.id = id;
        declaration.env = env;
        declaration.required = true;
        const auto required_it = item.find("required");
        if (required_it != item.end() && !required_it->is_null()) {
            if (!required_it->is_boolean()) {
                return MakeIssue(PluginManifestIssueCode::FieldInvalid, where + ".required 必须是布尔", text,
                                 "secrets");
            }
            declaration.required = required_it->get<bool>();
        }
        manifest.secret_declarations.push_back(std::move(declaration));
    }
    return std::nullopt;
}

// v2 limits 段:六顶帽只许往下调(>0 且 ≤ 硬帽)。
std::optional<PluginManifestIssue> ParseV2Limits(const nlohmann::json& root, PluginManifest& manifest,
                                                 std::string_view text) {
    const auto limits_it = root.find("limits");
    if (limits_it == root.end() || limits_it->is_null()) {
        return std::nullopt;
    }
    if (!limits_it->is_object()) {
        return MakeIssue(PluginManifestIssueCode::FieldInvalid, "limits 必须是 object", text, "limits");
    }
    struct LimitField {
        const char* name;
        std::int64_t max;
        std::optional<std::int64_t> HttpLimits::*slot;
    };
    const LimitField fields[] = {
        {"http_url_bytes", kHttpUrlMaxBytes, &HttpLimits::url_bytes},
        {"http_request_header_bytes", kHttpRequestHeaderMaxBytes, &HttpLimits::request_header_bytes},
        {"http_request_bytes", kHttpRequestBodyMaxBytes, &HttpLimits::request_body_bytes},
        {"http_response_header_bytes", kHttpResponseHeaderMaxBytes, &HttpLimits::response_header_bytes},
        {"http_response_bytes", kHttpResponseBodyMaxBytes, &HttpLimits::response_body_bytes},
        {"http_timeout_ms", kHttpTimeoutMaxMs, &HttpLimits::timeout_ms},
    };
    for (const LimitField& field : fields) {
        const auto value_it = limits_it->find(field.name);
        if (value_it == limits_it->end() || value_it->is_null()) {
            continue;
        }
        if (!value_it->is_number_integer()) {
            return MakeIssue(PluginManifestIssueCode::LimitInvalid,
                             std::string("limits.") + field.name + " 必须是整数", text, field.name);
        }
        const std::int64_t value = value_it->get<std::int64_t>();
        if (value <= 0) {
            return MakeIssue(PluginManifestIssueCode::LimitInvalid,
                             std::string("limits.") + field.name + " 是 " + value_it->dump() +
                                 "(0 不表示无限,v2 里 0 与负数直接非法)",
                             text, field.name);
        }
        if (value > field.max) {
            return MakeIssue(PluginManifestIssueCode::LimitInvalid,
                             std::string("limits.") + field.name + " 是 " + value_it->dump() + ",越宿主硬上限 " +
                                 std::to_string(field.max) + "(只许下调)",
                             text, field.name);
        }
        manifest.http_limits.*field.slot = value;
    }
    return std::nullopt;
}

}  // namespace

std::expected<PluginManifest, PluginManifestIssue> ParsePluginManifestDetailed(
    const std::string& manifest_json, const std::filesystem::path& plugin_dir) {
    // 解析失败即坏,不宽化。语法错要精确行列:走异常口拿 parse_error 的字节
    // 偏移(nlohmann 的 DOM 不存位置,只有这一处能给准坐标)。
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(manifest_json);
    } catch (const nlohmann::json::parse_error& e) {
        PluginManifestIssue issue;
        issue.code = PluginManifestIssueCode::JsonSyntax;
        issue.message = "plugin.json 不是合法 JSON: " + std::string(e.what());
        OffsetToLineColumn(manifest_json, e.byte, issue.line, issue.column);
        return std::unexpected(issue);
    }
    if (!root.is_object()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::TopLevelNotObject, "plugin.json 顶层必须是 object", manifest_json));
    }

    // manifest_version:只认 1 与 2。
    const auto version_value = root.find("manifest_version");
    if (version_value == root.end() || !version_value->is_number_integer()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::VersionMissing, "缺 manifest_version 或不是整数", manifest_json,
                      "manifest_version"));
    }
    const int manifest_version = version_value->get<int>();
    if (manifest_version != kPluginManifestVersion && manifest_version != kPluginManifestVersionV2) {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::VersionUnsupported,
                                         "manifest_version=" + version_value->dump() + " 宿主只认 1 或 2,不静默宽化",
                                         manifest_json, "manifest_version"));
    }

    // id/version:字符集与长度规矩。
    auto id = RequireString(root, "id");
    if (!id.has_value()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldMissing, id.error(), manifest_json, "id"));
    }
    if (!IsValidPluginIdentifier(*id, 64)) {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                         "id 只能是字母数字_-、字母数字开头、至多 64 字符: " + *id, manifest_json,
                                         "id"));
    }
    auto version = RequireString(root, "version");
    if (!version.has_value()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldMissing, version.error(), manifest_json, "version"));
    }
    if (!IsValidPluginVersion(*version, 32)) {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                         "version 只能是字母数字与 ._- 字符: " + *version, manifest_json, "version"));
    }
    // language:可选;只作诊断/展示,删掉也能跑。写了就查一下形状,防 typo
    // 悄悄变成"未知语言"。
    auto language = OptionalString(root, "language");
    if (!language.has_value()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldInvalid, language.error(), manifest_json, "language"));
    }
    if (language->has_value() && !IsValidPluginIdentifier(**language, 32)) {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                         "language 只能是字母数字_-: " + **language, manifest_json, "language"));
    }

    // runtime 段。
    const auto runtime_it = root.find("runtime");
    if (runtime_it == root.end() || !runtime_it->is_object()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldMissing, "缺 runtime 段或不是 object", manifest_json, "runtime"));
    }
    const auto& runtime_json = *runtime_it;
    auto kind_text = RequireString(runtime_json, "kind");
    if (!kind_text.has_value()) {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldMissing, kind_text.error(), manifest_json, "kind"));
    }
    RuntimeKind kind;
    if (*kind_text == "process") {
        kind = RuntimeKind::Process;
        if (manifest_version == kPluginManifestVersionV2) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::V2KindUnsupported,
                                             "manifest v2 第一版只收 embedded-lua(process 插件照旧走 v1)", manifest_json,
                                             "kind"));
        }
    } else if (*kind_text == "embedded-lua") {
        kind = RuntimeKind::EmbeddedLua;
        if (manifest_version == kPluginManifestVersion) {
            // §5.1:v1 写 embedded-lua 明报需 v2,不拿半份合同猜。
            return std::unexpected(
                MakeIssue(PluginManifestIssueCode::EmbeddedLuaNeedsV2,
                          "runtime.kind=embedded-lua(manifest-backed Lua)需 manifest_version 2,v1 不收半份合同",
                          manifest_json, "kind"));
        }
    } else if (*kind_text == "native-library") {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                         "runtime.kind=native-library 属后续批次,当前宿主不支持,整件拒绝",
                                         manifest_json, "kind"));
    } else {
        return std::unexpected(
            MakeIssue(PluginManifestIssueCode::FieldInvalid, "runtime.kind 不认得: " + *kind_text, manifest_json,
                      "kind"));
    }

    PluginManifest manifest;
    manifest.manifest_version = manifest_version;
    manifest.id = std::move(*id);
    manifest.version = std::move(*version);
    manifest.language = language->value_or("");
    manifest.kind = kind;

    // 插件目录:canonical(不存在/不可达时按 weakly_canonical 的尽力而为
    // 口径,拿词法绝对路径比一比——目录是宿主自己枚举出来的,枚举得到就
    // 存在;真不存在的极端情况留给路径校验去拦)。
    std::error_code ec;
    std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(plugin_dir, ec);
    if (ec || canonical_dir.empty()) {
        canonical_dir = std::filesystem::absolute(plugin_dir, ec);
        if (ec) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "插件目录路径解析失败: " + platform::PathToUtf8(plugin_dir),
                                             manifest_json));
        }
    }
    manifest.plugin_dir = canonical_dir;

    if (kind == RuntimeKind::EmbeddedLua) {
        // v2 embedded-lua:runtime 段只收 kind 与 entry(command/args/timeout
        // 是 process 的字,混进 v2 即坏)。
        for (const char* stray : {"command", "args", "timeout_ms"}) {
            if (runtime_json.contains(stray)) {
                return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                 std::string("v2 的 runtime 段不收 ") + stray +
                                                     "(那是 process 合同的字)",
                                                 manifest_json, stray));
            }
        }
        auto entry = RequireString(runtime_json, "entry");
        if (!entry.has_value()) {
            return std::unexpected(
                MakeIssue(PluginManifestIssueCode::FieldMissing, entry.error(), manifest_json, "entry"));
        }
        if (auto problem = ValidateV2Entry(*entry, manifest.plugin_dir, manifest_json); problem.has_value()) {
            return std::unexpected(*problem);
        }
        manifest.runtime_entry = std::move(*entry);

        // permissions 段(v2):network 数组 + secrets 数组;v1 的 env
        // allowlist 不进 v2(Secret 走声明,不递子进程)。
        const auto perms_it = root.find("permissions");
        if (perms_it != root.end() && !perms_it->is_null()) {
            if (!perms_it->is_object()) {
                return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid, "permissions 必须是 object",
                                                 manifest_json, "permissions"));
            }
            if (perms_it->contains("env")) {
                return std::unexpected(MakeIssue(
                    PluginManifestIssueCode::FieldInvalid,
                    "v2 的 permissions 不收 env allowlist(Secret 按 secrets 声明解析,不递子进程)", manifest_json,
                    "env"));
            }
            // v1 的布尔 network 在 v2 是类型错(ParseV2Network 里报)。
            if (auto problem = ParseV2Network(*perms_it, manifest, manifest_json); problem.has_value()) {
                return std::unexpected(*problem);
            }
            if (auto problem = ParseV2Secrets(*perms_it, manifest, manifest_json); problem.has_value()) {
                return std::unexpected(*problem);
            }
        }

        // limits 段:六顶帽,只许下调。
        if (auto problem = ParseV2Limits(root, manifest, manifest_json); problem.has_value()) {
            return std::unexpected(*problem);
        }
    }

    if (kind == RuntimeKind::Process) {
        auto command = RequireString(runtime_json, "command");
        if (!command.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldMissing, command.error(), manifest_json,
                                             "command"));
        }
        if (command->find("${plugin_dir}") != std::string::npos) {
            // command 里也允许 ${plugin_dir}(自带给全路径的可执行文件是正路),
            // canonical 校验在替换后统一做。
        }
        auto expanded_command = ExpandPluginDirPlaceholder(*command, manifest.plugin_dir);
        if (!expanded_command.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid, expanded_command.error(),
                                             manifest_json, "command"));
        }
        if (expanded_command->empty()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid, "runtime.command 替换后是空串",
                                             manifest_json, "command"));
        }
        manifest.argv.push_back(std::move(*expanded_command));

        const auto args_it = runtime_json.find("args");
        if (args_it != runtime_json.end()) {
            if (!args_it->is_array()) {
                return std::unexpected(
                    MakeIssue(PluginManifestIssueCode::FieldInvalid, "runtime.args 必须是数组", manifest_json, "args"));
            }
            for (const auto& arg : *args_it) {
                if (!arg.is_string()) {
                    return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                     "runtime.args 的元素必须是字符串(不经 shell,原样直传)",
                                                     manifest_json, "args"));
                }
                auto expanded = ExpandPluginDirPlaceholder(arg.get<std::string>(), manifest.plugin_dir);
                if (!expanded.has_value()) {
                    return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid, expanded.error(),
                                                     manifest_json, "args"));
                }
                manifest.argv.push_back(std::move(*expanded));
            }
        }

        // timeout_ms:可选,缺省 30s;0 是合法值(显式不设墙,不推荐)。
        const auto timeout_it = runtime_json.find("timeout_ms");
        if (timeout_it != runtime_json.end() && !timeout_it->is_null()) {
            if (!timeout_it->is_number_integer() || timeout_it->get<std::int64_t>() < 0) {
                return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                 "runtime.timeout_ms 必须是非负整数", manifest_json, "timeout_ms"));
            }
            manifest.timeout_ms = static_cast<int>(timeout_it->get<std::int64_t>());
        }

        // 路径校验:凡带 ${plugin_dir} 的段,canonical 后须仍位于插件目录内。
        // 绝对路径/相对可执行名(如 "python3",走 PATH)不在此列——那是
        // 明确批准的外部 executable 路子。这里复查原始声明,只有用了
        // 占位符的才圈在目录里。
        const auto check_arg = [&](const std::string& raw, const std::string& expanded,
                                   PluginManifestIssueCode code) -> std::optional<PluginManifestIssue> {
            if (raw.find("${plugin_dir}") == std::string::npos) {
                return std::nullopt;
            }
            std::error_code path_ec;
            const std::filesystem::path expanded_path =
                std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(
                                                      reinterpret_cast<const char8_t*>(expanded.data()),
                                                      expanded.size())),
                                                  path_ec);
            if (path_ec) {
                return MakeIssue(code, "路径解析失败: " + expanded, manifest_json, "args");
            }
            if (!PathIsInside(manifest.plugin_dir, expanded_path)) {
                return MakeIssue(code, "${plugin_dir} 替换后逃出了插件目录: " + expanded, manifest_json, "args");
            }
            return std::nullopt;
        };
        if (auto problem = check_arg(*command, manifest.argv[0], PluginManifestIssueCode::FieldInvalid);
            problem.has_value()) {
            return std::unexpected(*problem);
        }
        if (args_it != runtime_json.end()) {
            for (std::size_t i = 0; i < args_it->size(); ++i) {
                const std::string raw = (*args_it)[i].get<std::string>();
                if (auto problem =
                        check_arg(raw, manifest.argv[i + 1], PluginManifestIssueCode::FieldInvalid);
                    problem.has_value()) {
                    return std::unexpected(*problem);
                }
            }
        }
    }

    // permissions 段(v1 process 只记账;v2 已在 embedded-lua 分支收账)。
    if (kind == RuntimeKind::Process) {
        const auto perms_it = root.find("permissions");
        if (perms_it != root.end() && !perms_it->is_null()) {
            if (!perms_it->is_object()) {
                return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid, "permissions 必须是 object",
                                                 manifest_json, "permissions"));
            }
            const auto network_it = perms_it->find("network");
            if (network_it != perms_it->end() && !network_it->is_null()) {
                if (!network_it->is_boolean()) {
                    return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                     "permissions.network 必须是布尔", manifest_json, "network"));
                }
                manifest.network_allowed = network_it->get<bool>();
            }
            const auto env_it = perms_it->find("env");
            if (env_it != perms_it->end() && !env_it->is_null()) {
                if (!env_it->is_array()) {
                    return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                     "permissions.env 必须是数组", manifest_json, "env"));
                }
                for (const auto& name : *env_it) {
                    if (!name.is_string() || name.get<std::string>().empty()) {
                        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                                         "permissions.env 的元素必须是非空字符串", manifest_json,
                                                         "env"));
                    }
                    manifest.env_allowlist.push_back(name.get<std::string>());
                }
            }
        }
    }

    // tools 段:至少一件;逐件强校验;同插件内重名即拒。
    const auto tools_it = root.find("tools");
    if (tools_it == root.end() || !tools_it->is_array() || tools_it->empty()) {
        return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldMissing,
                                         "tools 必须是非空数组(插件至少声明一件工具)", manifest_json, "tools"));
    }
    std::set<std::string> seen_names;
    for (std::size_t i = 0; i < tools_it->size(); ++i) {
        const auto& tool_json = (*tools_it)[i];
        if (!tool_json.is_object()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) + "] 必须是 object", manifest_json,
                                             "tools"));
        }
        auto name = RequireString(tool_json, "name");
        if (!name.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldMissing,
                                             "tools[" + std::to_string(i) + "]: " + name.error(), manifest_json,
                                             "name"));
        }
        if (!IsValidPluginIdentifier(*name, 64)) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) +
                                                 "].name 只能是字母数字_-、字母数字开头、至多 64 字符: " + *name,
                                             manifest_json, "name"));
        }
        if (seen_names.count(*name) != 0) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::DuplicateEntry,
                                             "同一插件里工具重名: " + *name, manifest_json, "name"));
        }
        seen_names.insert(*name);

        auto description = OptionalString(tool_json, "description");
        if (!description.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) + "]: " + description.error(),
                                             manifest_json, "description"));
        }
        auto entry = OptionalString(tool_json, "entry");
        if (!entry.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) + "]: " + entry.error(), manifest_json,
                                             "entry"));
        }
        // entry 缺省 = 工具短名(单脚本一件工具时省得抄两遍)。
        std::string entry_value = entry->value_or(*name);
        if (!IsValidPluginIdentifier(entry_value, 64)) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) + "].entry 只能是字母数字_-: " +
                                                 entry_value,
                                             manifest_json, "entry"));
        }

        // input_schema:必填、必须是 object、必须是认得的子集形状。
        // Schema 坏了拒绝整件插件——这是与 DLL 路径"退化宽 object"相反的
        // 刻意选择(单子「Schema 的方向不能倒」)。
        const auto schema_it = tool_json.find("input_schema");
        if (schema_it == tool_json.end() || !schema_it->is_object()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldMissing,
                                             "tools[" + std::to_string(i) +
                                                 "].input_schema 缺失或不是 object"
                                                 "(Schema 坏了拒绝加载,不悄悄宽化)",
                                             manifest_json, "input_schema"));
        }
        if (auto shape_error = ValidateSchemaShape(*schema_it, 0); shape_error.has_value()) {
            return std::unexpected(MakeIssue(PluginManifestIssueCode::FieldInvalid,
                                             "tools[" + std::to_string(i) + "].input_schema 形状不认得: " +
                                                 *shape_error,
                                             manifest_json, "input_schema"));
        }

        PluginDefinition def;
        def.plugin_id = manifest.id;
        def.name = *name;
        def.full_name = BuildPluginToolName(manifest.id, *name);
        def.description = description->value_or("");
        def.entry = std::move(entry_value);
        def.input_schema = *schema_it;
        manifest.tools.push_back(std::move(def));
    }

    return manifest;
}

std::expected<PluginManifest, std::string> ParsePluginManifest(const std::string& manifest_json,
                                                               const std::filesystem::path& plugin_dir) {
    // 老口子:人话错误 = 详细问题的 Format()(码 + 坐标 + 人话)。
    auto result = ParsePluginManifestDetailed(manifest_json, plugin_dir);
    if (!result.has_value()) {
        return std::unexpected(result.error().Format());
    }
    return std::expected<PluginManifest, std::string>(std::move(result.value()));
}

// ---------------------------------------------------------------------------
// 进程协议帧
// ---------------------------------------------------------------------------

namespace plugin_protocol {

nlohmann::json SerializeRequest(const ProcessRequest& request) {
    nlohmann::json context = nlohmann::json::object();
    if (!request.context_cwd.empty()) {
        context["cwd"] = request.context_cwd;
    }
    return nlohmann::json{
        {"protocol", request.protocol},
        {"call_id", request.call_id},
        {"plugin", request.plugin},
        {"tool", request.tool},
        {"entry", request.entry},
        {"arguments", request.arguments},
        {"context", std::move(context)},
    };
}

ParsedResponse ParseResponse(std::string_view stdout_bytes, std::string_view expected_call_id) {
    ParsedResponse out;
    // 第一关:合法 UTF-8。不做编码猜测(GBK 转一把那种事是 hooks 的明示
    // 解码层的事),协议线就是 UTF-8,坏了即坏。
    const std::string bytes(stdout_bytes);
    if (!platform::IsValidUtf8(bytes)) {
        out.status = PluginErrorCode::BadUtf8;
        out.detail = "stdout 不是合法 UTF-8";
        return out;
    }
    // 第二关:恰好一份合法 JSON,整体 parse。前后混日志在这里就露馅——
    // 不从字堆里猜最后一枚 JSON。
    nlohmann::json root = nlohmann::json::parse(bytes, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "stdout 不是恰好一份合法 JSON object(混日志/多帧/坏文法都算协议错)";
        return out;
    }
    // 第三关:call_id 对得上。
    const auto call_it = root.find("call_id");
    if (call_it == root.end() || !call_it->is_string()) {
        out.status = PluginErrorCode::CallIdMismatch;
        out.detail = "响应缺 call_id 或不是字符串";
        return out;
    }
    const std::string got_call_id = call_it->get<std::string>();
    if (got_call_id != expected_call_id) {
        out.status = PluginErrorCode::CallIdMismatch;
        out.detail = "响应 call_id 对不上(要 " + std::string(expected_call_id) + ",来的是 " + got_call_id + ")";
        return out;
    }
    // protocol:v1=1、v2=2 都收(工具结果图片回喂单的版本协商:宿主请求
    // 说 2,v1 旧插件照旧回 1,两边都是合法响应)。再高的版本明说不认。
    const auto protocol_it = root.find("protocol");
    if (protocol_it == root.end() || !protocol_it->is_number_integer()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "响应 protocol 缺失或不是整数";
        return out;
    }
    const int protocol = protocol_it->get<int>();
    if (protocol != plugin_protocol::kProtocolVersionV1 && protocol != plugin_protocol::kProtocolVersion) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "响应 protocol 不认得(只收 1 或 2): " + std::to_string(protocol);
        return out;
    }
    // ok:必填布尔。
    const auto ok_it = root.find("ok");
    if (ok_it == root.end() || !ok_it->is_boolean()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "响应缺 ok 或不是布尔";
        return out;
    }
    out.response.protocol = protocol_it->get<int>();
    out.response.call_id = got_call_id;
    out.response.ok = ok_it->get<bool>();

    if (!out.response.ok) {
        const auto error_it = root.find("error");
        if (error_it != root.end() && error_it->is_object()) {
            const auto code_it = error_it->find("code");
            if (code_it != error_it->end() && code_it->is_string()) {
                out.response.error_code = code_it->get<std::string>();
            }
            const auto message_it = error_it->find("message");
            if (message_it != error_it->end() && message_it->is_string()) {
                out.response.error_message = message_it->get<std::string>();
            }
        }
        if (out.response.error_message.empty()) {
            out.response.error_message = "(插件没给错误说明)";
        }
        return out;  // status 保持 Ok:插件自报失败是合法响应,分型在调用方
    }

    // content:必填数组;v1 只认 type=text;v2 另收 type=image(工具结果
    // 图片回喂)。别的不静默转字符串。
    const auto content_it = root.find("content");
    if (content_it == root.end() || !content_it->is_array()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "ok=true 的响应缺 content 或 content 不是数组";
        return out;
    }
    std::string text;
    std::vector<ResponseImage> images;
    for (const auto& item : *content_it) {
        if (!item.is_object()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "content 的元素必须是 object";
            return out;
        }
        const auto type_it = item.find("type");
        if (type_it == item.end() || !type_it->is_string()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "content 的元素缺 type";
            return out;
        }
        const std::string type_name = type_it->get<std::string>();
        if (type_name == "image") {
            if (protocol != plugin_protocol::kProtocolVersion) {
                // v1 响应冒出 image 块:违反 v1 合同,照旧 UnknownContent。
                out.status = PluginErrorCode::UnknownContent;
                out.detail = "content type 不认得(v1 只认 text): " + type_name;
                return out;
            }
            ResponseImage image;
            const auto mime_it = item.find("mime_type");
            if (mime_it == item.end() || !mime_it->is_string()) {
                out.status = PluginErrorCode::BadJson;
                out.detail = "type=image 的元素缺 mime_type 或不是字符串";
                return out;
            }
            image.mime_type = mime_it->get<std::string>();
            const auto data_it = item.find("data");
            const auto path_it = item.find("path");
            const bool has_data = data_it != item.end() && !data_it->is_null();
            const bool has_path = path_it != item.end() && !path_it->is_null();
            if (has_data == has_path) {
                out.status = PluginErrorCode::BadJson;
                out.detail = "type=image 的元素须恰给 data(base64)或 path 之一";
                return out;
            }
            if (has_data) {
                if (!data_it->is_string()) {
                    out.status = PluginErrorCode::BadJson;
                    out.detail = "type=image 的 data 不是字符串";
                    return out;
                }
                image.data_base64 = data_it->get<std::string>();
            } else {
                if (!path_it->is_string()) {
                    out.status = PluginErrorCode::BadJson;
                    out.detail = "type=image 的 path 不是字符串";
                    return out;
                }
                image.path = path_it->get<std::string>();
            }
            images.push_back(std::move(image));
            continue;
        }
        if (type_name != "text") {
            out.status = PluginErrorCode::UnknownContent;
            out.detail = "content type 不认得(v1 只认 text): " + type_name;
            return out;
        }
        const auto text_it = item.find("text");
        if (text_it == item.end() || !text_it->is_string()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "type=text 的元素缺 text 或不是字符串";
            return out;
        }
        if (!text.empty()) {
            text += "\n";
        }
        text += text_it->get<std::string>();
    }
    out.response.text = std::move(text);
    out.response.images = std::move(images);
    // structured:可选,原样收。
    const auto structured_it = root.find("structured");
    if (structured_it != root.end() && !structured_it->is_null()) {
        out.response.structured = *structured_it;
    }
    return out;
}

}  // namespace plugin_protocol

// ---------------------------------------------------------------------------
// 入参校验(调用前统一跑的子集)
// ---------------------------------------------------------------------------

namespace {

bool JsonTypeMatches(const nlohmann::json& value, const std::string& expected) {
    if (expected == "string") {
        return value.is_string();
    }
    if (expected == "number") {
        return value.is_number();
    }
    if (expected == "integer") {
        return value.is_number_integer();
    }
    if (expected == "boolean") {
        return value.is_boolean();
    }
    if (expected == "array") {
        return value.is_array();
    }
    if (expected == "object") {
        return value.is_object();
    }
    if (expected == "null") {
        return value.is_null();
    }
    return true;
}

// UTF-8 码点计数(长度界按码点算,与 JSON Schema 语义一致)。入参在协议
// 层已保证合法 UTF-8,这里只数不验。
std::size_t CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::optional<std::string> ValidateValueAgainstSchema(const nlohmann::json& value, const nlohmann::json& schema,
                                                      const std::string& path, int depth) {
    if (depth > 32) {
        return path + ": 嵌套超过 32 层";
    }
    if (!schema.is_object()) {
        return std::nullopt;  // 没给 schema 的位置无从校验
    }
    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string expected = schema["type"].get<std::string>();
        if (!JsonTypeMatches(value, expected)) {
            return path + " 的类型应是 " + expected;
        }
    }
    if (schema.contains("const")) {
        if (value != schema["const"]) {
            return path + " 必须恒等于 " + schema["const"].dump();
        }
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool in_enum = false;
        for (const auto& allowed : schema["enum"]) {
            if (value == allowed) {
                in_enum = true;
                break;
            }
        }
        if (!in_enum) {
            return path + " 的取值不在枚举表里";
        }
    }
    // 数值界。
    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() &&
            number < schema["minimum"].get<double>()) {
            return path + " 小于最小值 " + schema["minimum"].dump();
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() &&
            number > schema["maximum"].get<double>()) {
            return path + " 大于最大值 " + schema["maximum"].dump();
        }
    }
    // 字符串长度界(码点)。
    if (value.is_string()) {
        const std::size_t length = CodePointCount(value.get_ref<const std::string&>());
        if (schema.contains("minLength") && schema["minLength"].is_number_integer() &&
            length < static_cast<std::size_t>(schema["minLength"].get<std::int64_t>())) {
            return path + " 短于 minLength " + schema["minLength"].dump();
        }
        if (schema.contains("maxLength") && schema["maxLength"].is_number_integer() &&
            length > static_cast<std::size_t>(schema["maxLength"].get<std::int64_t>())) {
            return path + " 长于 maxLength " + schema["maxLength"].dump();
        }
    }
    // 数组:items 逐项 + minItems/maxItems。
    if (value.is_array()) {
        const std::size_t size = value.size();
        if (schema.contains("minItems") && schema["minItems"].is_number_integer() &&
            size < static_cast<std::size_t>(schema["minItems"].get<std::int64_t>())) {
            return path + " 的元素个数少于 minItems " + schema["minItems"].dump();
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer() &&
            size > static_cast<std::size_t>(schema["maxItems"].get<std::int64_t>())) {
            return path + " 的元素个数多于 maxItems " + schema["maxItems"].dump();
        }
        if (schema.contains("items") && schema["items"].is_object()) {
            for (std::size_t i = 0; i < size; ++i) {
                if (auto problem = ValidateValueAgainstSchema(value[i], schema["items"],
                                                               path + "[" + std::to_string(i) + "]", depth + 1);
                    problem.has_value()) {
                    return problem;
                }
            }
        }
    }
    // 对象:required + properties 逐键 + additionalProperties。
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& key : schema["required"]) {
                if (key.is_string() && !value.contains(key.get<std::string>())) {
                    return path + " 缺少必填字段: " + key.get<std::string>();
                }
            }
        }
        const bool has_properties = schema.contains("properties") && schema["properties"].is_object();
        const bool ap_false = schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                              !schema["additionalProperties"].get<bool>();
        const bool ap_schema = schema.contains("additionalProperties") && schema["additionalProperties"].is_object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child_path = path.empty() ? it.key() : path + "." + it.key();
            if (has_properties) {
                const auto prop = schema["properties"].find(it.key());
                if (prop != schema["properties"].end()) {
                    if (auto problem =
                            ValidateValueAgainstSchema(it.value(), prop.value(), child_path, depth + 1);
                        problem.has_value()) {
                        return problem;
                    }
                    continue;
                }
            }
            if (ap_false) {
                return child_path + " 不在声明的字段里(additionalProperties=false)";
            }
            if (ap_schema) {
                if (auto problem = ValidateValueAgainstSchema(it.value(), schema["additionalProperties"], child_path,
                                                               depth + 1);
                    problem.has_value()) {
                    return problem;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> ValidateArgumentsAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema) {
    if (!schema.is_object()) {
        return std::nullopt;  // 没有有效 schema,无从校验(实现层仍须防御)
    }
    // 顶层通常声明 type=object;没声明也按对象验 required/properties——
    // 模型入参天然是 object。
    if (!input.is_object()) {
        return "入参必须是 object";
    }
    return ValidateValueAgainstSchema(input, schema, "", 0);
}

}  // namespace lubancode::runtime
