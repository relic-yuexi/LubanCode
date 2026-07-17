/* hello_plugin - lubancode C ABI 插件示例(M7)。
 *
 * 提供一个 reverse_text 工具:入参 {"text": "..."},把文本按 UTF-8 字符
 * 倒序返回(按字符不按字节,中文倒完还是合法 UTF-8)。
 *
 * 编译(独立小工程,见同目录 CMakeLists.txt):
 *   cmake -S . -B build && cmake --build build --config Release
 * 编出来的 hello_plugin.dll 放进 <主目录>/.lubancode/plugins/ 即挂载,
 * 工具名是 plugin__hello_plugin__reverse_text。
 *
 * 内存规矩:execute 返回的 content 用本 DLL 的 malloc 分配,宿主拷贝完
 * 回调 free_result,由本 DLL 自己 free——谁分配谁释放,不跨堆。
 */
#include <stdlib.h>
#include <string.h>

#include "luban_plugin.h"

/* ------------------------------------------------------------------ */
/* 极简 JSON 取值:从 {"text":"..."} 里抠出 text 字段的字符串值。
 * 只处理示例需要的场景(顶层对象里一个字符串字段),支持常见转义和
 * \uXXXX(含代理对)。返回 malloc 的字符串,失败返回 NULL。          */
/* ------------------------------------------------------------------ */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 把码点写成 UTF-8,返回写入字节数。 */
static int utf8_encode(unsigned long cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static unsigned long parse_hex4(const char* p) {
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        int h = hex_val(p[i]);
        if (h < 0) return (unsigned long)-1;
        v = (v << 4) | (unsigned long)h;
    }
    return v;
}

static char* extract_text_field(const char* json) {
    const char* key = strstr(json, "\"text\"");
    const char* p;
    char* out;
    size_t out_len = 0;
    if (key == NULL) return NULL;
    p = key + 6;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') return NULL;
    ++p;

    /* 解码后的长度不会超过原文长度,按原文长度分配足够了。 */
    out = (char*)malloc(strlen(p) + 1);
    if (out == NULL) return NULL;

    while (*p != '\0' && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"': out[out_len++] = '"'; ++p; break;
                case '\\': out[out_len++] = '\\'; ++p; break;
                case '/': out[out_len++] = '/'; ++p; break;
                case 'n': out[out_len++] = '\n'; ++p; break;
                case 't': out[out_len++] = '\t'; ++p; break;
                case 'r': out[out_len++] = '\r'; ++p; break;
                case 'b': out[out_len++] = '\b'; ++p; break;
                case 'f': out[out_len++] = '\f'; ++p; break;
                case 'u': {
                    unsigned long cp = parse_hex4(p + 1);
                    if (cp == (unsigned long)-1) { free(out); return NULL; }
                    p += 5;
                    /* 代理对:高代理后面必须跟 \uDC00-\uDFFF 的低代理。 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                        unsigned long low = parse_hex4(p + 2);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            p += 6;
                        }
                    }
                    out_len += (size_t)utf8_encode(cp, out + out_len);
                    break;
                }
                default: free(out); return NULL; /* 不认得的转义,放弃 */
            }
        } else {
            out[out_len++] = *p++;
        }
    }
    if (*p != '"') { free(out); return NULL; }
    out[out_len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* reverse_text:按 UTF-8 字符倒序。                                   */
/* ------------------------------------------------------------------ */

/* 一个 UTF-8 字符占几个字节(按首字节判断,坏字节当 1 个)。 */
static size_t utf8_char_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

static char* reverse_utf8(const char* text) {
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    size_t write = len; /* 从尾往前填 */
    size_t i = 0;
    if (out == NULL) return NULL;
    while (i < len) {
        size_t char_len = utf8_char_len((unsigned char)text[i]);
        if (i + char_len > len) char_len = 1; /* 尾巴上截断的坏序列,按单字节处理 */
        write -= char_len;
        memcpy(out + write, text + i, char_len);
        i += char_len;
    }
    out[len] = '\0';
    return out;
}

static luban_tool_result make_error(const char* message) {
    luban_tool_result r;
    size_t n = strlen(message);
    char* copy = (char*)malloc(n + 1);
    if (copy != NULL) memcpy(copy, message, n + 1);
    r.content = copy;
    r.is_error = 1;
    return r;
}

static luban_tool_result reverse_execute(const char* input_json) {
    luban_tool_result r;
    char* text;
    char* reversed;
    if (input_json == NULL) return make_error("入参是空的");
    text = extract_text_field(input_json);
    if (text == NULL) return make_error("入参里找不到 text 字段(要 {\"text\": \"...\"})");
    reversed = reverse_utf8(text);
    free(text);
    if (reversed == NULL) return make_error("内存分配失败");
    r.content = reversed;
    r.is_error = 0;
    return r;
}

static void free_result(luban_tool_result* result) {
    if (result != NULL && result->content != NULL) {
        free((void*)result->content);
        result->content = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* manifest + 入口                                                     */
/* ------------------------------------------------------------------ */

static const luban_tool_def k_tools[] = {
    {
        "reverse_text",
        "把输入文本按字符倒序返回(UTF-8 安全,中文按字不按字节)。",
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\","
        "\"description\":\"要倒序的文本\"}},\"required\":[\"text\"]}",
        reverse_execute,
        free_result,
    },
};

static const luban_plugin_manifest k_manifest = {
    LUBAN_PLUGIN_API_VERSION,
    (int)(sizeof(k_tools) / sizeof(k_tools[0])),
    k_tools,
};

#ifdef _WIN32
__declspec(dllexport)
#endif
const luban_plugin_manifest* luban_plugin_entry(void) {
    return &k_manifest;
}
