/* luban_plugin.h - lubancode C ABI 插件对外头文件(M7)。
 *
 * 插件是一个 DLL,放在 <用户主目录>/.lubancode/plugins/ 下,唯一导出一个
 * 入口函数:
 *
 *     extern "C" __declspec(dllexport)
 *     const luban_plugin_manifest* luban_plugin_entry(void);
 *
 * 宿主启动时 LoadLibraryW + GetProcAddress 拿到 manifest,校验 api_version,
 * 把每个 luban_tool_def 裹成内置工具挂进工具表(工具名前缀
 * plugin__<dll名>__,执行前一律要用户确认)。
 *
 * 跨堆规矩:execute 返回的 content 由插件自己分配,宿主拷贝完立刻回调
 * free_result 交还插件释放——两边可能链接不同的 CRT,谁分配谁释放,绝不
 * 跨堆 free。
 *
 * 风险声明:插件跟宿主同进程,DLL 里崩了(野指针、除零……)宿主兜不住,
 * 整个进程一起完蛋。装谁的插件,风险自担。
 */
#ifndef LUBAN_PLUGIN_H
#define LUBAN_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#define LUBAN_PLUGIN_API_VERSION 1

/* 一次工具执行的结果。content 指向插件自己分配的、UTF-8 编码、以 \0 结尾
 * 的文本;is_error 非 0 表示执行失败(content 里放人能看懂的错误说明)。 */
typedef struct {
    const char* content;
    int is_error;
} luban_tool_result;

/* 一个工具的完整定义。所有字符串都是 UTF-8、以 \0 结尾,生命周期由插件
 * 保证覆盖整个会话(通常做成静态常量即可)。 */
typedef struct {
    const char* name;              /* 工具名(不含前缀,宿主自己拼 plugin__<dll名>__) */
    const char* description;       /* 给模型看的一段说明 */
    const char* input_schema_json; /* 入参 JSON Schema,序列化好的 JSON 文本 */

    /* 执行。input_json 是模型给的入参,序列化好的 JSON 对象文本(UTF-8)。 */
    luban_tool_result (*execute)(const char* input_json);

    /* 释放 execute 返回的结果。跨堆释放,必须由插件自己释放——宿主拷贝完
     * content 后立刻调用,之后不再碰 result 里的指针。 */
    void (*free_result)(luban_tool_result*);
} luban_tool_def;

/* 插件清单:luban_plugin_entry 返回的东西。api_version 必须填
 * LUBAN_PLUGIN_API_VERSION,对不上宿主会打警告并跳过整个插件。 */
typedef struct {
    int api_version;
    int tool_count;
    const luban_tool_def* tools;
} luban_plugin_manifest;

/* 插件唯一导出:
 *     extern "C" __declspec(dllexport)
 *     const luban_plugin_manifest* luban_plugin_entry(void);
 */

#ifdef __cplusplus
}
#endif

#endif /* LUBAN_PLUGIN_H */
