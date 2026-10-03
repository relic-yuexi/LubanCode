#include "tools/lua_tool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
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

// 每枚 state 一份的运行时账:定义已搬进 .hpp(阶段 3 起 runtime 侧共用)。

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

// hook 中间件 state 的缺省画像(LuaHook 单 P0-A):白名单库 + 三道墙
// (§六的 500ms 是待验证建议值,不是性能结论)。
LuaProfile HookDefaultImpl() {
    LuaProfile profile;
    profile.level = LuaProfile::Level::Whitelisted;
    profile.instruction_budget = 200'000'000;
    profile.memory_cap_bytes = 256 * 1024 * 1024;
    profile.wall_budget = std::chrono::milliseconds(500);
    return profile;
}

// 底下的原生 allocator(realloc/free):lua_Alloc 契约:NULL 释放,osize
// 只在 ptr!=NULL 时才有意义。包一层记账,超帽返回 NULL——Lua 把 NULL
// 当 OOM 走 luaL_error,宿主堆不破。
void* GuardedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* guard = static_cast<LuaGuard*>(ud);
    if (nsize == 0) {
        if (ptr != nullptr && guard->memory_used >= osize) {
            guard->memory_used -= osize;
        }
        std::free(ptr);
        return nullptr;
    }
    const std::size_t old_size = ptr != nullptr ? osize : 0;
    const std::size_t retained = guard->memory_used >= old_size ? guard->memory_used - old_size : 0;
    if (guard->memory_cap > 0 &&
        (retained > guard->memory_cap || nsize > guard->memory_cap - retained)) {
        // 内存帽落锤:记账分型(预算错误的稳定码映射用)。
        guard->budget_hit = true;
        guard->last_budget_kind = LuaGuard::BudgetKind::Memory;
        return nullptr;  // Lua 把 NULL 当 OOM,luaL_error 走脚本错误,宿主不倒
    }
    void* next = std::realloc(ptr, nsize);
    if (next != nullptr) {
        // realloc 成功才记账;失败按没动算(Lua 会走 OOM)。
        guard->memory_used = retained + nsize;
    }
    return next;
}

// instruction hook(LUA_MASKCOUNT):数指令、查取消、查预算、查墙钟。luaL_error
// 是 longjmp 路子,pcall 接得住——宿主栈不破。
// allocator 已拥有这枚 guard。直接借其 userdata，不在低内存 hook 或
// state 初始化阶段分配 registry 键。

void GuardHook(lua_State* L, lua_Debug*) {
    void* allocator_data = nullptr;
    lua_getallocf(L, &allocator_data);
    auto* guard = static_cast<LuaGuard*>(allocator_data);
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
        guard->last_budget_kind = LuaGuard::BudgetKind::Instruction;
        // 不把临时 std::string 带过 luaL_error 的 longjmp 边界。
        luaL_error(L, "cpu 指令预算耗尽:改小输入或拆小任务");
        return;
    }
    // 墙钟(LuaHook 单 P0-A 第四道墙):只管本 state 跑野的脚本;阻塞
    // Host API 的等待不在这拦(各 Host API 自己接 deadline/cancel)。
    if (guard->wall_deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= guard->wall_deadline) {
        guard->budget_hit = true;
        guard->last_budget_kind = LuaGuard::BudgetKind::WallClock;
        luaL_error(L, "lua 墙钟预算耗尽(handler 自用时间到点):拆小任务或调大限额");
    }
}

}  // namespace

// 造一枚带三道墙(allocator 内存帽/hook 指令预算+墙钟/取消链)的 state。
// 预算与帽由 profile 定;取消旗是每轮执行期才灌的,存 guard 里由 hook 查。
// guard 的寿命:LuaTool 持 unique_ptr,state close 之后回调面消失,安全。
// (阶段 3 起 runtime 侧的 Lua Host API 走同一枚构造——墙只此一份。)
lua_State* NewGuardedLuaState(const LuaProfile& profile, std::unique_ptr<LuaGuard>& guard_out) {
    auto guard = std::make_unique<LuaGuard>();
    guard->instruction_budget = profile.instruction_budget;
    guard->memory_cap = profile.memory_cap_bytes;
    lua_State* L = lua_newstate(GuardedAlloc, guard.get());
    if (L == nullptr) {
        return nullptr;
    }
    // 墙钟也是指令 hook 查的:任一道时间墙立着,hook 就得挂。
    if (profile.instruction_budget > 0 || profile.wall_budget > std::chrono::milliseconds(0)) {
        lua_sethook(L, GuardHook, LUA_MASKCOUNT, kHookStride);
    }
    guard_out = std::move(guard);
    return L;
}

