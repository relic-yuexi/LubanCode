#include "tools/lua_tool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace lubancode::tools {

namespace {

// 转换深度上限:防插件写出自引用表(a.x = a)把 LuaValueToJson 递归递死。
constexpr int kMaxDepth = 64;

}  // namespace

// 每枚 state 一份的运行时账(公开给 .hpp 的前向声明兑现)。
struct LuaGuard {
    std::uint64_t instructions_used = 0;
    std::uint64_t instruction_budget = 0;   // 0 = 不设
    std::size_t memory_used = 0;
    std::size_t memory_cap = 0;             // 0 = 不设
    const std::atomic<bool>* cancel = nullptr;
    bool budget_hit = false;
};

namespace {

// instruction hook 的步长:每这么多条虚拟机指令走一次 hook。太密费宿主
// CPU,太疏取消/预算落锤慢;1e5 是 Lua 生态惯用的量级。
constexpr int kHookStride = 100'000;

LuaProfile PureDefaultImpl() {
    LuaProfile profile;
    profile.level = LuaProfile::Level::Pure;
    return profile;
}

LuaProfile TrustedDefaultImpl() {
    LuaProfile profile;
    profile.level = LuaProfile::Level::Trusted;
    // trusted 全开:预算与帽不设(信任是给的,不是猜的;要细帽自己写
    // profile 再灌)。
    profile.instruction_budget = 0;
    profile.memory_cap_bytes = 0;
    return profile;
}

// 底下的原生 allocator(realloc/free):lua_Alloc 契约:NULL 释放,osize
// 只在 ptr!=NULL 时才有意义。包一层记账,超帽返回 NULL——Lua 把 NULL
// 当 OOM 走 luaL_error,宿主堆不破。
void* GuardedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* guard = static_cast<LuaGuard*>(ud);
    if (nsize == 0) {
        if (guard->memory_used >= osize) {
            guard->memory_used -= osize;
        }
        std::free(ptr);
        return nullptr;
    }
    if (guard->memory_cap > 0 && guard->memory_used + nsize > guard->memory_cap) {
        return nullptr;  // Lua 把 NULL 当 OOM,luaL_error 走脚本错误,宿主不倒
    }
    void* next = std::realloc(ptr, nsize);
    if (next != nullptr) {
        // realloc 成功才记账;失败按没动算(Lua 会走 OOM)。
        if (guard->memory_used >= osize) {
            guard->memory_used -= osize;
        }
        guard->memory_used += nsize;
    }
    return next;
}

// instruction hook(LUA_MASKCOUNT):数指令、查取消、查预算。luaL_error 是
// longjmp 路子,pcall 接得住——宿主栈不破。
// guard 的通道:lua_sethook 没有 ud 参数,hook 也不是 C closure(upvalue
// 那条路不通),所以把 guard 塞进 registry 的固定 light userdata 键里。
const char* kGuardRegistryKey = "lubancode.lua.guard";

