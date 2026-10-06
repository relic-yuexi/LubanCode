# Worker 私有协议版本检查

基线 `242c14e4`。本片补现有父管道协议的版本入口，不开网络服务，
也不把 attachment 充作认证。执行仍只走公开 SDK。

每份请求可在顶层带 `protocol_version`。省略沿旧 v1；显式值须为整数。
当前只接 1。布尔、浮点、字符串、null、数组和对象报
`worker.invalid_protocol_version`；其余整数报
`worker.unsupported_protocol_version`。先核关联 ID 和版本，再读方法参数、
查 attachment 或调 SDK。未支持版本不能初始化目录、建场、受理、换连接
或关闭 Worker；错误仍回原请求 ID，不回请求正文。

`worker.status` 保留原 `protocol_version=1`，另列
`supported_protocol_versions=[1]`。父进程可先用旧格式查 status，再显式选择
双方都支持的版本。以后新版本须有独立解码与行为合同，不能只放宽数字检查。
能力仍按原表报实际范围；版本相同不暗授新能力。

原 JSON/UTF-8 帧帽、未知字段拒绝、幂等、取消、生命周期和 preview/full 双门
照旧。版本号不进初始化参数或业务幂等键，也不写成 Session 权限。
本片没有跨机认证、持久游标、HTTP/SSE 或 Managed 执行接线。

验收留在原十八场真实 Worker 进程测试的 health 场。真进程初始化前核旧格式、
显式 v1、坏类型和未来版本；拒绝后仍未创建数据根。初始化后核未来版本
不能建场、换连接或关进程，且不发模型请求。原十八场、时限和命令保留。
本地只查源码、Python AST 与文档；进程、HTTP 和编译交同源三平台 CI。
