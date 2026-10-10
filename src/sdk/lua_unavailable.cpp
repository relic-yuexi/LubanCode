#include "sdk/lua.hpp"

namespace lubancore::detail {

Result<void> SessionLua::Load(const lua::v1::Selection&) {
    return std::unexpected(Error{"sdk.lua.build_unavailable", "This SDK was built without Lua"});
}

} // namespace lubancore::detail
