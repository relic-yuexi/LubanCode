// 渠道附件接纳服务实现(QQ 接入单 Q4)。合同见头文件。
#include "runtime/channel_media_service.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/base64.hpp"
#include "agent/model_image_store.hpp"

namespace lubancode::runtime {

namespace {

// 净化后的名长帽(字节;UTF-8 边界截断)。
constexpr std::size_t kMaxAttachmentNameBytes = 80;

bool IsWindowsReservedName(std::string_view stem_lower) {
    static const std::set<std::string_view> kReserved = {
        "con",  "prn",  "aux",  "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    return kReserved.count(stem_lower) > 0;
}

std::string_view StemOf(std::string_view name) {
    const std::size_t dot = name.find('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

// UTF-8 截断:切断点回退到不含 10xxxxxx 起始字节的位置。
std::string TruncateUtf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    if (end == 0) {
        end = max_bytes;  // 防御:切点落在超长序列头部(理论不可达)
    }
    return std::string(text.substr(0, end));
}

std::string ToLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

// url 去 query(§十 10.2"签名 URL 不进普通日志"):token/签名全在 query,
// 整段剥掉;无 query 原样。host+path 做幂等键,平台换签不改键。
std::string StripUrlQuery(std::string_view url) {
    const std::size_t cut = url.find_first_of("?#");
    return std::string(cut == std::string_view::npos ? url : url.substr(0, cut));
}

bool ContainsNulInPrefix(const std::string& bytes, std::size_t limit) {
    const std::size_t end = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < end; ++i) {
        if (bytes[i] == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

bool IsAllowedInboundMimeType(std::string_view mime_type) {
    if (mime_type.empty()) {
        return false;
    }
    if (mime_type.rfind("text/", 0) == 0 || mime_type.rfind("image/", 0) == 0) {
        return true;
    }
    // 平台枚举的非 MIME 值("voice"/"file")与常见文档/数据档;白名单外
    // 如实拒,不虚报。
    static const std::set<std::string_view> kExact = {
        "voice", "file",
        "application/json", "application/pdf", "application/zip",
        "application/xml",  "application/octet-stream",
        "application/msword", "application/vnd.ms-excel", "application/vnd.ms-powerpoint",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "audio/amr",        "audio/silk", "video/mp4",
    };
    return kExact.count(mime_type) > 0;
}

bool IsTextLikeMimeType(std::string_view mime_type) {
    if (mime_type.rfind("text/", 0) == 0) {
        return true;
    }
    static const std::set<std::string_view> kTextLike = {
        "application/json", "application/xml", "application/javascript",
        "application/yaml", "application/x-yaml", "application/toml",
        "application/csv",
    };
    return kTextLike.count(mime_type) > 0;
}

std::string SanitizeChannelAttachmentName(std::string_view raw) {
    // 1) basename:按两种分隔符取最后一段(防 ../ 与绝对路径注入)。
    const std::size_t slash = raw.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? raw : raw.substr(slash + 1));
    // 2) 剥控制字符与平台非法字符。
    std::string cleaned;
    cleaned.reserve(name.size());
    for (const char ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '|' || ch == '?' || ch == '*') {
            continue;
        }
        cleaned.push_back(ch);
    }
    // 3) 剥尾部点与空格(Windows 目录项约束)。
    while (!cleaned.empty() &&
           (cleaned.back() == '.' || cleaned.back() == ' ')) {
        cleaned.pop_back();
    }
    // 4) 保留名改写(带扩展名也一并判:CON.txt 同样非法)。
    const std::string stem_lower = ToLowerAscii(StemOf(cleaned));
    if (!stem_lower.empty() && IsWindowsReservedName(stem_lower)) {
        cleaned.insert(cleaned.begin(), '_');
    }
    // 5) UTF-8 边界限长。
    cleaned = TruncateUtf8(cleaned, kMaxAttachmentNameBytes);
    while (!cleaned.empty() &&
           (cleaned.back() == '.' || cleaned.back() == ' ')) {
        cleaned.pop_back();  // 截断可能又切出尾点
    }
    // 6) 空名回落。
    if (cleaned.empty()) {
        cleaned = "attachment";
    }
    return cleaned;
}

bool ChannelMediaService::Open(ChannelMediaService* out, const std::filesystem::path& root) {
    if (out == nullptr) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "inbound", ec);
    if (ec) {
        return false;
    }
    auto writer = trajectory::JournalWriter::Open(
        root / "media.jsonl", trajectory::JournalWriter::OpenMode::Append);
    if (!writer.has_value()) {
        return false;
    }
    out->root_ = root;
    out->writer_ = std::move(*writer);
    out->known_by_url_.clear();
    out->ledger_lines_.clear();
    // 账重放:ready 行恢复幂等缓存(原件在才算数;被人工删的重下)。
    std::ifstream stream(root / "media.jsonl", std::ios::binary);
    std::string line_text;
    while (std::getline(stream, line_text)) {
        if (line_text.empty()) continue;
        nlohmann::json line;
        try {
            line = nlohmann::json::parse(line_text);
        } catch (const nlohmann::json::exception&) {
            continue;  // 坏行跳过(账是追加式,单行坏不拦服务)
        }
        out->ledger_lines_.push_back(line);
        if (!line.is_object() || !line.contains("state") ||
            !line.at("state").is_string() || line.at("state") != "ready") {
            continue;
        }
        KnownAttachment known;
        if (line.contains("artifactId") && line.at("artifactId").is_string()) {
            known.artifact_id = line.at("artifactId").get<std::string>();
        }
        if (line.contains("sha256") && line.at("sha256").is_string()) {
            known.sha256 = line.at("sha256").get<std::string>();
        }
        if (line.contains("sizeBytes") && line.at("sizeBytes").is_number_integer()) {
            known.size_bytes = line.at("sizeBytes").get<std::int64_t>();
        }
        if (known.artifact_id.empty() || known.sha256.empty()) {
            continue;
        }
        std::error_code size_ec;
        const std::filesystem::path stored = root / "inbound" /
                                             platform::Utf8ToPath(known.artifact_id + ".bin");
        const std::uintmax_t actual = std::filesystem::file_size(stored, size_ec);
        if (size_ec || actual != static_cast<std::uintmax_t>(known.size_bytes)) {
            continue;  // 原件丢失/不符:重下(known 不进缓存)
        }
        known.ready = true;
        if (line.contains("url") && line.at("url").is_string()) {
            out->known_by_url_[line.at("url").get<std::string>()] = std::move(known);
        }
    }
    return true;
}

void ChannelMediaService::AppendLedgerLine(const nlohmann::json& line) {
    if (!writer_.has_value()) {
        return;
    }
    if (writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        ledger_lines_.push_back(line);
    }
    // 写不进:如实丢这笔观测账(原件已落/未落由 receipt 带回;服务不因
    // 账失败拦正文轮——诊断面 broken 由上层看 ledger 缺口)。
}

std::vector<ChannelMediaService::AttachmentReceipt> ChannelMediaService::Ingest(
    const channel::ChannelInboundEvent& event, std::int64_t ingress_sid,
    const ChannelMediaDownloadFn& download, const ChannelMediaLimits& limits,
    std::int64_t now_ms) {
    std::vector<AttachmentReceipt> receipts;
    std::size_t accepted_index = 0;
    for (const channel::ChannelPart& part : event.parts) {
        const bool media_part = part.type == channel::ChannelPartType::Image ||
                                part.type == channel::ChannelPartType::Audio ||
                                part.type == channel::ChannelPartType::Video ||
                                part.type == channel::ChannelPartType::File;
        if (!media_part || !part.remote_ref.has_value() || part.remote_ref->empty()) {
            continue;
        }
        AttachmentReceipt receipt;
        receipt.original_name =
            SanitizeChannelAttachmentName(part.file_name.value_or(std::string()));
        receipt.mime_type = part.mime_type.value_or(std::string());

        nlohmann::json line = nlohmann::json::object();
        line["kind"] = "inbound";
        line["state"] = "failed";
        line["originalName"] = receipt.original_name;
        line["mimeType"] = receipt.mime_type;
        line["source"] = nlohmann::json{
            {"channel", event.channel_id},
            {"account", event.account_id},
            {"ingressSid", ingress_sid},
            {"providerEventId", event.provider_event_id},
        };
        line["atMs"] = now_ms;
        // url 脱敏记法(§十 10.2:签名 URL 不进普通日志/账)——host+path
        // 落账可诊断,query(带 token/签名)整段剥掉;同一根 url 的幂等
        // 键同源,平台换签不改键。
        const std::string url_key = StripUrlQuery(*part.remote_ref);
        line["url"] = url_key;

        const auto finish = [&](bool ok) {
            line["state"] = ok ? "ready" : "failed";
            if (!receipt.error_code.empty()) {
                line["errorCode"] = receipt.error_code;
            }
            if (ok) {
                line["artifactId"] = receipt.artifact_id;
                line["sha256"] = receipt.sha256;
                line["sizeBytes"] = receipt.size_bytes;
            }
            AppendLedgerLine(line);
            receipts.push_back(std::move(receipt));
        };

        if (accepted_index >= limits.max_attachments_per_message) {
            receipt.error_code = "too_many_attachments";
            receipt.prompt_line = "[附件 " + receipt.original_name + "] 超出单条消息附件"
                                                               "数量上限,本轮不可用。";
            finish(false);
            continue;
        }
        if (!IsAllowedInboundMimeType(receipt.mime_type)) {
            receipt.error_code = "mime_not_allowed";
            receipt.prompt_line = "[附件 " + receipt.original_name + "] 类型不在收件"
                                                        "白名单(" +
                                  receipt.mime_type + "),未下载。";
            finish(false);
            continue;
        }
        ++accepted_index;

        // 幂等:同根 url 已有 ready 原件(崩溃重扫/重放/平台换签)直接复用。
        const auto known = known_by_url_.find(url_key);
        if (known != known_by_url_.end() && known->second.ready) {
            receipt.ready = true;
            receipt.artifact_id = known->second.artifact_id;
            receipt.sha256 = known->second.sha256;
            receipt.size_bytes = known->second.size_bytes;
            receipt.stored_path = platform::PathToUtf8(
                root_ / "inbound" / platform::Utf8ToPath(known->second.artifact_id + ".bin"));
            receipt.prompt_line = "[附件 " + receipt.original_name + "] " +
                                  std::to_string(receipt.size_bytes) + " 字节 | " +
                                  receipt.mime_type + " | sha256:" +
                                  receipt.sha256.substr(0, 16) + " | 已存档:" +
                                  receipt.stored_path;
            finish(true);
            continue;
        }

        if (!download) {
            receipt.error_code = "download_failed:no_downloader";
            receipt.prompt_line =
                "[附件 " + receipt.original_name + "] 渠道未装配下载,本轮不可用。";
            finish(false);
            continue;
        }
        const auto downloaded = download(*part.remote_ref);
        if (!downloaded.has_value()) {
            receipt.error_code = "download_failed:" + downloaded.error();
            receipt.prompt_line = "[附件 " + receipt.original_name + "] 下载失败(" +
                                  downloaded.error() + "),本轮不可用。";
            finish(false);
            continue;
        }
        receipt.sha256 = platform::Sha256Hex(downloaded->bytes);
        receipt.size_bytes = static_cast<std::int64_t>(downloaded->bytes.size());
        receipt.artifact_id = "att-" + receipt.sha256.substr(0, 16);
        const std::filesystem::path stored =
            root_ / "inbound" / platform::Utf8ToPath(receipt.artifact_id + ".bin");

        std::error_code ec;
        bool stored_now = false;
        if (!std::filesystem::exists(stored, ec) || ec) {
            const auto write = platform::AtomicWriteFile(
                stored, downloaded->bytes, platform::WriteDurability::ProcessCrashDurability);
            if (!write.has_value()) {
                receipt.error_code = "store_failed";
                receipt.prompt_line = "[附件 " + receipt.original_name +
                                      "] 落盘失败,本轮不可用。";
                finish(false);
                continue;
            }
            stored_now = true;
        }
        receipt.ready = true;
        receipt.stored_path = platform::PathToUtf8(stored);
        receipt.prompt_line = "[附件 " + receipt.original_name + "] " +
                              std::to_string(receipt.size_bytes) + " 字节 | " +
                              receipt.mime_type + " | sha256:" + receipt.sha256.substr(0, 16) +
                              " | 已存档:" + receipt.stored_path;
        if (IsTextLikeMimeType(receipt.mime_type)) {
            const std::string& bytes = downloaded->bytes;
            const bool has_nul = ContainsNulInPrefix(bytes, limits.max_preview_bytes);
            if (bytes.empty()) {
                receipt.prompt_line += "\n(空文件)";
            } else if (has_nul) {
                receipt.prompt_line += "\n(内容含二进制字节,未预览;可用 read_file 读取)";
            } else {
                const std::string preview = TruncateUtf8(bytes, limits.max_preview_bytes);
                receipt.prompt_line += "\n预览(前 " + std::to_string(preview.size()) +
                                       " 字节):\n" + preview;
                if (preview.size() < bytes.size()) {
                    receipt.prompt_line += "\n(预览截断)";
                }
            }
            // 受控读取口恒在提示里(§十 Q4:大文件给受控读取口,沿 Skill
            // 受控资源读取的路径纪律——read_file 绝对路径直读,offset/
            // limit 有界)。
            receipt.prompt_line += "\n(完整内容可用 read_file 工具读取该路径;"
                                   "大文件用 offset/limit 分段读)";
        } else {
            receipt.prompt_line += "\n(非文本原件已存档；图片由宿主尝试接入视觉输入，文档须另用解析工具或技能。"
                                   "read_file 不能代替图片识别、PDF/Office 解析。)";
        }
        (void)stored_now;
        known_by_url_[url_key] =
            KnownAttachment{receipt.artifact_id, receipt.sha256, receipt.size_bytes, true};
        finish(true);
    }
    return receipts;
}

std::optional<api::ImageBlock> LoadChannelImage(const ChannelMediaService::AttachmentReceipt& receipt) {
    if (!receipt.ready || receipt.size_bytes <= 0 || receipt.size_bytes > 20 * 1024 * 1024) return std::nullopt;
    const auto& mime = receipt.mime_type;
    if (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp")
        return std::nullopt;
    std::ifstream stream(platform::Utf8ToPath(receipt.stored_path), std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(receipt.size_bytes), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(stream.gcount()) != bytes.size() || stream.peek() != EOF ||
        platform::Sha256Hex(bytes) != receipt.sha256) return std::nullopt;
    const auto dimensions = agent::ReadImageDimensions(bytes, mime);
    if (agent::SniffImageFormat(bytes).mime_type != mime || dimensions.width == 0 || dimensions.height == 0)
        return std::nullopt;
    return api::ImageBlock{mime, platform::Base64Encode(bytes), receipt.original_name,
                            dimensions.width, dimensions.height};
}

}  // namespace lubancode::runtime
