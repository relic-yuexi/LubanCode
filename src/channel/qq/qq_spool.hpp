// QQ sidecar spool(QQ 机器人接入单 Q1):"先落盘再上报,ACK 后清理"的入站
// 水路耐久账(单 §五映射表 channel.inbound 行)。
//
// 与宿主 ingress 账各归各(单 §五末行):这里只保"适配器已收到、宿主还没
// 确认"的平台事件;宿主 ACK(inbound.ack)一到即删。重启后 ListPending 全部
// 重投——宿主 ingress 的去重键(provider_event_id)保证重投不重复入账。
//
// 落盘:原子写(ProcessCrashDurability 档——这是事实账,断电承诺要到
// "要么整笔在、要么整笔不在")。文件名只认 [A-Za-z0-9._-] 的 delivery_id,
// 拼不出目录外路径。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::channel::qq {

class QqSpoolStore {
public:
    // 打开(建)spool 目录:<state_root>/qqbot/<account>/spool/pending。
    static std::expected<QqSpoolStore, std::string> Open(const std::filesystem::path& pending_dir);

    QqSpoolStore() = default;
    QqSpoolStore(QqSpoolStore&& other) noexcept = default;
    QqSpoolStore& operator=(QqSpoolStore&& other) noexcept = default;

    // 落一笔待确认事件(整份规范化事件 JSON)。失败返回人话错误——调用方
    // 停止上报该事件(不丢弃、不假称已耐久)。
    std::optional<std::string> AppendPending(const std::string& delivery_id,
                                             const nlohmann::json& event_json);

    // 重启重投:全部待确认事件,按 delivery_id 排序(重投次序稳定)。
    std::vector<std::pair<std::string, nlohmann::json>> ListPending() const;

    // 宿主 ACK 后清理。未知 delivery_id 不报错(重复 ACK 幂等)。
    std::optional<std::string> RemoveAcked(const std::string& delivery_id);

    std::size_t pending_count() const;

    const std::filesystem::path& dir() const { return pending_dir_; }

private:
    explicit QqSpoolStore(std::filesystem::path dir) : pending_dir_(std::move(dir)) {}
    std::filesystem::path pending_dir_;
};

// delivery_id 字符集守门(文件名成分)。
bool IsValidSpoolDeliveryId(const std::string& delivery_id);

}  // namespace lubancode::channel::qq
