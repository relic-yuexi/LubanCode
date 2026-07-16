# lubancode

一个用 C++ 写的 AI 编程 CLI。眼下还是骨架阶段(M0):能编译、能跑、`--version` 有输出。

## 依赖

- CMake ≥ 3.21
- 一个支持 C++23 的编译器(见下方"这台机器上的实际情况")
- [cpr](https://github.com/libcpr/cpr)(HTTP 客户端)、[nlohmann-json](https://github.com/nlohmann/json)——两个都会自动拉取,不用手装

## 构建

本机(Windows,Git Bash)已验证过的构建方式,用 CMakePresets:

```bash
# Debug
cmake --preset debug
cmake --build --preset debug
./build/debug/Debug/lubancode.exe --version

# Release
cmake --preset release
cmake --build --preset release
./build/release/Release/lubancode.exe --version
```

预期输出:

```
lubancode 0.1.0
```

不传参数直接跑,会顺带跑一遍 `cpr`、`nlohmann-json` 的最小示例,证明两个依赖都链接、能用:

```
lubancode 0.1.0
[demo] nlohmann::json -> {"deps":["cpr","nlohmann-json"],"name":"lubancode","version":"0.1.0"}
[demo] cpr 链接成功, CPR_VERSION = 1.11.1
```

## 这台机器上的实际情况

写这份骨架时探测过的工具链,如实记一笔:

- **CMake** 4.2.3,**Ninja** 1.13.2 都有。
- **MSVC**:没装在 PATH 里能直接调用的 `cl.exe`,但 Visual Studio 2022 BuildTools(17.14,VC 工具集 14.44)装在机器上,CMake 用 `Visual Studio 17 2022` 生成器能自动找到、能编译、能跑通 `std::expected`。所以 `CMakePresets.json` 里用的就是这个生成器。
- **g++**(MSYS2 mingw64,15.2.0)也在 PATH 里,同样支持 C++23 与 `std::expected`,留作备选,没有用在默认 preset 里。
- **vcpkg**:这台机器上没装(`where vcpkg` 找不到,`VCPKG_ROOT` 没设,`C:\vcpkg` 不存在)。`vcpkg.json` 照样写好留给 CI/装了 vcpkg 的机器用;`CMakeLists.txt` 里会先探测 `VCPKG_ROOT` / vcpkg 工具链,探测到就走 `find_package`,探测不到就自动回退到 `FetchContent` 直接拉 cpr、nlohmann-json 的源码来构建——本机走的就是这条路。

C++ 标准用的是 **C++23**(MSVC 19.44 与 mingw g++ 15.2 都完整支持 `std::expected`,不用降级到 C++20)。

## 依赖拉取的坑(如实记录)

本机网络环境下,`github.com` 主站(不管是 `git clone` 还是 `github.com/.../releases/download/...` 这类直链)连接经常被重置、连不上;但 `codeload.github.com` 的归档包下载(`https://codeload.github.com/<owner>/<repo>/tar.gz/refs/tags/<tag>`)是通的。所以 `CMakeLists.txt` 里所有 `FetchContent_Declare` 都改成了走 `codeload.github.com` 的 tarball URL,没有用 `GIT_REPOSITORY`。

cpr 默认会自己再拉一份 `curl`(默认地址在 `github.com/curl/curl/releases/download/...`,同样连不上)和 `zlib-ng`(`git clone` 到 `github.com`,也连不上)。这里的处理是:

- `curl` 源码提前用 `codeload.github.com` 下载解压好,通过 `FETCHCONTENT_SOURCE_DIR_CURL` 让 cpr 直接复用,不用它自己再下载一次。
- `zlib-ng` 直接关掉(`CURL_ZLIB OFF`),M0 骨架用不上压缩支持。

另外,这台机器上 MSYS2/mingw64 的 `include` 目录里装着 `nghttp2`、`libidn2`、`libpsl`、`libssh2` 的开发头文件。用 MSVC 编译 curl 时,CMake 的 `find_package` 会把这些"顺手"找出来当系统库用——但那是给 MinGW/GCC 用的头文件,MSVC 编译器读不懂,会直接报语法错误(`C2061`/`C2059` 之类)。处理办法是显式关掉这几个可选特性,并用 `CMAKE_DISABLE_FIND_PACKAGE_<Pkg>` 禁止 `find_package` 再去找它们(见 `CMakeLists.txt` 里的注释)。

依赖默认编译成静态库(`BUILD_SHARED_LIBS OFF`),这样 `lubancode.exe` 自己就能跑,不用把 `cpr.dll`、`libcurl-*.dll` 之类的运行时依赖到处放、还得操心 PATH。

## 目录结构

```
src/
├── main.cpp   # 入口:解析 --version / --help
├── cli/       # 命令行交互层
├── agent/     # agent 核心循环
├── api/       # 与大模型对话的通路(Anthropic / Responses 两个后端)
├── tools/     # 模型可调用的工具(read_file、run_command……)
└── config/    # 配置
docs/
└── architecture.md   # 架构说明,详见此文档
```

架构细节(分层依赖、api 双后端设计、工具层、错误处理、里程碑规划)见 [`docs/architecture.md`](docs/architecture.md)。
