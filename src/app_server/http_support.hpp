// app_server 两套本地 HTTP 承载的公共机制件(架构审查单 HC-03):独立 WS
// 承载(ws_transport)与助理 Web 承载(local_web_server)共用一份的有界
// 读头、恒时字节比较、只读 artifact 加载。两处原本是同尺复制件——改头部
// 边界、产物大小或读失败规则只改这里,两边同步生效。
//
// 只合机制,不合策略:首帧 token 门/CORS/不可变缓存(WS 宿主)与
// Cookie 会话/同源门/CSP/no-store(助理宿主)仍归各自宿主;应答头与
// 错误话面由调用方拼,本层不识字节以外的任何事。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "app_server/ws_sockets.hpp"

namespace lubancode::app_server {

// 头部读入上限:本地承载的 HTTP 头就几行,超了就是来捣乱的,断。
inline constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

// 单枚 artifact 的读入上限:截图/镜像帧是图,内容寻址落盘时本就有尺寸
// 约束;这里再兜一层,超了按"没有这枚"回,不当流媒体伺候。
inline constexpr std::uintmax_t kMaxArtifactBytes = 64ull * 1024 * 1024;

// 一口一口读到 \r\n\r\n 或断/超上限。返回 false = 断/坏。读到的一切都进
// header——含终止符之后同一段 TCP 挤进来的先头字节(POST body 前缀),
// 劈开是调用方的事。
bool ReadUntilHeaderEnd(net::Socket& socket, std::string& header);

// 恒时比较:逐字节累积差,不短路——不给计时侧信道留口。长度不同直接
// false(长度本身不是秘密)。
bool ConstantTimeEqual(std::string_view given, std::string_view expected);

// 只读 artifact 的加载结果。ok=false 统一按"没有这枚"回(名字形状不对/
// 没配目录/文件不在/超限/读失败),调用方拿自己的话面与应答头拼应答。
struct ArtifactBytes {
    bool ok = false;
    std::string bytes;       // ok 时的文件字节
    const char* mime = "";   // ok 时:"image/png" 或 "image/jpeg"
};

// 名字形状(ws::IsValidArtifactName:art-<hex 8..64>.(png|jpeg|jpg))→
// file_size 上限 → 整读 → MIME 三选一。max_bytes 默认 kMaxArtifactBytes,
// 测试可收窄走同一条超限路。
ArtifactBytes LoadArtifactBytes(const std::string& artifact_dir, const std::string& name,
                                std::uintmax_t max_bytes = kMaxArtifactBytes);

}  // namespace lubancode::app_server
