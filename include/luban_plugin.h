/* luban_plugin.h - lubancode 原生插件 C ABI 头(跨平台,plugins 单第 5/6 步)。
 *
 * 一份纯 C ABI,三个平台各自编产物:Windows .dll / Linux .so / macOS
 * .dylib。放在 <用户主目录>/.lubancode/plugins/ 下,宿主启动时统一走
 * OpenModule/FindSymbol/CloseModule 平台抽象加载(见
 * src/platform/dynamic_library.hpp)。
 *
 * ABI 版本账:
 *   - ABI v1(legacy):manifest 首字段 int api_version,值恒 1。老插件
 *     只有 name/description/schema/execute/free_result 五样。
 *   - ABI v2(当前):manifest 首字段 int abi_tag = LUBAN_PLUGIN_ABI_V2,
 *     带 struct_size / api_min / api_max / 插件 id 与 version / shutdown /
 *     capability flags / host 回调表。宿主按 struct_size 前向兼容读:
 *     插件写的 struct_size 比宿主认识的小,只读宿主认得的字段;比宿主
 *     新,只用到宿主那层的字段——两头都不静默拿错结构体。
 *   - 判别:manifest 首字段的值。1 = v1(legacy,宿主兼容读取,加载行
 *     明报 "legacy ABI v1");2 = v2。其余值拒绝加载,警告里点名不猜。
 *
 * 跨堆规矩(两版同一条):execute 返回的 content 由插件自己分配,宿主
 * 拷贝完立刻回调 free_result 交还插件释放——两边可能链不同的 CRT,谁分配
 * 谁释放,绝不跨堆 free。v2 另立 buffer 契约:插件也可以用 host_callbacks
 * 的 allocate/release 在宿主堆上拿/还内存(能力位 capability_host_allocator
 * 声明过才用),两不混。
 *
 * 风险声明:插件跟宿主同进程,库里崩了(野指针、栈破坏、ABI 错配)宿主
 * 兜不住,整个进程一起完蛋。装谁的插件,风险自担;native 插件必须单独
 * 批准并记文件 hash(单子「核心定案」C 节)。
 */
#ifndef LUBAN_PLUGIN_H
#define LUBAN_PLUGIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI 版本                                                            */
/* ------------------------------------------------------------------ */

#define LUBAN_PLUGIN_ABI_V1 1  /* legacy:api_version 字段,值 1 */
#define LUBAN_PLUGIN_ABI_V2 2  /* 当前:struct_size + 版本域 + 能力位 */

#define LUBAN_PLUGIN_V2_API_MIN 2  /* 宿主/插件都须认的下界 */
#define LUBAN_PLUGIN_V2_API_MAX 2  /* 宿主认的上界(升 ABI 时抬) */

/* ------------------------------------------------------------------ */
/* 工具执行结果(两版共用)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* content;  /* UTF-8、\0 结尾;插件自己的堆 */
    int is_error;         /* 非 0 = 执行失败(content 是人话错误说明) */
} luban_tool_result;

/* 一个工具的完整定义。字符串都是 UTF-8、\0 结尾,生命周期由插件保证覆盖
 * 整个会话(静态常量即可)。 */
typedef struct {
    const char* name;              /* 工具短名(宿主拼 plugin__<插件id>__) */
    const char* description;       /* 给模型看的说明 */
    const char* input_schema_json; /* 入参 JSON Schema,序列化好的 JSON 文本 */

    /* 执行。input_json 是模型入参序列化好的 JSON 对象文本(UTF-8)。 */
    luban_tool_result (*execute)(const char* input_json);

    /* 释放 execute 返回的结果。跨堆释放必须由插件自己做;宿主拷完
     * content 立刻调用,之后不再碰 result 里的指针。 */
    void (*free_result)(luban_tool_result*);
} luban_tool_def;

/* ------------------------------------------------------------------ */
/* ABI v1 manifest(legacy,兼容读取)                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int api_version;  /* 恒 1(LUBAN_PLUGIN_ABI_V1) */
    int tool_count;
    const luban_tool_def* tools;
} luban_plugin_manifest_v1;

