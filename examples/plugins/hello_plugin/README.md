# hello_plugin — native 插件示例(ABI v2,三平台)

一枚 `reverse_text` 工具:入参 `{"text": "..."}`,按 UTF-8 字符倒序返回
(中文按字不按字节)。声明 `CAP_HOST_ALLOCATOR`,结果 buffer 从宿主堆拿
(`host_callbacks.allocate`)、`free_result` 里 `release` 交还——两边就
算链不同的 CRT 也不跨堆。

## ABI v2 要点(include/luban_plugin.h)

- manifest 首字段是 `abi_tag`:写 `LUBAN_PLUGIN_ABI_V2`(2);v1 老插件
  写 1,宿主兼容读取,加载行明报 legacy。
- `struct_size` 写 `sizeof(luban_plugin_manifest_v2)`:宿主按它前向兼容
  (新宿主认得更多字段也不读越界,新插件旧宿主只用到旧字段)。
- `api_min`/`api_max` 与宿主的 `[2, 2]` 须有交集。
- `plugin_id` 定工具名前缀(这里 `hello_plugin`),`plugin_version` 进
  `/plugin inspect` 台账。
- `shutdown` 可空;写了就在卸载前被调一次,收自己的线程与资源。
- v2 的 manifest 须可写(宿主加载后灌 `host_callbacks`),别加 const。

## 构建

```bash
cmake -S . -B build
cmake --build build --config Release
# Windows: build/Release/hello_plugin.dll
# Linux:   build/libhello_plugin.so
# macOS:   build/libhello_plugin.dylib
```

CI 里同一份 fixture 三平台真编真载真调真卸(tests/CMakeLists 的
hello_plugin_fixture,plugins 单第 5 步验收)。

## 多平台包布局

一只包带多平台产物时按 OS + arch 分目录,manifest 指对当前 target:

```text
native-tools/
  plugin.json                 # runtime.entry 按 target 指产物
  bin/windows-x64/hello.dll
  bin/linux-x64/libhello.so
  bin/linux-arm64/libhello.so
  bin/macos-x64/libhello.dylib
  bin/macos-arm64/libhello.dylib
```

宿主按 OS + arch 精确挑一份;找不到当前 target 就 unavailable,不拿
相近文件试载。架构不合(`x64` DLL 撞 `arm64` 宿主)在加载错误里明说。

## 安装(单平台直放)

```text
Windows: %USERPROFILE%\.lubancode\plugins\hello_plugin.dll
Linux:   ~/.lubancode/plugins/libhello_plugin.so
macOS:   ~/.lubancode/plugins/libhello_plugin.dylib
```

主库与依赖库同目录放;宿主略过没有 `luban_plugin_entry` 的依赖库。
重启 LubanCode,`/plugins` 应见 `hello_plugin: 1 个工具`,工具名
`plugin__hello_plugin__reverse_text`。

## 风险声明

native 插件加载即执行库 constructor(Windows 的 DllMain 同理),崩了带倒
宿主,也能取得宿主进程权限。只加载信得过、版本对得上的二进制;分发时
标明 ABI 版本、架构与 CRT,并让用户走 hash 信任账。