void GuardHook(lua_State* L, lua_Debug*) {
    lua_pushstring(L, kGuardRegistryKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* guard = static_cast<LuaGuard*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (guard == nullptr) {
        return;
    }
    guard->instructions_used += static_cast<std::uint64_t>(kHookStride);
    if (guard->cancel != nullptr && guard->cancel->load()) {
        luaL_error(L, "用户取消(ESC):lua 脚本已终止");
        return;
    }
    if (guard->instruction_budget > 0 && guard->instructions_used >= guard->instruction_budget) {
        guard->budget_hit = true;
        // luaL_error 走 lua_pushfstring,格式符只认 Lua 白名单(%d/%s/%f/%p/%I),
        // C 的 %llu 不行——预算数先拼进字符串再交。
        luaL_error(L, ("cpu 指令预算耗尽(约 " + std::to_string(guard->instruction_budget) +
                       " 条虚拟机指令):改小输入或拆小任务")
                          .c_str());
    }
}

// 造一枚带三道墙(allocator 内存帽/hook 指令预算/取消链)的 state。
// 预算与帽由 profile 定;取消旗是每轮执行期才灌的,存 guard 里由 hook 查。
// guard 的寿命:LuaTool 持 unique_ptr,state close 之后回调面消失,安全。
lua_State* NewGuardedState(const LuaProfile& profile, std::unique_ptr<LuaGuard>& guard_out) {
    auto guard = std::make_unique<LuaGuard>();
    guard->instruction_budget = profile.instruction_budget;
    guard->memory_cap = profile.memory_cap_bytes;
    lua_State* L = lua_newstate(GuardedAlloc, guard.get());
    if (L == nullptr) {
        return nullptr;
    }
    // registry 里塞 light userdata(hook 的取货口)。
    lua_pushstring(L, kGuardRegistryKey);
    lua_pushlightuserdata(L, guard.get());
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (profile.instruction_budget > 0) {
        lua_sethook(L, GuardHook, LUA_MASKCOUNT, kHookStride);
    }
    guard_out = std::move(guard);
    return L;
}

// pure 画像关门:os.execute / os.exit / io / package.loadlib 拿掉。
// luaL_openlibs 开全之后做减法,比挨个 luaopen 少踩内部初始化的坑
// (io 的 __gc 元方法在关 io 表后照旧有效,文件柄照收)。
void ApplyPureProfile(lua_State* L) {
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "execute");
        lua_pushnil(L);
        lua_setfield(L, -2, "exit");
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setglobal(L, "io");
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "loadlib");
    }
    lua_pop(L, 1);
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// JSON -> lua 值,压到栈顶。字符串按字节原样搬(lua 字符串就是字节串,
// UTF-8 中文不过手不转码)。null 压 nil——出现在表值里等于"这个键不存在",
// 跟 lua 自己的语义一致。
void PushJsonToLua(lua_State* L, const nlohmann::json& value) {
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            lua_pushnil(L);
            break;
        case nlohmann::json::value_t::boolean:
            lua_pushboolean(L, value.get<bool>() ? 1 : 0);
            break;
        case nlohmann::json::value_t::number_integer:
            lua_pushinteger(L, static_cast<lua_Integer>(value.get<std::int64_t>()));
            break;
        case nlohmann::json::value_t::number_unsigned:
            lua_pushinteger(L, static_cast<lua_Integer>(value.get<std::uint64_t>()));
            break;
        case nlohmann::json::value_t::number_float:
            lua_pushnumber(L, static_cast<lua_Number>(value.get<double>()));
            break;
        case nlohmann::json::value_t::string: {
            const auto& s = value.get_ref<const std::string&>();
            lua_pushlstring(L, s.data(), s.size());
            break;
        }
        case nlohmann::json::value_t::array: {
            lua_createtable(L, static_cast<int>(value.size()), 0);
            lua_Integer index = 1;
            for (const auto& element : value) {
                PushJsonToLua(L, element);
                lua_rawseti(L, -2, index);
                ++index;
            }
            break;
        }
        case nlohmann::json::value_t::object: {
            lua_createtable(L, 0, static_cast<int>(value.size()));
            for (const auto& [key, element] : value.items()) {
                lua_pushlstring(L, key.data(), key.size());
                PushJsonToLua(L, element);
                lua_rawset(L, -3);
            }
            break;
        }
        default:  // binary/discarded 不该出现在模型入参里,兜底成 nil
            lua_pushnil(L);
            break;
    }
}

// lua 值 -> JSON。表按"键是不是恰好 1..n 的连续整数"判定数组/对象;对象键
// 只收字符串和数字(数字键转成十进制字符串),别的键(函数、表当键这种
// 歪路子)跳过。深度超限、遇到没法表达的值(函数、userdata)报错误串。
nlohmann::json LuaValueToJson(lua_State* L, int index, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "表嵌套超过 " + std::to_string(kMaxDepth) + " 层(是不是自引用了?)";
        return nullptr;
    }
    const int abs_index = lua_absindex(L, index);
    switch (lua_type(L, abs_index)) {
        case LUA_TNIL:
            return nullptr;
        case LUA_TBOOLEAN:
            return lua_toboolean(L, abs_index) != 0;
        case LUA_TNUMBER:
            if (lua_isinteger(L, abs_index) != 0) {
                return static_cast<std::int64_t>(lua_tointeger(L, abs_index));
            }
            return static_cast<double>(lua_tonumber(L, abs_index));
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, abs_index, &len);
            return std::string(s, len);
        }
        case LUA_TTABLE: {
            // 先数一遍:全部键都是 1..n 连续整数才算数组。
            lua_Integer max_index = 0;
            std::size_t total_keys = 0;
            bool array_like = true;
            lua_pushnil(L);
            while (lua_next(L, abs_index) != 0) {
                ++total_keys;
                if (lua_isinteger(L, -2) != 0) {
                    const lua_Integer k = lua_tointeger(L, -2);
                    if (k < 1) {
                        array_like = false;
                    } else {
                        max_index = (std::max)(max_index, k);
                    }
                } else {
                    array_like = false;
                }
                lua_pop(L, 1);  // 弹值留键,继续 next
            }
            array_like = array_like && static_cast<std::size_t>(max_index) == total_keys;

            if (array_like) {
                nlohmann::json arr = nlohmann::json::array();
                for (lua_Integer i = 1; i <= max_index; ++i) {
                    lua_rawgeti(L, abs_index, i);
                    arr.push_back(LuaValueToJson(L, -1, depth + 1, error));
                    lua_pop(L, 1);
                    if (!error.empty()) {
                        return nullptr;
                    }
                }
                return arr;
            }

            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, abs_index) != 0) {
                std::string key;
                bool key_ok = true;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    std::size_t len = 0;
                    const char* s = lua_tolstring(L, -2, &len);
                    key.assign(s, len);
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    // 数字键:不能直接 lua_tostring(会把键原地改成字符串,
                    // 弄乱 lua_next 的遍历),自己格式化。
                    if (lua_isinteger(L, -2) != 0) {
                        key = std::to_string(static_cast<std::int64_t>(lua_tointeger(L, -2)));
                    } else {
                        key = std::to_string(static_cast<double>(lua_tonumber(L, -2)));
                    }
                } else {
                    key_ok = false;  // 函数/表当键,JSON 表达不了,跳过
                }
                if (key_ok) {
                    obj[key] = LuaValueToJson(L, -1, depth + 1, error);
                }
                lua_pop(L, 1);
                if (!error.empty()) {
                    return nullptr;
                }
            }
            return obj;
        }
        default:
            error = std::string("没法转成 JSON 的 lua 类型: ") + lua_typename(L, lua_type(L, abs_index));
            return nullptr;
    }
}

