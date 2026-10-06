# Command Jobs 探针产物选择

远端 a9d3f16e 的 Windows 探针已配置、编译成功。原始 CMake File API 同时列出 Release/lubancore_command_limits_probe.exe 与同目录 PDB。旧门把产物总数当作可执行文件数，误拒了这份图。

本笔只改 CI 产物选择。生产源码、公头、预算、安装消费与命令探针都不动。Windows 必须恰有一份指定 EXE，可附同目录、同名一份 PDB；POSIX 必须恰有指定无后缀可执行文件。重复产物、缺执行文件、别名或别处 PDB、其它产物仍拒绝。

选择来自真实 target.artifacts。记录生产平台、执行文件与附属产物；上传后复核仍读原始图，用同一条规则核选择。原始 index、codemodel、全部 target/directory 字节不动。源文件、目标类型、零依赖、路径边界、实际文件、指纹与执行权限照旧核。

本地只跑纯数据反例和文档检查。真实配置、编译、执行交远端 CI。Windows 首次失败的 29 件原件保持封存，SHA-256 为 57210de4e007f73ba085572b4adf842cafe4532cb5d6fad54778431dee5ed265。
