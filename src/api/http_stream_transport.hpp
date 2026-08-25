// 四家 wire(anthropic/chat/gemini/responses)client 共用的 cpr 流式传输
// 骨架。批六(API 传输合流)之前这副骨架在四份 client.cpp 里各抄一份
// (Header/Write/Progress 回调五连 + 收场分型,约 190 行/份),如今收拢成
// 这一处。client 只剩两件事:拼 url/headers/body(协议形状),把 2xx 响应
// 体喂给自家的分帧器/事件解析器(协议语义)。
//
// 行为契约(与合流前逐字节一致,tests/integration/process/
// test_network_timeout.cpp 拿真 socket 钉着):
//   - cancel 非空且置位:Write/Progress 回调里就地掐流,收场报
//     ErrorKind::Cancelled,不当网络错;
//   - 硬墙钟(request_hard_timeout_secs > 0):ProgressCallback 里比
//     steady_clock 期限掐流,收场文案优先走 hard_timeout 档;
//   - OPERATION_TIMEDOUT 按"收到过响应体字节没有"分型成连接超时/流空闲
//     超时(CURLOPT 两处共用同一个错误码,只能靠旁证分);
//   - 非 2xx:响应体原样攒进 Error(不喂 sink,免得分帧器瞎解析);
//   - sink 返回 false(单帧溢出这类协议绝境):掐流,报 Parse 错。
// "流意外结束(没等到终止事件)"的完整性检查不在这——各家对"终止"的
// 定义不同(MessageDone / parser.Finish()),留给调用方收尾。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api {

// 一次流式 POST 的全部参数:四家 client 各自拼好递进来,传输层不认任何
// 厂商形状。
struct HttpStreamCall {
    std::string url;
    // 发送前翻成 cpr::Header(大小写不敏感,同名再赋值即覆盖)。
    std::map<std::string, std::string> headers;
    std::string body;
    int connect_timeout_ms = 0;         // 连接阶段上限(毫秒)
    int stream_idle_timeout_secs = 0;   // 空闲读超时(秒,cpr::LowSpeed)
    int request_hard_timeout_secs = 0;  // 硬墙钟(秒),<= 0 不设
};

// 2xx 响应体回调:吃到一段调一段。返回 false = 协议已不可信,掐流(分帧
// 器单帧溢出);传输层据此报 Parse 错。非 2xx 的响应体不进来。
using StreamDataSink = std::function<bool(std::string_view)>;

// POST + 流式收体 + 取消/超时/错误分型。失败(取消、网络错、HTTP 非 2xx、
// 帧溢出)返回 Error;流走完返回 void(是否"走完整"由调用方检查)。
std::expected<void, Error> PostSseStream(const HttpStreamCall& call, const StreamDataSink& sink,
                                         const std::atomic<bool>* cancel = nullptr);

// 从 HTTP 状态行(形如 "HTTP/1.1 200 OK")里抠出状态码。抠不出来返回 0。
// 单测直接调;线上只有 PostSseStream 的 HeaderCallback 用。
int ExtractStatusCode(std::string_view header_line);

// 把 cpr 的网络错误翻成人话。分型规则见实现处注释。单测直接调。
std::string ClassifyNetworkError(const cpr::Error& error, bool received_any_bytes, int connect_timeout_ms,
                                 int stream_idle_timeout_secs, bool hard_timeout_hit, int hard_timeout_secs);

// 请求体序列化 + 非法 UTF-8 的最后兜底:干净树直接 dump;dump 抛异常
// (type_error.316,历史里漏网的坏串)则响亮记一笔日志(wire_tag 是
// "anthropic"/"chat"/"gemini"/"responses",日志里认门牌),坏串按
// U+FFFD 清洗后照发——回传错误会把带病历史原样留在会话里,往后每回合
// 必挂,会话等于砖死。绝不抛异常。
std::string DumpRequestBody(const char* wire_tag, const nlohmann::json& body);

}  // namespace lubancode::api
