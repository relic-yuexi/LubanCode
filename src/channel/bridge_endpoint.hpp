// InProcessBridgeEndpoint:进程内 Channel Bridge 端点共用件(架构审查
// SV-08,三渠道收发机械合并)。QQ/飞书/企微三只适配器原先各自实现同一套
// 帧收发机械——宿主来向解码循环、to_host 出站缓冲、九个"锁内编码入缓冲"
// 函数;这里收拢成一只组合件,独占 FrameDecoder、出站缓冲与一把输出
// mutex,对适配器只露 Feed/Drain/Reply/Notify 窄口。
//
// 职责边界(SV-08 定):本件只管"分帧与线程安全出站缓冲";平台业务
//(HandleHostFrame 分派、握手应答、spool/ACK、网关重连、发送线程)留在
// 各适配器——不设虚基类、不带平台分支。编码失败行为照原三份机械:
// EncodeFrame 出错静默不入缓冲(要不要留痕是独立后续决策,本件不借合并
// 悄悄改宿主合同)。
//
// 线程与锁(照原三份的边界):Feed 只在宿主线程调(宿主在 ChannelManager
// 锁内);解码分派回调不持输出锁——回调里允许同步调 Reply*/Notify(宿主
// 侧典型路径:收到握手请求帧 → 同步回 response),不死锁。Reply*/Notify
// 任意线程可调(网关线程/发送线程/宿主线程),Drain 归宿主线程。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/bridge_protocol.hpp"
#include "channel/frame.hpp"

namespace lubancode::channel {

class InProcessBridgeEndpoint final {
public:
    // 整帧分派口:Feed 每解出一份完整 JSON 正文调一次。回调里同步调本件
    // Reply*/Notify 合法(Feed 调它时不持输出锁)。
    using FrameHandler = std::function<void(const nlohmann::json&)>;

    // 喂宿主来向字节(增量)。整帧交 on_frame;半帧留在解码器等更多字节;
    // 坏帧(超帽/坏 UTF-8/非 object JSON)自发 Fatal(invalid_frame)通知
    // 并停——解码器粘性错,后续 Feed 仍回同一错误、再发 Fatal。
    void Feed(const std::byte* data, std::size_t size, const FrameHandler& on_frame);

    // 取走全部出站字节(取后缓冲清空)。
    std::vector<std::byte> Drain();

    // 出站三口(照原三份机械:锁内构造 JSON、编码成帧、追加进缓冲)。
    void ReplyResult(std::int64_t id, const nlohmann::json& result);
    void ReplyDomainError(std::int64_t id, DomainErrorName name, const std::string& detail);
    void Notify(BridgeMethod method, const nlohmann::json& params);

private:
    // 锁内把一帧 payload 编码并追加进出站缓冲(编码失败静默丢)。
    void AppendFrame(const nlohmann::json& payload);

    std::mutex out_mutex_;            // to_host_ 的账(出站缓冲唯一锁)
    std::vector<std::byte> to_host_;  // sidecar -> 宿主待取字节
    FrameDecoder decoder_;            // 宿主来向帧解码(仅宿主线程喂)
};

}  // namespace lubancode::channel
