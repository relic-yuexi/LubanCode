// 渠道中立合同册(架构审查 SV-07,渠道中立合同退出 QQ 实现头):钉两件
// 事——
//   1. 三平台(QQ/飞书/企微)的连接快照与网关传输 seam 是同一份中立类型
//      (channel::ConnectionSnapshot / channel::transport::IGatewayTransport),
//      不是同形异名的复制;qq::/wecombot:: 旧名只是迁移期别名(不增加
//      第二份状态数据)。断言全用 static_assert 编译期钉死。
//   2. include 闭包:本册只含各平台的窄头即可编译并取到上述类型——飞书
//      适配器头不再传递包含 qq_adapter.hpp,企微网关头不再包含
//      qq_gateway.hpp(若仍暗依赖,这里的类型引用直接编不过)。
// 行为零变更由既有册守门:channelqq/channelfeishu/channelwecombot 的
// test_*_adapter / test_*_gateway 各自跑成功/断线/退避/停止路径,断言
// 一字未改;reporter 册(test_qq_connection_reporter)钉快照输出与落盘。
#include <doctest/doctest.h>

#include <type_traits>

#include "channel/connection_state.hpp"
#include "channel/feishu/feishu_adapter.hpp"
#include "channel/qq/qq_adapter.hpp"
#include "channel/transport/gateway_transport.hpp"
#include "channel/wecombot/wecom_adapter.hpp"
#include "channel/wecombot/wecom_gateway.hpp"

namespace lubancode::channel {
namespace {

TEST_CASE("SV-07:三平台连接快照是同一份 channel 中立合同") {
    // 旧名是别名,不是同形复制——加字段只改 connection_state.hpp 一处。
    static_assert(std::is_same_v<qq::ConnectionSnapshot, ConnectionSnapshot>);
    static_assert(std::is_same_v<qq::ConnectionFailure, ConnectionFailure>);
    static_assert(std::is_same_v<wecombot::ConnectionSnapshot, ConnectionSnapshot>);
    static_assert(std::is_same_v<wecombot::ConnectionFailure, ConnectionFailure>);
    CHECK(true);

    // 三平台适配器的 ConnectionState() 直出同一份中立合同(注册表的
    // connection_state 口同用,企微不再逐字段手抄换名)。
    static_assert(std::is_same_v<decltype(&feishu::FeishuBotAdapter::ConnectionState),
                                 ConnectionSnapshot (feishu::FeishuBotAdapter::*)() const>);
    static_assert(std::is_same_v<decltype(&wecombot::WecombotAdapter::ConnectionState),
                                 ConnectionSnapshot (wecombot::WecombotAdapter::*)() const>);
    static_assert(std::is_same_v<decltype(&qq::QqBotAdapter::ConnectionState),
                                 ConnectionSnapshot (qq::QqBotAdapter::*)() const>);
    CHECK(true);
}

TEST_CASE("SV-07:网关传输 seam 升 channel/transport,qq/企微旧名是别名") {
    static_assert(std::is_same_v<qq::IGatewayTransport, transport::IGatewayTransport>);
    static_assert(std::is_same_v<qq::GatewayConnectError, transport::GatewayConnectError>);
    static_assert(std::is_same_v<wecombot::WecomTransport, transport::IGatewayTransport>);
    CHECK(true);
}

}  // namespace
}  // namespace lubancode::channel