// pure 画像关门:os.execute / os.exit / io / package.loadlib 拿掉。
// luaL_openlibs 开全之后做减法,比挨个 luaopen 少踩内部初始化的坑
// (io 的 __gc 元方法在关 io 表后照旧有效,文件柄照收)。
void ApplyPureLuaProfile(lua_State* L) {
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

void ApplyWhitelistLuaProfile(lua_State* L) {
    // §五"Lua 库采用白名单":不从"开全库再减法"出发——那会漏(os.remove/
    // rename、模块搜索器、coroutine 预算绕过都是现成教训)。从零只开:
    //   base(去 dofile/loadfile) + string + table + math + utf8
    //   os 只留时间四函数(clock/date/time/difftime)
    // 不开:io、package(连带 require 与模块搜索器)、coroutine(新建线程
    // 不继承指令 hook,预算可被绕)、debug、loadfile/dofile(文件口)。
    // luaL_requiref 走 registry 的 LOADED 表,不依赖 package 全局。
    luaL_requiref(L, LUA_GNAME, luaopen_base, 0);
    lua_pop(L, 1);
    // base 里的文件口:显式摘掉(load 保留——只编译字符串,不吃盘)。
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    // os:开真表(glb=0),拷时间四函数到新表再挂全局——真表只留在
    // LOADED 里,require 没开,脚本摸不回去。
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 0);
    lua_createtable(L, 0, 4);
    for (const char* name : {"clock", "date", "time", "difftime"}) {
        lua_getfield(L, -2, name);
        if (lua_isfunction(L, -1) != 0) {
            lua_setfield(L, -2, name);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);  // 真 os 表
}

// 按画像开库的唯一入口:装载路径(LuaTool/LuaHostState)不再各自抄一份
// luaL_openlibs(抄出来的第二份迟早和白名单漂移)。
void OpenLuaLibraries(lua_State* L, const LuaProfile& profile) {
    if (profile.level == LuaProfile::Level::Whitelisted) {
        ApplyWhitelistLuaProfile(L);
    } else {
        luaL_openlibs(L);
        if (profile.level == LuaProfile::Level::Pure) ApplyPureLuaProfile(L);
    }
    if (!profile.allow_print) { lua_pushnil(L); lua_setglobal(L, "print"); }
}

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

namespace {

// 所有 C++ 容器在 lua_pcall 外持有。回调只读借值指令表，Lua OOM
// longjmp 不会跳过 JSON iterator proxy、vector 或 string 的析构。
struct JsonPushStep {
    enum class Kind { Null, Boolean, Integer, Unsigned, Number, String, Array, Object, ArraySet, ObjectSet };
    Kind kind;
    const std::string* text = nullptr;
    std::int64_t integer = 0;
    std::uint64_t unsigned_integer = 0;
    double number = 0;
    int count = 0;
};
struct JsonPushPlan {
    std::vector<JsonPushStep> steps;
    int stack_slots = 8;
    void push_back(JsonPushStep step) { steps.push_back(step); }
};

void PrepareJsonPush(const nlohmann::json& value, JsonPushPlan& plan, int depth = 0) {
    if (depth > ((std::numeric_limits<int>::max)() - 8) / 3)
        throw std::runtime_error("JSON 入参超出 Lua 栈容量");
    plan.stack_slots = (std::max)(plan.stack_slots, depth * 3 + 8);
    using Kind = JsonPushStep::Kind;
    switch (value.type()) {
        case nlohmann::json::value_t::null: plan.push_back({Kind::Null}); break;
        case nlohmann::json::value_t::boolean:
            plan.push_back({Kind::Boolean, nullptr, value.get<bool>() ? 1 : 0}); break;
        case nlohmann::json::value_t::number_integer:
            plan.push_back({Kind::Integer, nullptr, value.get<std::int64_t>()}); break;
        case nlohmann::json::value_t::number_unsigned: {
            JsonPushStep step{Kind::Unsigned}; step.unsigned_integer = value.get<std::uint64_t>();
            plan.push_back(step); break;
        }
        case nlohmann::json::value_t::number_float: {
            JsonPushStep step{Kind::Number}; step.number = value.get<double>(); plan.push_back(step); break;
        }
        case nlohmann::json::value_t::string:
            plan.push_back({Kind::String, &value.get_ref<const std::string&>()}); break;
        case nlohmann::json::value_t::array: {
            if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                throw std::runtime_error("JSON 数组超出 Lua 容量");
            JsonPushStep step{Kind::Array}; step.count = static_cast<int>(value.size()); plan.push_back(step);
            std::int64_t index = 1;
            for (const auto& element : value) {
                PrepareJsonPush(element, plan, depth + 1);
                plan.push_back({Kind::ArraySet, nullptr, index++});
            }
            break;
        }
        case nlohmann::json::value_t::object: {
            if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                throw std::runtime_error("JSON 对象超出 Lua 容量");
            JsonPushStep step{Kind::Object}; step.count = static_cast<int>(value.size()); plan.push_back(step);
            for (auto item = value.begin(); item != value.end(); ++item) {
                plan.push_back({Kind::String, &item.key()});
                PrepareJsonPush(item.value(), plan, depth + 1);
                plan.push_back({Kind::ObjectSet});
            }
            break;
        }
        default: plan.push_back({Kind::Null}); break;
    }
}

// 此帧只有 POD/引用。逐层栈空间也在受保护边界内申请。
void PushPreparedJson(lua_State* L, const JsonPushPlan& plan) {
    if (lua_checkstack(L, plan.stack_slots) == 0) luaL_error(L, "JSON 入参 Lua 栈不足");
    for (std::size_t index = 0; index < plan.steps.size(); ++index) {
        const JsonPushStep& step = plan.steps[index];
        using Kind = JsonPushStep::Kind;
        switch (step.kind) {
            case Kind::Null: lua_pushnil(L); break;
            case Kind::Boolean: lua_pushboolean(L, step.integer != 0); break;
            case Kind::Integer: lua_pushinteger(L, static_cast<lua_Integer>(step.integer)); break;
            case Kind::Unsigned: lua_pushinteger(L, static_cast<lua_Integer>(step.unsigned_integer)); break;
            case Kind::Number: lua_pushnumber(L, static_cast<lua_Number>(step.number)); break;
            case Kind::String: lua_pushlstring(L, step.text->data(), step.text->size()); break;
            case Kind::Array: lua_createtable(L, step.count, 0); break;
            case Kind::Object: lua_createtable(L, 0, step.count); break;
            case Kind::ArraySet: lua_rawseti(L, -2, static_cast<lua_Integer>(step.integer)); break;
            case Kind::ObjectSet: lua_rawset(L, -3); break;
        }
    }
}

int PushJsonCallback(lua_State* L) {
    const auto* plan = static_cast<const JsonPushPlan*>(lua_touserdata(L, 1));
    PushPreparedJson(L, *plan);
    return 1;
}

}  // namespace

// 原共享入口保留相同栈效果。转换表在受保护调用外销毁，之后才向已有
// Lua 调用链抛回错误；standalone Run 把搬运和 execute 一并放在其外层 pcall。
void PushJsonToLua(lua_State* L, const nlohmann::json& value) {
    int status = LUA_OK;
    {
        JsonPushPlan plan;
        PrepareJsonPush(value, plan);
        lua_pushcfunction(L, PushJsonCallback);
        lua_pushlightuserdata(L, &plan);
        status = lua_pcall(L, 1, 1, 0);
    }
    if (status != LUA_OK) lua_error(L);
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

namespace {

// 这里的 owner 均在 Lua 受保护边界外。C callback 只借 POD/源码与画像。
struct LuaStateCloser { void operator()(lua_State* state) const { if (state) lua_close(state); } };
using OwnedLuaState = std::unique_ptr<lua_State, LuaStateCloser>;

class LuaCallScope {
public:
    LuaCallScope(lua_State* state, LuaGuard* guard, const LuaProfile& profile,
                 const std::atomic<bool>* cancel = nullptr)
        : state_(state), guard_(guard), top_(lua_gettop(state)) {
        if (guard_) {
            guard_->instructions_used = 0;
            guard_->budget_hit = false;
            guard_->last_budget_kind = LuaGuard::BudgetKind::None;
            guard_->cancel = cancel;
            guard_->wall_deadline = profile.wall_budget > std::chrono::milliseconds(0)
                ? std::chrono::steady_clock::now() + profile.wall_budget
                : std::chrono::steady_clock::time_point{};
        }
    }
    ~LuaCallScope() {
        if (guard_) { guard_->cancel = nullptr; guard_->wall_deadline = {}; }
        lua_settop(state_, top_);
    }
    LuaCallScope(const LuaCallScope&) = delete;
    LuaCallScope& operator=(const LuaCallScope&) = delete;
private:
    lua_State* state_;
    LuaGuard* guard_;
    int top_;
};

std::string LuaErrorText(lua_State* state) {
    // lua_tolstring 对 number 会分配，错误口只读已存在的 string。
    if (lua_type(state, -1) != LUA_TSTRING) return "(没有字符串错误信息)";
    std::size_t bytes = 0;
    const char* text = lua_tolstring(state, -1, &bytes);
    return text ? std::string(text, bytes) : "(没有错误信息)";
}

struct LuaLoadContext {
    const std::string* script;
    const std::string* stem;
    const LuaProfile* profile;
    const char* phase = "开库";
    int execute_ref = LUA_NOREF;
};

int LoadLuaToolCallback(lua_State* state) {
    auto* context = static_cast<LuaLoadContext*>(lua_touserdata(state, 1));
    OpenLuaLibraries(state, *context->profile);
    context->phase = "编译";
    if (luaL_loadbuffer(state, context->script->data(), context->script->size(),
                        context->stem->c_str()) != LUA_OK) return lua_error(state);
    context->phase = "执行";
    lua_call(state, 0, 1);
    if (!lua_istable(state, -1))
        return luaL_error(state, "脚本的返回值不是表(要 return { name=..., execute=... } 这样一张表)");
    context->phase = "定义";
    // getfield 可执行 __index；连同引用表分配都在本次 pcall/budget 内。
    lua_getfield(state, 2, "name");
    lua_getfield(state, 2, "description");
    lua_getfield(state, 2, "input_schema");
    lua_getfield(state, 2, "execute");
    if (!lua_isfunction(state, -1)) return luaL_error(state, "execute 字段不是函数");
    context->execute_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return 3;  // name / description / input_schema；不把定义表带出。
}

struct LuaExecuteContext { int execute_ref; const JsonPushPlan* input; };
int ExecuteLuaToolCallback(lua_State* state) {
    const auto* context = static_cast<const LuaExecuteContext*>(lua_touserdata(state, 1));
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->execute_ref);
    PushPreparedJson(state, *context->input);
    lua_call(state, 1, 1);
    return 1;
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
    OwnedLuaState state(NewGuardedLuaState(profile, guard));
    if (!state) return std::unexpected("lua_newstate 失败(内存不够?)");
    lua_State* L = state.get();
    LuaCallScope call_scope(L, guard.get(), profile);
    try {
        LuaLoadContext context{&script, &stem, &profile};
        lua_pushcfunction(L, LoadLuaToolCallback);
        lua_pushlightuserdata(L, &context);
        if (lua_pcall(L, 1, 3, 0) != LUA_OK)
            return std::unexpected(std::string("脚本") + context.phase + "失败: " + LuaErrorText(L));

        if (lua_type(L, 1) != LUA_TSTRING) return std::unexpected("name 字段不是字符串");
        std::size_t bytes = 0;
        const char* name_data = lua_tolstring(L, 1, &bytes);
        std::string name(name_data, bytes);
        if (name.empty()) return std::unexpected("name 字段是空串");
        std::string description;
        if (!lua_isnil(L, 2)) {
            if (lua_type(L, 2) != LUA_TSTRING) return std::unexpected("description 字段不是字符串");
            const char* description_data = lua_tolstring(L, 2, &bytes);
            description.assign(description_data, bytes);
        }
        // checkstack 只返回失败，不抛 Lua 错误。转换中的 rawget/next 与
        // 已为 string 的 tolstring 不分配 Lua 对象，也不执行 metamethod。
        if (lua_checkstack(L, kMaxDepth * 3 + 8) == 0)
            return std::unexpected("input_schema 转换 Lua 栈不足");
        nlohmann::json schema = nlohmann::json{{"type", "object"}};
        if (lua_type(L, 3) == LUA_TSTRING) {
            const char* schema_data = lua_tolstring(L, 3, &bytes);
            schema = nlohmann::json::parse(std::string_view(schema_data, bytes), nullptr, false);
            if (schema.is_discarded()) return std::unexpected("input_schema 不是合法 JSON");
        } else if (lua_istable(L, 3)) {
            std::string error;
            schema = LuaValueToJson(L, 3, 0, error);
            if (!error.empty()) return std::unexpected("input_schema 表转 JSON 失败: " + error);
        } else if (!lua_isnil(L, 3)) {
            return std::unexpected("input_schema 字段要么是 JSON 字符串要么是表");
        }

        auto tool = std::unique_ptr<LuaTool>(new LuaTool());
        tool->execute_ref_ = context.execute_ref;
        tool->stem_ = stem;
        tool->full_name_ = "plugin__" + stem + "__" + name;
        tool->description_ = "[plugin:" + stem + "] " + description;
        tool->schema_ = std::move(schema);
        tool->profile_ = profile;
        // 先做完可能抛错的 C++ 装配，再移交 VM/allocator owner。
        tool->guard_ = std::move(guard);
        tool->lua_ = state.release();
        return tool;
    } catch (const std::exception& error) {
        return std::unexpected(std::string("lua 工具装载失败: ") + error.what());
    }
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
    return Run(input, cancel_);
}

Tool::Result LuaTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    // context 的取消旗优先(本次调用那根:主回合 ESC / 子代理 CancelChain
    // 合并旗);没递进来退回 SetCancel 灌的。
    return Run(input, context.cancel != nullptr ? context.cancel : cancel_);
}

Tool::Result LuaTool::Run(const nlohmann::json& input, const std::atomic<bool>* effective_cancel) {
    // ToolRuntime 的 sub registry 会被多只后台子代理共享。Lua state 不具备
    // 线程安全语义,同一工具的栈操作须串行；不同 LuaTool 各有 state 和锁,
    // 仍能彼此并行。
    if (effective_cancel && effective_cancel->load()) {
        Result cancelled{"用户取消(ESC):lua 脚本未启动", true};
        cancelled.outcome = "plugin_exception";
        cancelled.error_code = "plugin.lua_error";
        return cancelled;
    }
    const std::lock_guard<std::mutex> lock(execute_mutex_);
    if (effective_cancel && effective_cancel->load()) {
        Result cancelled{"用户取消(ESC):lua 脚本未启动", true};
        cancelled.outcome = "plugin_exception";
        cancelled.error_code = "plugin.lua_error";
        return cancelled;
    }
    LuaCallScope call_scope(lua_, guard_.get(), profile_, effective_cancel);
    try {
        JsonPushPlan input_plan;
        PrepareJsonPush(input, input_plan);
        LuaExecuteContext context{execute_ref_, &input_plan};
        lua_pushcfunction(lua_, ExecuteLuaToolCallback);
        lua_pushlightuserdata(lua_, &context);
        if (lua_pcall(lua_, 1, 1, 0) != LUA_OK) {
            Result lua_error{"lua 执行出错: " + LuaErrorText(lua_), true};
            lua_error.outcome = "plugin_exception";
            lua_error.error_code = "plugin.lua_error";
            return lua_error;
        }
        if (lua_checkstack(lua_, kMaxDepth * 3 + 8) == 0) {
            Result lua_error{"lua 返回值转换栈不足", true};
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
                out.SetText(std::string(s, len));
                break;
            }
            case LUA_TNUMBER:
                if (lua_isinteger(lua_, -1) != 0) {
                    out.SetText(std::to_string(static_cast<std::int64_t>(lua_tointeger(lua_, -1))));
                } else {
                    out.SetText(std::to_string(static_cast<double>(lua_tonumber(lua_, -1))));
                }
                break;
            case LUA_TBOOLEAN:
                out.SetText(lua_toboolean(lua_, -1) != 0 ? "true" : "false");
                break;
            case LUA_TTABLE: {
                std::string conv_error;
                const nlohmann::json converted = LuaValueToJson(lua_, -1, 0, conv_error);
                if (!conv_error.empty()) {
                    out.SetText("lua 返回的表转不成 JSON: " + conv_error);
                    out.is_error = true;
                } else {
                    out.SetText(converted.dump());
                }
                break;
            }
            case LUA_TNIL:
                out.SetText("lua 的 execute 没有返回值(要 return 一个字符串)");
                out.is_error = true;
                break;
            default:
                out.SetText(std::string("lua 的 execute 返回了没法字符串化的类型: ") +
                            lua_typename(lua_, lua_type(lua_, -1)));
                out.is_error = true;
                break;
        }
        return out;
    } catch (const std::exception& error) {
        Result lua_error{std::string("lua 执行出错: ") + error.what(), true};
        lua_error.outcome = "plugin_exception";
        lua_error.error_code = "plugin.lua_error";
        return lua_error;
    }
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
LuaProfile LuaProfile::HookDefault() { return HookDefaultImpl(); }

}  // namespace lubancode::tools
