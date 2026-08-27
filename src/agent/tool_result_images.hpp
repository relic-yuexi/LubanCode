// 工具结果图片回喂单:请求出门前把工具结果里的图片字节从 artifact 落盘
// 重灌成 base64,给四家 wire 上原生图块用。
//
// 为什么要"重灌"而不是常驻:MCP 富结果单立下的规矩是 durable history 里
// 只有 ArtifactRef 引用,base64 不许长期挂在会话对象上(会话存档会跟着
// 膨胀,resume 也会重放半辈子前的字节)。但 wire 上要真图,所以每一轮
// 请求在副本上现灌:文件在、验身过、帽内,才把 base64 填进 ImageContent
// ::wire_base64(恳求态字段,session_store 不落);发完即弃,下一轮再灌。
// 与用户贴图(api::ImageBlock)的待遇一致——那路 base64 也是每轮随请求
// 重发的。
//
// 护栏(与 MCP 侧 rich_result 的口径同源,帽子更紧):
//   - 魔数复核:agent::SniffImageFormat 认出的 MIME 必须与块声明一致,
//     文件被换过内容就拒灌(伪 MIME 不出门);
//   - 单张字节帽 5 MiB(解码后):anthropic 上限 10MB base64(≈7.5MiB
//     解码),responses/gemini 也都在 20MB 档,5 MiB 是四家通吃的稳妥帽;
//   - 边长帽 8000px:anthropic 单图上限 8000x8000;
//   - 每请求合计帽 20 MiB(base64 后):多图叠着不放爆请求体。
// 任一不过就跳过那张图(wire 走文本投影降级,投影短句里已有路径),
// 不报错、不截字节——重灌是尽力而为的增项,不能把整轮请求拖死。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "api/types.hpp"

namespace lubancode::agent {

// 单张工具图上 wire 的解码字节帽。
inline constexpr std::size_t kMaxToolResultWireImageBytes = 5 * 1024 * 1024;
// 单张工具图上 wire 的边长帽(px,宽高各算)。
inline constexpr std::uint32_t kMaxToolResultWireImageSide = 8000;
// 单次请求全部工具图 base64 合计帽。
inline constexpr std::size_t kMaxToolResultWireImageTotalBytes = 20 * 1024 * 1024;

// 遍历请求里的 ToolResultBlock,把 ImageContent 块从 artifact 落盘重灌
// base64 到 wire_base64。artifact_root 是会话 artifact 目录(与
// ToolExecutionContext::artifact_dir 同一目录:LandToolArtifact 落的
// <会话>/mcp-artifacts)。已灌过的(幂等重入)、文件不在、验身不过、
// 超帽的一律跳过。返回本轮灌进去的张数(日志/诊断用)。
std::size_t RehydrateToolResultImages(api::Request& request, const std::string& artifact_root);

}  // namespace lubancode::agent
