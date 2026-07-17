/* 测试夹具:api_version 填错的插件——宿主必须打警告跳过它,不挂载、不崩。 */
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

static const luban_plugin_manifest k_manifest = {
    /* api_version = */ 9999, /* 故意不合 */
    1,
    k_tools,
};

#ifdef _WIN32
__declspec(dllexport)
#endif
const luban_plugin_manifest* luban_plugin_entry(void) {
    return &k_manifest;
}