// 读取表字段成字符串(栈顶是那张表)。missing_ok 时字段缺失返回空串。
std::expected<std::string, std::string> GetStringField(lua_State* L, const char* field, bool missing_ok) {
    lua_getfield(L, -1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (missing_ok) {
            return std::string();
        }
        return std::unexpected(std::string("表里缺 ") + field + " 字段");
    }
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        return std::unexpected(std::string(field) + " 字段不是字符串");
    }
    std::size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    std::string out(s, len);
    lua_pop(L, 1);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// LuaTool
// ---------------------------------------------------------------------------

LuaTool::~LuaTool() {
    if (lua_ != nullptr) {
        lua_close(lua_);  // execute_ref_ 跟着整个 state 一起没,不用单独 unref
        lua_ = nullptr;
    }
}

std::expected<std::unique_ptr<LuaTool>, std::string> LuaTool::LoadFromScript(
    const std::string& script, const std::string& stem, const LuaProfile& profile) {
    std::unique_ptr<LuaGuard> guard;
    lua_State* L = NewGuardedState(profile, guard);
    if (L == nullptr) {
        return std::unexpected("lua_newstate 失败(内存不够?)");
    }
    // 出错路径统一走这个收尾;成功路径把 L 移交给 LuaTool 后置空。
    const auto fail = [&L](std::string message) {
        lua_close(L);
        L = nullptr;
        return std::unexpected(std::move(message));
    };

    luaL_openlibs(L);
    if (profile.level == LuaProfile::Level::Pure) {
        ApplyPureProfile(L);
    }

    // 编译 + 执行整个 chunk,期望返回一张表。
    if (luaL_loadbuffer(L, script.data(), script.size(), stem.c_str()) != LUA_OK) {
        std::string message = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "(没有错误信息)";
        return fail("脚本编译失败: " + message);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string message = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "(没有错误信息)";
        return fail("脚本执行失败: " + message);
    }
    if (!lua_istable(L, -1)) {
        return fail("脚本的返回值不是表(要 return { name=..., execute=... } 这样一张表)");
    }

    auto name_result = GetStringField(L, "name", /*missing_ok=*/false);
    if (!name_result.has_value()) {
        return fail(name_result.error());
    }
    if (name_result->empty()) {
        return fail("name 字段是空串");
    }
    auto description_result = GetStringField(L, "description", /*missing_ok=*/true);
    if (!description_result.has_value()) {
        return fail(description_result.error());
    }

    // input_schema:JSON 字符串或 lua 表都认;没写就给一张最宽的对象 schema。
    nlohmann::json schema = nlohmann::json{{"type", "object"}};
    lua_getfield(L, -1, "input_schema");
    if (lua_type(L, -1) == LUA_TSTRING) {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        nlohmann::json parsed = nlohmann::json::parse(std::string_view(s, len), /*cb=*/nullptr,
                                                       /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            lua_pop(L, 1);
            return fail("input_schema 不是合法 JSON");
        }
        schema = std::move(parsed);
    } else if (lua_istable(L, -1)) {
        std::string conv_error;
        schema = LuaValueToJson(L, -1, 0, conv_error);
        if (!conv_error.empty()) {
            lua_pop(L, 1);
            return fail("input_schema 表转 JSON 失败: " + conv_error);
        }
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fail("input_schema 字段要么是 JSON 字符串要么是表");
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "execute");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return fail("execute 字段不是函数");
    }
    const int execute_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // 顺手把函数从栈上收进注册表
    lua_pop(L, 1);                                            // 弹掉那张表,栈清空

    auto tool = std::unique_ptr<LuaTool>(new LuaTool());
    tool->lua_ = L;
    tool->execute_ref_ = execute_ref;
    tool->stem_ = stem;
    tool->full_name_ = "plugin__" + stem + "__" + *name_result;
    tool->description_ = "[plugin:" + stem + "] " + *description_result;
    tool->schema_ = std::move(schema);
    tool->profile_ = profile;
    tool->guard_ = std::move(guard);
    L = nullptr;  // 所有权移交,fail 那条路走不到了
    return tool;
}

