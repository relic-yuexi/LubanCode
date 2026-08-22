// 0.28.x"排队消息在工具边界送达并可 Shift+左键编辑"的按键层测试:
//   - platform::MapCsiToKey(纯函数,POSIX CSI 序列的参数表在 Windows 上也
//     能钉死):CSI 1;2D = Shift+Left、CSI 1;5D = Ctrl+Left、裸 CSI D = Left,
//     认不出的序列整个吃掉(None);
//   - 取回键判定(ShouldRecallQueuedMessage/IsQueueRecallKey):正文非空不
//     误触、编辑态不重入、备用键可经环境变量关掉;
//   - 修饰键抬起/IME 组合期的行为落在"正文空是硬前提"上:组合中的半个词
//     先以正文形式落在 composer 里,取回路自然堵死(注释级说明 + 判定函数
//     钉住)。

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "cli/queue_model.hpp"
#include "platform/console.hpp"
#include "platform/csi_keys.hpp"

using lubancode::cli::IsQueueRecallKey;
using lubancode::cli::QueueRecallFallbackEnabled;
using lubancode::cli::ShouldRecallQueuedMessage;
using K = lubancode::platform::KeyInput::Kind;

namespace {

// 设置/还原环境变量(备用键开关测试用)。Windows 的 _putenv 与 POSIX 的
// setenv 都在 <cstdlib> 里,这里包一层小的。
struct EnvGuard {
    explicit EnvGuard(const char* name) : name_(name) {}
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }
    const char* name_;
};

}  // namespace

TEST_CASE("MapCsiToKey:方向键/Home/End/ShiftTab/VT 数字键照旧") {
    using lubancode::platform::MapCsiToKey;
    CHECK(MapCsiToKey("", 'A').kind == K::Up);
    CHECK(MapCsiToKey("", 'B').kind == K::Down);
    CHECK(MapCsiToKey("", 'C').kind == K::Right);
    CHECK(MapCsiToKey("", 'D').kind == K::Left);
    CHECK(MapCsiToKey("", 'H').kind == K::Home);
    CHECK(MapCsiToKey("", 'F').kind == K::End);
    CHECK(MapCsiToKey("", 'Z').kind == K::ShiftTab);
    CHECK(MapCsiToKey("1", '~').kind == K::Home);
    CHECK(MapCsiToKey("7", '~').kind == K::Home);
    CHECK(MapCsiToKey("4", '~').kind == K::End);
    CHECK(MapCsiToKey("3", '~').kind == K::Delete);
    // PageUp/PageDown(会话选择器翻页):CSI 5~ / 6~。
    CHECK(MapCsiToKey("5", '~').kind == K::PageUp);
    CHECK(MapCsiToKey("6", '~').kind == K::PageDown);
}

TEST_CASE("MapCsiToKey:Shift+Left = CSI 1;2D,Ctrl+Left = CSI 1;5D") {
    using lubancode::platform::MapCsiToKey;
    CHECK(MapCsiToKey("1;2", 'D').kind == K::ShiftLeft);
    CHECK(MapCsiToKey("1;5", 'D').kind == K::CtrlLeft);
    // Shift+Ctrl 等其余修饰组合不当取回键:退回普通 Left。
    CHECK(MapCsiToKey("1;6", 'D').kind == K::Left);
    CHECK(MapCsiToKey("5", 'D').kind == K::Left);
    // 方向键之外的 Shift 组合不产 ShiftLeft:Shift+Right 仍按 Right 处理。
    CHECK(MapCsiToKey("1;2", 'C').kind == K::Right);
    CHECK(MapCsiToKey("1;2", 'A').kind == K::Up);
}

TEST_CASE("MapCsiToKey:认不出的序列整个吃掉,参数不漏成正文字符") {
    using lubancode::platform::MapCsiToKey;
    CHECK(MapCsiToKey("200", '~').kind == K::None);   // bracketed paste 由 IO 层先拦
    CHECK(MapCsiToKey("999", '~').kind == K::None);   // 未映射的数字键
    CHECK(MapCsiToKey("1;2", 'x').kind == K::None);   // 未映射的终止字节
    CHECK(MapCsiToKey("5", 'y').kind == K::None);
}

TEST_CASE("ShouldRecallQueuedMessage:正文空、非编辑态、队列非空,三者齐备才取") {
    CHECK(ShouldRecallQueuedMessage(/*composer_empty=*/true, /*editing=*/false, /*queue_size=*/1));
    CHECK(ShouldRecallQueuedMessage(true, false, 3));
    // 正文非空:Shift+Left 保持 composer 既有光标语义,不抢(含 IME 组合期
    // ——组合中的半个词先落在正文里,这条路自然堵死)。
    CHECK_FALSE(ShouldRecallQueuedMessage(false, false, 3));
    // 已在编辑态:不重入。
    CHECK_FALSE(ShouldRecallQueuedMessage(true, true, 3));
    // 队列空:无事可取。
    CHECK_FALSE(ShouldRecallQueuedMessage(true, false, 0));
}

TEST_CASE("IsQueueRecallKey:Shift+Left 恒认,Ctrl+Left 是可配置备用键") {
    CHECK(IsQueueRecallKey(K::ShiftLeft));
    CHECK_FALSE(IsQueueRecallKey(K::Left));
    CHECK_FALSE(IsQueueRecallKey(K::Right));
    CHECK_FALSE(IsQueueRecallKey(K::Char));

    // 默认:备用键开。
    CHECK(QueueRecallFallbackEnabled());
    CHECK(IsQueueRecallKey(K::CtrlLeft));

    EnvGuard guard("LUBANCODE_QUEUE_RECALL_FALLBACK");
    guard.set("none");
    CHECK_FALSE(QueueRecallFallbackEnabled());
    CHECK_FALSE(IsQueueRecallKey(K::CtrlLeft));
    CHECK(IsQueueRecallKey(K::ShiftLeft));  // 主键不受备用键开关影响

    guard.set("off");
    CHECK_FALSE(QueueRecallFallbackEnabled());
    guard.set("0");
    CHECK_FALSE(QueueRecallFallbackEnabled());
    guard.set("FALSE");
    CHECK_FALSE(QueueRecallFallbackEnabled());
    guard.set("ctrl-left");
    CHECK(QueueRecallFallbackEnabled());  // 其它取值一律按开处理
}
