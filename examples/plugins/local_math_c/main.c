/* local_math_c — C 可执行文件 process 插件示例(plugins 单第 9 步)。
 *
 * 源码不能直接跑:.c 是源码,运行前总要编译(单子「先答五个问题」四)。
 * 这条正路是作者预编成独立 executable,manifest 的 command 指产物——
 * 用户机器不需要编译器。
 *
 * 编译(任选其一):
 *   gcc -O2 -o local_math_c main.c
 *   cl /O2 main.c
 *
 * 协议 v1:stdin 恰好一份 JSON,stdout 恰好一份 JSON,退出即结束。
 * 示例级的 JSON 解析( sscanf 抠字段)只够演示;正经插件用 jsmn 之类
 * 小解析库,别在协议线上手搓。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kMaxInputBytes (4 * 1024 * 1024)

/* 从请求 JSON 里抠 "a":N 与 "b":N(示例级:只认数字字段,不支持科学计数
 * 之外的花样)。返回 0 = 两枚都拿到。 */
static int extract_numbers(const char* json, double* a, double* b) {
    const char* pa = strstr(json, "\"a\"");
    const char* pb = strstr(json, "\"b\"");
    if (pa == NULL || pb == NULL) return -1;
    if (sscanf(pa, "\"a\":%lf", a) != 1) return -1;
    if (sscanf(pb, "\"b\":%lf", b) != 1) return -1;
    return 0;
}

/* 抠 call_id(示例级:不带转义的普通串)。 */
static void extract_call_id(const char* json, char* out, size_t cap) {
    const char* p = strstr(json, "\"call_id\"");
    out[0] = '\0';
    if (p == NULL) return;
    p = strchr(p + 10, '"');
    if (p == NULL) return;
    ++p;
    size_t n = 0;
    while (p[n] != '"' && p[n] != '\0' && n + 1 < cap) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
}

int main(void) {
    char* buf = (char*)malloc(kMaxInputBytes + 1);
    if (buf == NULL) return 1;
    const size_t n = fread(buf, 1, kMaxInputBytes, stdin);
    buf[n] = '\0';

    char call_id[128];
    extract_call_id(buf, call_id, sizeof(call_id));

    double a = 0, b = 0;
    if (extract_numbers(buf, &a, &b) != 0) {
        printf("{\"protocol\":1,\"call_id\":\"%s\",\"ok\":false,"
               "\"error\":{\"code\":\"bad_input\",\"message\":\"找不到 a/b 数字字段\"}}",
               call_id);
        free(buf);
        return 0;
    }

    /* 结果只写 stdout(结果专线);有话要说写 stderr。 */
    printf("{\"protocol\":1,\"call_id\":\"%s\",\"ok\":true,"
           "\"content\":[{\"type\":\"text\",\"text\":\"%g\"}],\"structured\":%g}",
           call_id, a + b, a + b);
    free(buf);
    return 0;
}