std::expected<std::unique_ptr<LuaTool>, std::string> LuaTool::LoadFromFile(
    const std::filesystem::path& file, const LuaProfile& profile) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected("读不到文件 " + PathToUtf8(file));
    }
    const std::string script((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return LoadFromScript(script, PathToUtf8(file.stem()), profile);
}

std::string LuaTool::name() const { return full_name_; }

std::string LuaTool::description() const { return description_; }

nlohmann::json LuaTool::input_schema() const { return schema_; }

Tool::Result LuaTool::execute(const nlohmann::json& input) {
    // ToolRuntime 的 sub registry 会被多只后台子代理共享。Lua state 不具备
    // 线程安全语义,同一工具的栈操作须串行；不同 LuaTool 各有 state 和锁,
    // 仍能彼此并行。
    const std::lock_guard<std::mutex> lock(execute_mutex_);
    // 每次执行换一轮账:指令计数清零(预算是单次调用的),取消旗灌进
    // guard(hook 查的就是它)。内存账不清——state 是长命的,累计口径
    // 才是真实的占用。
    if (guard_ != nullptr) {
        guard_->instructions_used = 0;
        guard_->budget_hit = false;
        guard_->cancel = cancel_;
    }
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, execute_ref_);
    PushJsonToLua(lua_, input);
    if (lua_pcall(lua_, 1, 1, 0) != LUA_OK) {
        std::string message =
            lua_tostring(lua_, -1) != nullptr ? lua_tostring(lua_, -1) : "(没有错误信息)";
        lua_pop(lua_, 1);
        // 逐枚追踪单:compile/load 与 runtime error 分开(单子"Lua
        // compile/load/runtime error 分开"——load 段在 LoadFromFile,这里
        // 是 runtime 段),稳定码不靠中文正文分辨。
        Result lua_error{"lua 执行出错: " + message, true};
        lua_error.outcome = "plugin_exception";
        lua_error.error_code = "plugin.lua_error";
        return lua_error;
    }

    // 返回值字符串化:字符串原样收,数字/布尔转文本,表转 JSON,nil 算错
    // (execute 忘了 return,多半是插件写岔了,明说比静默空串好排查)。
    Result out;
    switch (lua_type(lua_, -1)) {
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(lua_, -1, &len);
            out.content.assign(s, len);
            break;
        }
        case LUA_TNUMBER:
            if (lua_isinteger(lua_, -1) != 0) {
                out.content = std::to_string(static_cast<std::int64_t>(lua_tointeger(lua_, -1)));
            } else {
                out.content = std::to_string(static_cast<double>(lua_tonumber(lua_, -1)));
            }
            break;
        case LUA_TBOOLEAN:
            out.content = lua_toboolean(lua_, -1) != 0 ? "true" : "false";
            break;
        case LUA_TTABLE: {
            std::string conv_error;
            const nlohmann::json converted = LuaValueToJson(lua_, -1, 0, conv_error);
            if (!conv_error.empty()) {
                out.content = "lua 返回的表转不成 JSON: " + conv_error;
                out.is_error = true;
            } else {
                out.content = converted.dump();
            }
            break;
        }
        case LUA_TNIL:
            out.content = "lua 的 execute 没有返回值(要 return 一个字符串)";
            out.is_error = true;
            break;
        default:
            out.content = std::string("lua 的 execute 返回了没法字符串化的类型: ") +
                          lua_typename(lua_, lua_type(lua_, -1));
            out.is_error = true;
            break;
    }
    lua_pop(lua_, 1);
    return out;
}

// ---------------------------------------------------------------------------
// 目录扫描
// ---------------------------------------------------------------------------

LuaScanResult LoadLuaPlugins(const std::filesystem::path& dir, const LuaProfile& profile) {
    LuaScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return result;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = lubancode::tools::PathToUtf8(entry.path().extension());
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".lua") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // 顺序稳定,好测试、好复现

    for (const auto& file : files) {
        auto loaded = LuaTool::LoadFromFile(file, profile);
        if (!loaded.has_value()) {
            result.warnings.push_back("[plugin] " + PathToUtf8(file.filename()) + ": " +
                                      loaded.error() + ",跳过");
            continue;
        }
        result.tools.push_back(std::move(*loaded));
    }
    return result;
}

LuaProfile LuaProfile::PureDefault() { return PureDefaultImpl(); }
LuaProfile LuaProfile::TrustedDefault() { return TrustedDefaultImpl(); }

}  // namespace lubancode::tools
