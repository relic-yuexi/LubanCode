# 第三方组件声明(THIRD_PARTY_NOTICES)

LubanCode 发行包内携带的第三方组件在此逐件声明:组件名、版本、上游地址、
所选许可证与打包位置。随包发行时本文件与各组件许可证原文(`licenses/`)
一同进 archive;缺一件,Release 流水线必须红(设计单 P0-6 验收口径)。

## ripgrep

- 组件名:ripgrep(`rg` 命令行搜索工具)
- 版本:15.2.0(钉死于 `third_party/ripgrep/manifest.json`,不追 latest)
- 上游:https://github.com/BurntSushi/ripgrep
- 上游许可证:MIT OR UNLICENSE(双许可,上游仓库 `COPYING`/`LICENSE-MIT`/
  `UNLICENSE`)。**LubanCode 按 MIT 路分发**,不混写"MIT/Unlicense 随便算";
  MIT 原文原样随包:`third_party/ripgrep/LICENSE-MIT`(仓库内)→
  `licenses/ripgrep-MIT.txt`(发行包内)。
- 打包位置:发行包 `libexec/rg`(Windows 为 `libexec/rg.exe`)。不放在包根,
  不进用户 PATH——它只由 LubanCode 的 `search` 工具经 exe 相对路径调用,
  不是给全系统装的 `rg` 命令。
- 三平台资产(archive、SHA-256、archive 内成员路径)见
  `third_party/ripgrep/manifest.json`;哈希由 `scripts/fetch_ripgrep.py`
  下载后重算核对,Release 流水线再独立重算一遍。
- 取包取舍:Linux 取上游 amd64 `.deb` 内的 `usr/bin/rg`——实测 static-pie
  静态链接(musl 系,`ldd` 零动态依赖),非 glibc 动态构建。曾因上游
  `x86_64-unknown-linux-musl` 预编译包有"超大目录高并发偶发 SIGSEGV"的
  公开报告(BurntSushi/ripgrep#3494)想避 musl,但 `.deb` 内本就是 musl
  静态件;2026-09-01 裁决接受:本仓大树压力门 100 轮已过,且本仓工况
  (100 条截断+命中满额主动收树)不碰该 issue 场景;上游结论留 watch。

## 构建期依赖(不随发行包分发)

以下依赖只在开发/CI 构建 LubanCode 本体时经 CMake FetchContent 获取,
不进发行包、不装到用户机器上,故不在包内 notices 逐件列出;各自许可证
随 FetchContent 源码在构建目录内:

- nlohmann/json(MIT)
- yaml-cpp(MIT)
- libcurl / cpr(curl 许可证)
- Lua(Lua 许可证,MIT 型条款)
- doctest(MIT,仅测试)
