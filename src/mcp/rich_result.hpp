// MCP tools/call 富结果解析(MCP 富结果单 P0.4/P0.5):把 server 返回的
// content[](text/image/audio/resource_link/resource)与 structuredContent
// 逐块翻成 tools 层的中立 ToolResultPayload。解析与落盘集中在这一个纯
// 模块,Client 只管调用——单测不起进程也能钉死解析、乱序、坏 base64、
// 伪 MIME、大小帽与 artifact 落盘。
//
// 安全帽(P0.5):
//   - base64 严格解码(复用 agent::DecodeBase64Strict 的字母表/padding
//     规矩),坏字母、坏 padding、声明与实字节能对上才收;
//   - 图片 MIME 先过 allowlist,再按魔数复核——image/png 装一份 EXE 字节
//     须拒绝(mcp.mime_mismatch);
//   - 二进制先落会话 artifact 目录(内容寻址,文件名本地起,不信
//     server),块里只留 ArtifactRef;没有落盘地(单测/没开会话)按稳定
//     错收口,不吞字节不冒充落盘;
//   - 单块/单次调用设字节帽,超帽整次按协议错收口(mcp.size_cap_exceeded)
//     ——不拿半真半假的内容继续跑;
//   - 坏一块 = 整次协议错(mcp.malformed_content),不静默丢块。
#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/tool_content.hpp"

namespace lubancode::mcp {

// ---- 帽(单测要能收窄,全走 ParseContext,不埋魔数) --------------------

// 单块图片上限(与 agent::kMaxModelImageBytes 同量级:20 MiB)。
inline constexpr std::size_t kMaxImageBlockBytes = 20 * 1024 * 1024;
// 单块音频上限。
inline constexpr std::size_t kMaxAudioBlockBytes = 20 * 1024 * 1024;
// 单块内嵌 blob 资源上限。
inline constexpr std::size_t kMaxBlobBlockBytes = 50 * 1024 * 1024;
// 单次 tools/call 全部二进制块解码后合计上限。
inline constexpr std::size_t kMaxCallBinaryBytes = 64 * 1024 * 1024;
// 内嵌文本资源的内联字符帽:帽内原样,超帽落 artifact 只回节选。
inline constexpr std::size_t kMaxEmbeddedTextInlineChars = 200 * 1024;
// structuredContent 的 dump 总字节帽。
inline constexpr std::size_t kMaxStructuredBytes = 1024 * 1024;
// structuredContent 的 JSON 深度帽。
inline constexpr int kMaxStructuredDepth = 32;

// 解析入参。
struct CallToolParseContext {
    std::string server_name;
    // 二进制落盘目录(会话 <session>/mcp-artifacts)。空 = 无落盘地,
    // 遇二进制块按 mcp.artifact_unavailable 收口。
    std::string artifact_dir;
    // 单次调用允许的解码字节预算(余量,由 Client 扣过分项账后递进来;
    // 纯函数单测给满额)。
    std::size_t binary_budget = kMaxCallBinaryBytes;
    // 会话累计落盘上限(Client 侧的账:超了由 Client 拦在解析之前)。
    static constexpr std::size_t kSessionArtifactCap = 512ULL * 1024 * 1024;
    // 工具声明的 outputSchema(tools/list 拿到的);有它才校验
    // structuredContent,不合给 mcp.output_schema_mismatch。
    std::optional<nlohmann::json> output_schema;
};

// 解析产物:payload 是好结果;protocol_error=true 时 payload 作废,按
// 整次协议错收口(is_error 置位、稳定码见 error_code)。非致命问题(未知
// content type)不置 protocol_error,块以 UnknownContent 占位、details 留码。
struct CallToolParseResult {
    tools::ToolResultPayload payload;
    bool server_is_error = false;
    bool protocol_error = false;
    std::string error_code;
    std::string error_message;
    nlohmann::json details = nlohmann::json::object();
    // 本次调用落盘的二进制字节合计(含超帽卸载的内嵌文本),Client 拿去
    // 记会话累计账。
    std::size_t landed_bytes = 0;
};

// 落一盘 artifact:内容寻址 art-<sha8>.<ext>,原子写、同图去重。返回
// 相对路径("<dir>/art-xxx.png")供 ArtifactRef.path;失败给空串(调用方按
// 协议错收口)。单测直接钉 SHA/路径/幂等。
std::string LandToolArtifact(const std::string& artifact_dir, const std::string& bytes,
                             const std::string& extension);

// tools/call 的 result 对象 -> 富 payload。永不抛异常(坏 JSON 形状按
// mcp.malformed_content 收口)。
CallToolParseResult ParseCallToolResult(const nlohmann::json& result, const CallToolParseContext& context);

}  // namespace lubancode::mcp
