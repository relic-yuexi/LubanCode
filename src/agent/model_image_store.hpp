// 模型输出图片的落盘口(ccmoon 真机巡检单 P0):Responses 的
// image_generation_call 把整张图以 base64 塞在流里,这一层负责把它变成
// 会话 artifact 目录里的一只真文件:
//
//   <会话目录>/images/img-<sha8>.<ext>   内容寻址,同图只落一份
//
// 硬规矩(工单"完整修复"节):
//   - base64 限额解码(解码后字节数超 kMaxModelImageBytes 直接拒),
//     解码严格校验(字母表/padding),坏串不落盘;
//   - 魔数验身:PNG/JPEG/GIF/WebP 四类认得出才收,MIME 与尺寸从字节里
//     读出来,不信服务端自报的 output_format 一个字;
//   - 原子落盘:同目录 tmp 写完 flush 再 rename,任一步失败清掉 tmp,
//     不留半截文件;文件名由本地按内容寻址起,不信模型正文;
//   - 返回的 ModelImageBlock 只是引用,base64 用完即弃,不进历史。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "api/types.hpp"

namespace lubancode::agent {

// 解码后的图片上限(字节)。SSE 单帧本有 8 MiB 上限(SseFramer::
// kMaxFrameBytes,base64 折算解码后约 6 MiB),这道是模块自己的独立闸:
// 不经 SSE 的调用路(回放测试、未来的非流式收图)也得有顶。
inline constexpr std::size_t kMaxModelImageBytes = 20 * 1024 * 1024;

// 落盘结果:block 入历史(session/export/resume 只存这份引用),display_path
// 给终端一行"已保存 <路径>"用——绝对路径,当场可打开。
struct ModelImageLanding {
    api::ModelImageBlock block;
    std::string display_path;
};

// 图片字节 → {MIME, 扩展名}。认不得的格式给空串(调用方拒收)。
// 纯函数,单测钉魔数表。
struct ImageFormat {
    std::string mime_type;
    std::string extension;  // 不带点("png")
};
ImageFormat SniffImageFormat(const std::string& bytes);

// 从图片字节里读宽高(像素)。读不出给 {0,0}(不拦落盘——尺寸是展示与
// 校验用的账,个别变体流(渐进 JPEG 的罕见排列、扩展 WebP)读不出就空着,
// 魔数已经把格式门住了)。
struct ImageDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
ImageDimensions ReadImageDimensions(const std::string& bytes, const std::string& mime_type);

// 解码+验身+落盘,一步到位。images_dir 是会话图片目录(通常
// <sessions_dir>/<session-id>/images,由宿主递进来,目录不存在会创建)。
// 失败给人话(error),不落半截文件。
std::expected<ModelImageLanding, std::string> LandModelImage(const std::string& images_dir,
                                                             const api::ImageOutput& image);

// 严格 base64 解码(标准字母表 + 必须 padding,拒绝空白/URL 安全变体/
// 超长输入)。max_bytes 是解码后上限,超了报错不硬解。纯函数,单测钉。
std::expected<std::string, std::string> DecodeBase64Strict(const std::string& text, std::size_t max_bytes);

}  // namespace lubancode::agent
