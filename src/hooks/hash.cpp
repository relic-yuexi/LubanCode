// definition hash 用的 SHA-256(FIPS 180-4)。算法内核统一住在
// platform/sha256.cpp(src 收口审计 P2:此前 hooks 与 channel 各养一份
// 64 常量/填充/压缩轮,两份各修各的迟早漂移);这里只做领域命名——
// hook 信任按定义哈希记账,命令一改,哈希变,project 信任失效。这层是
// 供应链信任的锚,不用玩具散列(FNV 之类随手工造碰撞),用正经 SHA-256。
#include "hooks/hash.hpp"

#include "platform/sha256.hpp"  // SHA-256 算法内核(字节序/十六进制小写合同冻结)

namespace lubancode::hooks {

std::string Sha256Hex(std::string_view data) {
    return platform::Sha256Hex(data);
}

}  // namespace lubancode::hooks
