// 渠道附件接纳服务(QQ 接入单 Q4 §十 10.2 第一项:宿主附件接纳/解析)。
//
// 分工(§十 10.2):适配器只取平台附件引用(remote_ref)并执行受控传输
//(下载 seam 由装配层递渠道实现);本服务核 MIME 白名单、净化文件名、
// 算 hash、原子发布原件到受控仓、记耐久账、产模型可见的有界投影。
// 下载发生在准入通过之后(ProcessWorkItem 执行前)——陌生用户进不了
// 泵,耗不了下载/解析资源。
//
// 受控仓(§十 10.2 "会话可访问的受控 artifact 目录";不另造存储——
// 原语复用 AtomicWriteFile/JournalWriter,账行 JSONL 与 V3 结果仓同款
// 追加式 PowerLoss):
//   <媒体根>/inbound/<artifactId>.bin   原件(不可变;artifactId=att-<hash16>)
//   <媒体根>/media.jsonl                账(ready/failed 逐行)
// 媒体根缺省在 workspace 身份根下(<identity_root>/channel-media)——
// 不在渠道状态根(Q0 的工具护单盖整棵渠道状态树,模型 read_file 读不
// 到;放 workspace 侧,受控读取口就是既有 read_file 工具的路径纪律,
// 绝对路径直读,offset/limit 有界)。
//
// 模型投影纪律:文件名/大小/MIME/sha256 前 16/存档路径 + 文本类前
// 2 KiB 有界预览(UTF-8 边界截断,含 NUL 视为二进制不预览);正文不
// 整塞——大文件模型自取 read_file。失败附件给一行稳定说明,不假装读过。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::runtime {

// 附件下载 seam(装配层包渠道实现;测试注入假账)。错误串是稳定码 +
// 脱敏人话(渠道实现保证不带 url query 里的 token)。
struct ChannelMediaBytes {
    std::string bytes;
};
using ChannelMediaDownloadFn =
    std::function<std::expected<ChannelMediaBytes, std::string>(const std::string& url)>;

struct ChannelMediaLimits {
    std::size_t max_attachments_per_message = 4;  // 超出如实拒(too_many)
    std::size_t max_preview_bytes = 2048;         // 文本类模型预览帽
};

// 入站 MIME 白名单(宿主接纳策略,§十 10.2 "宿主核 …… MIME"):平台
// 枚举含非 MIME 值(QQ 的 "voice"/"file");白名单外如实拒
// (mime_not_allowed),不虚报已接。前缀放行 text/ 与 image/,其余精确。
bool IsAllowedInboundMimeType(std::string_view mime_type);

// 文本类判定(可给模型预览的类型;二进制不预览,模型自取 read_file)。
bool IsTextLikeMimeType(std::string_view mime_type);

// 附件名净化(纯函数,§十 10.2"恶意文件名清洗净化"):取 basename(防
// 路径穿越)、剥控制字符与平台非法字符(<>:"/\|?*)、剥尾点/空格、
// Windows 保留名(CON/NUL/COM1.. 等)加前缀、UTF-8 边界限长 80 字节、
// 空名回落 "attachment"。净化不改变原件内容,只改存档/展示名。
std::string SanitizeChannelAttachmentName(std::string_view raw);

class ChannelMediaService {
public:
    // 一枚附件的接纳结果(prompt_line 是模型可见投影行)。
    struct AttachmentReceipt {
        bool ready = false;
        std::string original_name;   // 净化后的展示名
        std::string stored_path;     // 原件绝对路径 UTF-8(ready 时)
        std::string artifact_id;     // att-<sha256 前 16>
        std::string mime_type;
        std::int64_t size_bytes = 0;
        std::string sha256;
        std::string error_code;   // mime_not_allowed | too_many_attachments |
                                  // download_failed:<稳定码> | store_failed
        std::string prompt_line;
    };

    // 开仓:建媒体根、重放账(known 幂等缓存)。失败返回 false(调用方
    // 不装配——附件功能如实不可用,不拦渠道正文路)。
    static bool Open(ChannelMediaService* out, const std::filesystem::path& root);

    // 接纳一件渠道来信的全部附件(Image/Audio/Video/File part 且带
    // remote_ref)。逐枚独立成败:失败进 receipt 与账,不拦其他附件与
    // 正文轮(附件不可用如实报,不假装读过文件)。
    std::vector<AttachmentReceipt> Ingest(const channel::ChannelInboundEvent& event,
                                          std::int64_t ingress_sid,
                                          const ChannelMediaDownloadFn& download,
                                          const ChannelMediaLimits& limits,
                                          std::int64_t now_ms);

    // 只读观测(测试/诊断):账行重放。
    std::vector<nlohmann::json> ledger_lines() const { return ledger_lines_; }

private:
    void AppendLedgerLine(const nlohmann::json& line);

    std::filesystem::path root_;
    std::optional<trajectory::JournalWriter> writer_;
    // 进程内幂等缓存:url -> ready 原件(重启从账重放)。同 url 不重复
    // 下载;原件丢失(人工删)则重下。
    struct KnownAttachment {
        std::string artifact_id;
        std::string sha256;
        std::int64_t size_bytes = 0;
        bool ready = false;
    };
    std::map<std::string, KnownAttachment> known_by_url_;
    std::vector<nlohmann::json> ledger_lines_;
};

}  // namespace lubancode::runtime
