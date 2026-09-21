// QQ 接入单 Q4:渠道附件接纳服务册——文件名净化、MIME 白名单、下载
// 落仓(sha256/原子发布)、幂等(同 url 不重下/重启账重放)、失败分型、
// 模型有界预览(UTF-8 边界/二进制不预览)、账面 url 脱敏(query 的
// token 不落账)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "channel/types.hpp"
#include "runtime/channel_media_service.hpp"
#include "runtime/channel_file_delivery.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:半字符回归的合同断言

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

struct MediaDir {
    std::filesystem::path root;

    explicit MediaDir(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-channel-media-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
};

channel::ChannelInboundEvent MakeEvent() {
    channel::ChannelInboundEvent event;
    event.delivery_id = "qq-del-1";
    event.provider_event_id = "M1";
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = "dm-owner";
    event.sender.id = "dm-owner";
    event.message_id = "M1";
    return event;
}

channel::ChannelPart MakeAttachment(const std::string& name, const std::string& url,
                                    const std::string& mime,
                                    channel::ChannelPartType type) {
    channel::ChannelPart part;
    part.type = type;
    part.file_name = name;
    part.remote_ref = url;
    part.mime_type = mime;
    return part;
}

// 计数的假下载器:按 url 回字节,记录调用。
struct FakeDownload {
    std::vector<std::string> calls;
    std::map<std::string, std::string> bytes_by_url;
    std::string fail_url;

