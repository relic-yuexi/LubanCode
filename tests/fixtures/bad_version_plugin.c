/* 测试夹具:abi_tag 填错的插件——宿主必须打警告跳过它,不挂载、不崩。
 * (abi_tag 是 manifest 首字段的值;9999 不在 {1, 2} 里,版本判别即拒。) */
#include <stddef.h>

#include "luban_plugin.h"

static luban_tool_result noop_execute(const char* input_json) {
    luban_tool_result r;
    (void)input_json;
    r.content = NULL; /* 静态数据都不给,反正永远不该被调到 */
    r.is_error = 1;
    return r;
}

static void noop_free(luban_tool_result* result) { (void)result; }

static const luban_tool_def k_tools[] = {
    {"noop", "空操作(不该被挂载)", "{\"type\":\"object\"}", noop_execute, noop_free},
};

/* 首字段放 9999:宿主按首 int 判版本,认不得即拒(不静默拿错结构体)。 */
static const int k_bad_tag[] = {9999, 1};

#ifdef _WIN32
__declspec(dllexport)
#endif
const void* luban_plugin_entry(void) {
    /* 借 int 数组当假 manifest:首字段就是 abi_tag,宿主读到这里就该停。 */
    return (const void*)k_bad_tag;
}
