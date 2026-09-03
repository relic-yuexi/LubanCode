// Base64 内核实现(合同见 base64.hpp)。
#include "platform/base64.hpp"

namespace lubancode::platform {

std::string Base64Encode(std::span<const std::byte> data) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const unsigned triple = (static_cast<unsigned>(data[i]) << 16) |
                                (static_cast<unsigned>(data[i + 1]) << 8) |
                                static_cast<unsigned>(data[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const unsigned pair = static_cast<unsigned>(data[i]) << 16;
        out.push_back(kAlphabet[(pair >> 18) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const unsigned pair = (static_cast<unsigned>(data[i]) << 16) |
                              (static_cast<unsigned>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(pair >> 18) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

}  // namespace lubancode::platform