    ChannelMediaDownloadFn Fn() {
        return [this](const std::string& url) -> std::expected<ChannelMediaBytes, std::string> {
            calls.push_back(url);
            if (url == fail_url) {
                return std::unexpected("too_large");
            }
            const auto found = bytes_by_url.find(url);
            if (found == bytes_by_url.end()) {
                return std::unexpected("not_found");
            }
            return ChannelMediaBytes{found->second};
        };
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数:净化 / 白名单
// ---------------------------------------------------------------------------

TEST_CASE("净化:路径穿越/控制字符/保留名/限长/空名") {
    CHECK(SanitizeChannelAttachmentName("report.txt") == "report.txt");
    // 路径穿越只留 basename(两种分隔符)。
    CHECK(SanitizeChannelAttachmentName("../../etc/passwd") == "passwd");
    CHECK(SanitizeChannelAttachmentName("..\\..\\win.ini") == "win.ini");
    CHECK(SanitizeChannelAttachmentName("/abs/path/data.csv") == "data.csv");
    // 控制字符与平台非法字符剥除。
    CHECK(SanitizeChannelAttachmentName("a<b>c:d\"e|f?g*h") == "abcdefgh");
    CHECK(SanitizeChannelAttachmentName(std::string("x\ty\nz")) == "xyz");
    // Windows 保留名加前缀(带扩展名同样判)。
    CHECK(SanitizeChannelAttachmentName("CON") == "_CON");
    CHECK(SanitizeChannelAttachmentName("CON.txt") == "_CON.txt");
    CHECK(SanitizeChannelAttachmentName("nul.log") == "_nul.log");
    // 尾点/空格剥(Windows 目录项约束)。
    CHECK(SanitizeChannelAttachmentName("name. ") == "name");
    CHECK(SanitizeChannelAttachmentName("trail ") == "trail");
    // 超长按 UTF-8 边界截断(中文每字 3 字节,80 帽不撕半个字)。
    std::string long_name;
    for (int i = 0; i < 60; ++i) {
        long_name += "汉";
    }
    const std::string truncated = SanitizeChannelAttachmentName(long_name);
    CHECK(truncated.size() <= 80);
    CHECK(truncated.size() % 3 == 0);
    CHECK(truncated.substr(0, 3) == "汉");
    // 空名/纯非法字符回落。
    CHECK(SanitizeChannelAttachmentName("") == "attachment");
    CHECK(SanitizeChannelAttachmentName("..") == "attachment");
    CHECK(SanitizeChannelAttachmentName("???") == "attachment");
}

TEST_CASE("白名单:text/image 前缀放行;平台枚举与常见档精确;其余拒") {
    CHECK(IsAllowedInboundMimeType("text/plain"));
    CHECK(IsAllowedInboundMimeType("image/png"));
    CHECK(IsAllowedInboundMimeType("voice"));
    CHECK(IsAllowedInboundMimeType("file"));
    CHECK(IsAllowedInboundMimeType("application/pdf"));
    CHECK(IsAllowedInboundMimeType("application/zip"));
    CHECK(IsAllowedInboundMimeType("video/mp4"));
    CHECK_FALSE(IsAllowedInboundMimeType(""));
    CHECK_FALSE(IsAllowedInboundMimeType("application/x-msdownload"));
    CHECK_FALSE(IsAllowedInboundMimeType("video/quicktime"));
    CHECK(IsTextLikeMimeType("text/csv"));
    CHECK(IsTextLikeMimeType("application/json"));
    CHECK_FALSE(IsTextLikeMimeType("image/png"));
    CHECK_FALSE(IsTextLikeMimeType("voice"));
}

// ---------------------------------------------------------------------------
// Ingest:下载落仓/幂等/失败分型/预览
// ---------------------------------------------------------------------------

TEST_CASE("Ingest:文本附件下载落仓,sha256 命名,预览有界且账面无 token") {
    MediaDir dir("ingest-text");
    ChannelMediaService service;
    REQUIRE(ChannelMediaService::Open(&service, dir.root));

    FakeDownload download;
    download.bytes_by_url["https://multimedia.nt.qq.com/d?token=SECTOK"] =
        "第一行\n第二行\n第三行";

    auto event = MakeEvent();
    event.parts.push_back(channel::ChannelPart{});
    event.parts[0].type = channel::ChannelPartType::Text;
    event.parts[0].text = std::string("看这个文件");
    event.parts.push_back(MakeAttachment(
        "../notes.txt", "https://multimedia.nt.qq.com/d?token=SECTOK", "text/plain",
        channel::ChannelPartType::File));

    const auto receipts = service.Ingest(event, /*ingress_sid=*/7, download.Fn(),
                                         ChannelMediaLimits{}, 1'000);
    REQUIRE(receipts.size() == 1);
    const auto& receipt = receipts[0];
    REQUIRE(receipt.ready);
    CHECK(receipt.original_name == "notes.txt");  // 穿越名净化
    CHECK(receipt.mime_type == "text/plain");
    CHECK(receipt.size_bytes > 0);
    CHECK(receipt.artifact_id.rfind("att-", 0) == 0);
    CHECK(receipt.artifact_id.size() == 20);  // att- + sha256 前 16
    // 原件真落盘且内容逐字节相符。
    std::ifstream stored(receipt.stored_path, std::ios::binary);
    REQUIRE(stored.good());
    const std::string content((std::istreambuf_iterator<char>(stored)),
                              std::istreambuf_iterator<char>());
    CHECK(content == "第一行\n第二行\n第三行");
    // 预览:小文件全文进投影,截断提示不出现。
    CHECK(receipt.prompt_line.find("已存档") != std::string::npos);
    CHECK(receipt.prompt_line.find("第一行") != std::string::npos);
    CHECK(receipt.prompt_line.find("read_file") != std::string::npos);
    // 账:ready 行,url 脱敏(query 的 token 不落账)。
    const auto lines = service.ledger_lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].at("state") == "ready");
    CHECK(lines[0].at("source").at("ingressSid") == 7);
    CHECK(lines[0].at("url").get<std::string>().find("SECTOK") == std::string::npos);
    CHECK(lines[0].at("url").get<std::string>().find("token") == std::string::npos);
}

TEST_CASE("Ingest:长文本预览截断在 UTF-8 边界;二进制(含 NUL)不预览") {
    MediaDir dir("ingest-preview");
    ChannelMediaService service;
    REQUIRE(ChannelMediaService::Open(&service, dir.root));
    ChannelMediaLimits limits;
    limits.max_preview_bytes = 9;  // 中文 3 字节/字 → 3 字。

    FakeDownload download;
    std::string long_text;
    for (int i = 0; i < 20; ++i) {
        long_text += "汉字";
    }
    download.bytes_by_url["https://x.qq.com/long"] = long_text;
    download.bytes_by_url["https://x.qq.com/bin"] = std::string("a\0b", 3);

    auto event = MakeEvent();
    event.parts.push_back(MakeAttachment("long.txt", "https://x.qq.com/long", "text/plain",
                                         channel::ChannelPartType::File));
    event.parts.push_back(MakeAttachment("blob.txt", "https://x.qq.com/bin", "text/plain",
                                         channel::ChannelPartType::File));
    const auto receipts = service.Ingest(event, 1, download.Fn(), limits, 1'000);
    REQUIRE(receipts.size() == 2);
    REQUIRE(receipts[0].ready);
    // 截断落在字边界(9 字节 = 3 个汉字),提示截断。
    CHECK(receipts[0].prompt_line.find("预览截断") != std::string::npos);
    CHECK(receipts[0].prompt_line.find("汉字") != std::string::npos);
    REQUIRE(receipts[1].ready);
    CHECK(receipts[1].prompt_line.find("二进制") != std::string::npos);
    CHECK(receipts[1].prompt_line.find("read_file") != std::string::npos);
}

TEST_CASE("Ingest:预览帽容不下首个码点,宁空勿半(AR-11 半字符回归)") {
    MediaDir dir("ingest-preview-narrow");
    ChannelMediaService service;
    REQUIRE(ChannelMediaService::Open(&service, dir.root));
    ChannelMediaLimits limits;
    limits.max_preview_bytes = 2;  // "汉"三字节,预算 2 装不下。

    FakeDownload download;
    download.bytes_by_url["https://x.qq.com/cjk"] = "汉";

    auto event = MakeEvent();
    event.parts.push_back(MakeAttachment("cjk.txt", "https://x.qq.com/cjk", "text/plain",
                                         channel::ChannelPartType::File));
    const auto receipts = service.Ingest(event, 1, download.Fn(), limits, 1'000);
    REQUIRE(receipts.size() == 1);
    REQUIRE(receipts[0].ready);
    // 旧病:切点退到 0 后又回退到 max_bytes,放出一个首字节(半字符),
    // prompt_line 随之变非法 UTF-8。修后预览为空,截断提示照给。
    CHECK(platform::IsValidUtf8(receipts[0].prompt_line));
    CHECK(receipts[0].prompt_line.find("预览截断") != std::string::npos);
}

TEST_CASE("Ingest:白名单外拒;下载失败如实记账;附件数帽") {
    MediaDir dir("ingest-fail");
    ChannelMediaService service;
    REQUIRE(ChannelMediaService::Open(&service, dir.root));

    FakeDownload download;
    download.bytes_by_url["https://x.qq.com/ok"] = "ok";
    download.bytes_by_url["https://x.qq.com/a"] = "a";

    auto event = MakeEvent();
    event.parts.push_back(MakeAttachment("bad.exe", "https://x.qq.com/bad", "application/x-msdownload",
                                         channel::ChannelPartType::File));
    event.parts.push_back(MakeAttachment("lost.txt", "https://x.qq.com/lost", "text/plain",
                                         channel::ChannelPartType::File));
    event.parts.push_back(MakeAttachment("ok.txt", "https://x.qq.com/ok", "text/plain",
                                         channel::ChannelPartType::File));
    download.fail_url = "https://x.qq.com/lost";
    const auto receipts = service.Ingest(event, 1, download.Fn(),
                                         ChannelMediaLimits{}, 1'000);
    REQUIRE(receipts.size() == 3);
    CHECK_FALSE(receipts[0].ready);
    CHECK(receipts[0].error_code == "mime_not_allowed");
    CHECK(receipts[0].prompt_line.find("白名单") != std::string::npos);
    CHECK_FALSE(receipts[1].ready);
    CHECK(receipts[1].error_code == "download_failed:too_large");
    REQUIRE(receipts[2].ready);  // 单枚失败不拦其他附件

    // 附件数帽:同信第四枚起 too_many。
    auto big = MakeEvent();
    ChannelMediaLimits tiny;
    tiny.max_attachments_per_message = 1;
    big.parts.push_back(MakeAttachment("a.txt", "https://x.qq.com/a", "text/plain",
                                       channel::ChannelPartType::File));
    big.parts.push_back(MakeAttachment("b.txt", "https://x.qq.com/b", "text/plain",
                                       channel::ChannelPartType::File));
    const auto capped = service.Ingest(big, 2, download.Fn(), tiny, 1'000);
    REQUIRE(capped.size() == 2);
    CHECK(capped[0].ready);
    CHECK_FALSE(capped[1].ready);
    CHECK(capped[1].error_code == "too_many_attachments");
}

TEST_CASE("Ingest:同 url 幂等不重下;重启(重开仓)从账恢复也复用") {
    MediaDir dir("ingest-idempotent");
    {
        ChannelMediaService service;
        REQUIRE(ChannelMediaService::Open(&service, dir.root));
        FakeDownload download;
        // 假下载器按完整 url 查表(首次携 token=T1);幂等键在服务里按
        // 去 query 的根 url,换签(T2/T3)命中账不重下。
        download.bytes_by_url["https://x.qq.com/same?token=T1"] = "stable";
        auto event = MakeEvent();
        event.parts.push_back(MakeAttachment("s.txt", "https://x.qq.com/same?token=T1",
                                             "text/plain", channel::ChannelPartType::File));
        const auto first = service.Ingest(event, 1, download.Fn(),
                                          ChannelMediaLimits{}, 1'000);
        REQUIRE(first.size() == 1);
        REQUIRE(first[0].ready);
        // 同信重扫(平台换签 token):同根 url(query 剥掉后同键)复用,不重下。
        auto replay = MakeEvent();
        replay.parts.push_back(MakeAttachment("s.txt", "https://x.qq.com/same?token=T2",
                                              "text/plain", channel::ChannelPartType::File));
        const auto second = service.Ingest(replay, 1, download.Fn(),
                                           ChannelMediaLimits{}, 2'000);
        REQUIRE(second.size() == 1);
        REQUIRE(second[0].ready);
        CHECK(download.calls.size() == 1);
    }
    {
        // 重启:重开仓(账重放 + 原件在)→ 复用,零下载。
        ChannelMediaService reopened;
        REQUIRE(ChannelMediaService::Open(&reopened, dir.root));
        FakeDownload download;
        auto event = MakeEvent();
        event.parts.push_back(MakeAttachment("s.txt", "https://x.qq.com/same?token=T3",
                                             "text/plain", channel::ChannelPartType::File));
        const auto receipts = reopened.Ingest(event, 1, download.Fn(),
                                              ChannelMediaLimits{}, 3'000);
        REQUIRE(receipts.size() == 1);
        REQUIRE(receipts[0].ready);
        CHECK(download.calls.empty());
    }
}

TEST_CASE("Ingest:未装配下载 seam 时如实报,不假装读过") {
    MediaDir dir("ingest-nodownloader");
    ChannelMediaService service;
    REQUIRE(ChannelMediaService::Open(&service, dir.root));
    auto event = MakeEvent();
    event.parts.push_back(MakeAttachment("x.txt", "https://x.qq.com/x", "text/plain",
                                         channel::ChannelPartType::File));
    const auto receipts = service.Ingest(event, 1, nullptr, ChannelMediaLimits{}, 1'000);
    REQUIRE(receipts.size() == 1);
    CHECK_FALSE(receipts[0].ready);
    CHECK(receipts[0].error_code == "download_failed:no_downloader");
}

TEST_CASE("send_file freezes original, isolates turns and rejects paths outside workspace") {
    MediaDir dir("outbound");
    const auto workspace = dir.root / "workspace";
    const auto staging = dir.root / "staging";
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "report.txt") << "original";
    std::ofstream(dir.root / "private.txt") << "outside";
    tools::ToolRegistry registry;
    RegisterChannelFileTool(registry);
    auto* tool = registry.Find("send_file");
    REQUIRE(tool != nullptr);
    CHECK(tool->needs_confirm());
    CHECK(tool->execute({{"path", "report.txt"}}).is_error);
    {
        ChannelFileDeliveryScope scope(workspace, staging, "turn-one");
        CHECK(tool->execute({{"path", "../private.txt"}}).is_error);
#ifndef _WIN32
        const auto sibling = dir.root / "WORKSPACE";
        std::filesystem::create_directories(sibling);
        // Case-insensitive filesystems may alias it to workspace; either way it cannot
        // be used to cross to a distinct canonical directory.
        std::error_code case_ec;
        if (!std::filesystem::equivalent(sibling, workspace, case_ec)) {
            std::ofstream(sibling / "secret.txt") << "private";
            CHECK(tool->execute({{"path", platform::PathToUtf8(sibling / "secret.txt")}}).is_error);
        }
#endif
        CHECK_FALSE(tool->execute({{"path", "report.txt"}}).is_error);
        CHECK_FALSE(tool->execute({{"path", "report.txt"}}).is_error);
        std::ofstream(workspace / "report.txt") << "modified";
        CHECK(tool->execute({{"path", "report.txt"}}).is_error);
    }
    const auto frozen = StagedChannelFile(staging, "turn-one");
    REQUIRE(frozen.has_value());
    std::ifstream bytes(platform::Utf8ToPath(frozen->local_path));
    std::string text; bytes >> text;
    CHECK(text == "original");
    CHECK_FALSE(StagedChannelFile(staging, "turn-two").has_value());
    CHECK(tool->execute({{"path", "report.txt"}}).is_error);
}

TEST_CASE("accepted image enters vision only when bytes, hash and dimensions agree") {
    MediaDir dir("vision");
    // GIF header with dimensions, same parser used by model image handling.
    const std::string bytes("GIF89a\x01\x00\x01\x00\x00\x00\x00", 13);
    const auto path = dir.root / "image.bin";
    std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
    ChannelMediaService::AttachmentReceipt receipt;
    receipt.ready = true; receipt.stored_path = platform::PathToUtf8(path);
    receipt.mime_type = "image/gif"; receipt.original_name = "photo.gif";
    receipt.size_bytes = bytes.size(); receipt.sha256 = platform::Sha256Hex(bytes);
    const auto image = LoadChannelImage(receipt);
    REQUIRE(image.has_value());
    CHECK(image->width == 1);
    CHECK(image->filename == "photo.gif");
    receipt.sha256 = "wrong";
    CHECK_FALSE(LoadChannelImage(receipt).has_value());
    receipt.sha256 = platform::Sha256Hex(bytes); receipt.mime_type = "application/pdf";
    CHECK_FALSE(LoadChannelImage(receipt).has_value());
}