/* ------------------------------------------------------------------ */
/* ABI v2:宿主给插件的回调表(buffer 契约的另一半)                     */
/* ------------------------------------------------------------------ */

/* 宿主堆上的分配/释放(v2 capability_host_allocator 声明过才用):
 * allocate 拿 N 字节(release 交还),失败返回 NULL。插件用这对回调拿到
 * 的 buffer 可以直接放进 luban_tool_result.content——但 free_result 仍是
 * 必须的:插件在里面回调 host_callbacks.release 把 buffer 还给宿主堆,
 * 再清自己的账。契约只有一条:content 是谁分配的,free_result 就把谁
 * 的分配还给谁(自己的 malloc 自己 free;host allocate 的 release 回去)。
 * 宿主照旧拷完 content 就调 free_result,不必分辨 buffer 来路。 */
typedef struct {
    void* (*allocate)(size_t bytes);
    void (*release)(void* pointer);
} luban_plugin_host_callbacks;

/* ------------------------------------------------------------------ */
/* 能力位(v2 manifest 的 capability_flags;按位或,未知的位宿主忽略)   */
/* ------------------------------------------------------------------ */

#define LUBAN_PLUGIN_CAP_NONE             0u
#define LUBAN_PLUGIN_CAP_HOST_ALLOCATOR   (1u << 0)  /* 会用 host_callbacks 分配结果 buffer */
#define LUBAN_PLUGIN_CAP_THREAD_SAFE      (1u << 1)  /* execute 可被并发调(否则宿主串行) */
#define LUBAN_PLUGIN_CAP_STREAMING_LATER  (1u << 2)  /* 预留:流式结果(未实现,占位) */

/* ------------------------------------------------------------------ */
/* ABI v2 manifest(当前)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    /* 头四个字段是"ABI 门面",位置与类型永不变——宿主只看这里判版本。 */
    int abi_tag;      /* LUBAN_PLUGIN_ABI_V2 */
    int struct_size;  /* sizeof(luban_plugin_manifest_v2) 写死;宿主按它前向兼容 */
    int api_min;      /* 插件认的宿主 ABI 下界(含) */
    int api_max;      /* 插件认的宿主 ABI 上界(含) */

    /* 插件身份:诊断/台账用,不进模型 prompt。 */
    const char* plugin_id;      /* 如 "native-tools";空串 = 宿主拿文件名当 id */
    const char* plugin_version; /* 语义展示用 */

    /* 工具清单。 */
    int tool_count;
    const luban_tool_def* tools;

    /* 能力位(LUBAN_PLUGIN_CAP_*)。 */
    unsigned int capability_flags;

    /* 宿主回调表(host allocator)。宿主加载后、首次 execute 前灌一次;
     * 插件没声明 CAP_HOST_ALLOCATOR 就不碰。 */
    luban_plugin_host_callbacks host_callbacks;

    /* 收尾钩子:宿主卸载模块前调一次(可 NULL)。插件在这关自己起的线程、
     * 放静态资源;不许再调任何 execute。 */
    void (*shutdown)(void);
} luban_plugin_manifest_v2;

/* ------------------------------------------------------------------ */
/* 入口(两版同一枚导出符号)                                           */
/* ------------------------------------------------------------------ */

/* 宿主 FindSymbol 找的就是这个名字。返回的 manifest 首字段判版本:
 *   值 1 = luban_plugin_manifest_v1(legacy)
 *   值 2 = luban_plugin_manifest_v2
 * 插件要跨 ABI 发一份产物,就在这枚函数里按编译期开关返回对应结构。
 * 返回 NULL = 插件自己拒绝上工(宿主打警告跳过)。
 *
 * Windows 导出:
 *     extern "C" __declspec(dllexport)
 *     const void* luban_plugin_entry(void);
 * POSIX 导出(默认可见即可,或显式 __attribute__((visibility("default")))):
 *     extern "C" const void* luban_plugin_entry(void);
 */
#ifdef __cplusplus
}  /* extern "C" */

/* C++ 侧便捷:入口的返回类型按 ABI 分派写 void*,C 里手写同款也行。 */
extern "C" const void* luban_plugin_entry(void);
#endif

#endif /* LUBAN_PLUGIN_H */
