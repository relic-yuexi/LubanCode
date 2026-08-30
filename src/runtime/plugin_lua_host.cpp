// Lua Host API 与动态调用上下文的实现(阶段 3)。§六的模块形状、§九的
// 六步时序、§11 的错误合同都在这里落地;HTTP/Secret 的真活全在阶段 0-2
// 的件里(ExecutePluginHttp/SecretResolver/transport seam),本文件只做
// Lua 边界的形状转换与分派——所以这里也没有第二份网络执法,越权、超帽、
// DNS 安全都还是 transport 的账。
#include "runtime/plugin_lua_host.hpp"

#include <utility>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace lubancode::runtime {

namespace {

// registry 键:动态调用上下文(light userdata 或 nil)。与 lua_tool 的
// guard 键同一风格——lua_sethook/C 函数都缺 ud 通道,registry 是唯一口。
const char* kCallContextRegistryKey = "lubancode.lua.callcontext";

// SecretRef 的 metatable 名(registry 键 + __name + 锁表串,三用)。
const char* kSecretRefMetatableName = "luban.secret_ref";

// ---------------------------------------------------------------------------
// 栈上小件
// ---------------------------------------------------------------------------

// 压 err 表(§6.2 失败形状):{ code, message, status, retryable }。
void PushErrorTable(lua_State* L, LuaHostErrorCode code, std::string message, int status, bool retryable) {
    const std::string_view name = LuaHostErrorCodeName(code);
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, name.data(), name.size());
    lua_setfield(L, -2, "code");
    lua_pushlstring(L, message.data(), message.size());
    lua_setfield(L, -2, "message");
    lua_pushinteger(L, static_cast<lua_Integer>(status));
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, retryable ? 1 : 0);
    lua_setfield(L, -2, "retryable");
}

void PushErrorTable(lua_State* L, const PluginHttpCallError& error) {
    PushErrorTable(L, error.code, error.message, error.status, error.retryable);
}

// no_active_tool_call 的统一落法(§九第 3 步):nil + err,函数返回 2。
int PushNoActiveToolCall(lua_State* L) {
    lua_pushnil(L);
    PushErrorTable(L, LuaHostErrorCode::NoActiveToolCall,
                   std::string(LuaHostErrorCodeDefaultMessage(LuaHostErrorCode::NoActiveToolCall)), 0, false);
    return 2;
}

// 栈上字符串读成 std::string(不改栈)。
std::string StackString(lua_State* L, int index) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, index, &len);
    return std::string(s != nullptr ? s : "", len);
}

// ---------------------------------------------------------------------------
// SecretRef(§6.3):opaque userdata。负载是零字节的壳 + 一枚 uservalue
// (逻辑 id 的 lua 串)——id 本来就是 manifest 里的公开名字,不是 Secret
// 材料;真正的值宿主侧 SecretValue RAII 管,Lua 到不了。
// 元方法锁死清单:
//   __tostring -> "<secret:<id>>"(只有 id,永远没有值)
//   __name     -> "luban.secret_ref"(报错文案用)
//   __metatable-> 锁表(getmetatable 只得串;setmetatable 拒)
//   __index/__concat/__pairs 一概不设:索引、拼接、遍历全走 Lua 原生报错
//   __eq 不设:userdata 按同一性比较(比 ref 不比值,比不出原文)
//   转 JSON:LuaValueToJson 对 userdata 报"没法转成 JSON 的 lua 类型"
// ---------------------------------------------------------------------------

int LuaSecretRefToString(lua_State* L) {
    lua_getiuservalue(L, 1, 1);
    const std::string id = StackString(L, -1);
    lua_pop(L, 1);
    const std::string text = "<secret:" + id + ">";
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

void EnsureSecretRefMetatable(lua_State* L) {
    if (luaL_newmetatable(L, kSecretRefMetatableName) != 0) {
        // 新表:__tostring 给形状,__metatable 锁死;__name 由
        // luaL_newmetatable 置好。不设 __index/__concat——缺省即拒。
        lua_pushcfunction(L, LuaSecretRefToString);
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, kSecretRefMetatableName);
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
}

// 造一枚 SecretRef(栈顶留下它)。
void PushSecretRef(lua_State* L, std::string_view id) {
    EnsureSecretRefMetatable(L);
    lua_newuserdatauv(L, 0, 1);
    lua_pushlstring(L, id.data(), id.size());
    lua_setiuservalue(L, -2, 1);
    luaL_getmetatable(L, kSecretRefMetatableName);
    lua_setmetatable(L, -2);
}

// 栈上值是不是 SecretRef(metatable 同一性认定,防拿别家 userdata 冒充)。
bool IsSecretRefValue(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA) {
        return false;
    }
    if (lua_getmetatable(L, index) == 0) {
        return false;
    }
    luaL_getmetatable(L, kSecretRefMetatableName);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

// 从 SecretRef 读回逻辑 id(uservalue 里的串)。
std::string SecretRefId(lua_State* L, int index) {
    lua_getiuservalue(L, index, 1);
    const std::string id = StackString(L, -1);
    lua_pop(L, 1);
    return id;
}

// ---------------------------------------------------------------------------
// luban.http.request(§6.2)
// ---------------------------------------------------------------------------

// 请求表解析的失败形状(转成 err 表的路数)。
struct ParseFailure {
    LuaHostErrorCode code = LuaHostErrorCode::InvalidRequest;
    std::string message;
};

// 读一枚必填/可选字符串字段。index 是请求表的位置。
std::expected<std::string, ParseFailure> StringField(lua_State* L, int index, const char* field, bool required) {
    lua_getfield(L, index, field);
    const int type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        if (required) {
            return std::unexpected(ParseFailure{LuaHostErrorCode::InvalidRequest,
                                                std::string("请求表缺 ") + field + " 字段"});
        }
        return std::string();
    }
    if (type != LUA_TSTRING) {
        lua_pop(L, 1);
        return std::unexpected(
            ParseFailure{LuaHostErrorCode::InvalidRequest, std::string(field) + " 字段不是字符串"});
    }
    const std::string value = StackString(L, -1);
    lua_pop(L, 1);
    return value;
}

// method 规范成大写(manifest methods 已是大写;小写写法是作者手滑,不折
// 成错误——§5.3 的"大小写规范化"同一路数)。
std::string AsciiUppered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    return out;
}

std::expected<PluginHttpApiRequest, ParseFailure> ParseHttpRequestTable(lua_State* L) {
    PluginHttpApiRequest request;

    auto method = StringField(L, 1, "method", /*required=*/true);
    if (!method.has_value()) {
        return std::unexpected(method.error());
    }
    request.method = AsciiUppered(*method);
    if (request.method.empty()) {
        return std::unexpected(ParseFailure{LuaHostErrorCode::InvalidRequest, "method 字段是空串"});
    }

    auto url = StringField(L, 1, "url", /*required=*/true);
    if (!url.has_value()) {
        return std::unexpected(url.error());
    }
    request.url = std::move(*url);

    // headers:string -> string(键值都只收字符串;乱序收进有序表,Lua 表
    // 本身无序,顺序由宿主表决定)。
    lua_getfield(L, 1, "headers");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected(ParseFailure{LuaHostErrorCode::InvalidRequest, "headers 字段不是表"});
        }
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 2);  // 弹值与键(lua_next 要留键,这里整件退场)
                return std::unexpected(ParseFailure{LuaHostErrorCode::InvalidRequest,
                                                    "headers 的键与值都须是字符串"});
            }
            request.headers.emplace_back(StackString(L, -2), StackString(L, -1));
            lua_pop(L, 1);  // 弹值留键
        }
    }
    lua_pop(L, 1);

    // json:与 body 二选一(两填由 ExecutePluginHttp 拒,这里只记账)。
    lua_getfield(L, 1, "json");
    if (!lua_isnil(L, -1)) {
        std::string convert_error;
        request.json = tools::LuaValueToJson(L, -1, 0, convert_error);
        lua_pop(L, 1);
        if (!convert_error.empty()) {
            return std::unexpected(
                ParseFailure{LuaHostErrorCode::InvalidRequest, "json 字段转 JSON 失败: " + convert_error});
        }
        request.has_json = true;
    } else {
        lua_pop(L, 1);
    }

    auto body = StringField(L, 1, "body", /*required=*/false);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    if (!body->empty()) {
        request.body = std::move(*body);
        request.has_body = true;
    }

    // auth:{ type, secret, optional, name, prefix }。secret 收逻辑 id 字符串
    // (语法糖)或 SecretRef userdata——两条进路在门口都折成 id,后面是同
    // 一条注入链(§6.3)。
    lua_getfield(L, 1, "auth");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected(ParseFailure{LuaHostErrorCode::InvalidRequest, "auth 字段不是表"});
        }
        auto type = StringField(L, -1, "type", /*required=*/true);
        if (!type.has_value()) {
            lua_pop(L, 1);
            return std::unexpected(type.error());
        }
        request.auth.type = std::move(*type);
        request.has_auth = true;

        lua_getfield(L, -1, "secret");
        if (lua_type(L, -1) == LUA_TSTRING) {
            request.auth.secret_id = StackString(L, -1);
            lua_pop(L, 1);
        } else if (IsSecretRefValue(L, -1)) {
            request.auth.secret_id = SecretRefId(L, -1);
            lua_pop(L, 1);
        } else if (!lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pop(L, 1);  // auth 表
            return std::unexpected(
                ParseFailure{LuaHostErrorCode::InvalidRequest, "auth.secret 须是逻辑 id 或 SecretRef"});
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, -1, "optional");
        request.auth.optional = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        auto name = StringField(L, -1, "name", /*required=*/false);
        if (!name.has_value()) {
            lua_pop(L, 1);
            return std::unexpected(name.error());
        }
        request.auth.name = std::move(*name);
        auto prefix = StringField(L, -1, "prefix", /*required=*/false);
        if (!prefix.has_value()) {
            lua_pop(L, 1);
            return std::unexpected(prefix.error());
        }
        request.auth.prefix = std::move(*prefix);
    }
    lua_pop(L, 1);  // auth 表

    // timeout_ms:整数;>0 只降不升(ExecutePluginHttp 取小)。
    lua_getfield(L, 1, "timeout_ms");
    if (!lua_isnil(L, -1)) {
        if (lua_isinteger(L, -1) == 0) {
            lua_pop(L, 1);
            return std::unexpected(
                ParseFailure{LuaHostErrorCode::InvalidRequest, "timeout_ms 字段不是整数"});
        }
        request.timeout_ms = static_cast<std::int64_t>(lua_tointeger(L, -1));
    }
    lua_pop(L, 1);

    return request;
}

// 成功响应压成 Lua 表(§6.2):{ status, headers, body, json?, url, bytes }。
// headers 键已小写;重复头按明确数组形状保留——首枚是串,重复变数组追加
// (§6.2 "不能悄悄拼错")。
void PushResponseTable(lua_State* L, const PluginHttpApiResponse& response) {
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, static_cast<lua_Integer>(response.status));
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, response.body.data(), response.body.size());
    lua_setfield(L, -2, "body");
    lua_pushlstring(L, response.url.data(), response.url.size());
    lua_setfield(L, -2, "url");
    lua_pushinteger(L, static_cast<lua_Integer>(response.bytes));
    lua_setfield(L, -2, "bytes");

    lua_createtable(L, 0, static_cast<int>(response.headers.size()));
    for (const auto& [name, value] : response.headers) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawget(L, -2);  // 旧值(可能 nil/串/数组)
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushlstring(L, name.data(), name.size());
            lua_pushlstring(L, value.data(), value.size());
            lua_rawset(L, -3);
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string previous = StackString(L, -1);
            lua_pop(L, 1);
            lua_pushlstring(L, name.data(), name.size());
            lua_createtable(L, 2, 0);
            lua_pushlstring(L, previous.data(), previous.size());
            lua_rawseti(L, -2, 1);
            lua_pushlstring(L, value.data(), value.size());
            lua_rawseti(L, -2, 2);
            lua_rawset(L, -3);
        } else {
            // 已是数组:追加。
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
            lua_pushlstring(L, value.data(), value.size());
            lua_rawseti(L, -2, count + 1);
            lua_pop(L, 1);
        }
    }
    lua_setfield(L, -2, "headers");

    if (response.json_parsed) {
        tools::PushJsonToLua(L, response.json);
        lua_setfield(L, -2, "json");
    }
}

int LuaHttpRequest(lua_State* L) {
    // C 函数不许让 C++ 异常穿 Lua 边界(resolver/transport 是宿主 seam,
    // 谁也不知道底下抛不抛):整件包住,炸了折成 err 表(§6.2)。
    try {
        // §九第 3 步:顶层/调用外一票否决——先查 context,查不到就退,
        // transport/resolver 一根毛都不碰(假件计数器钉死为 0 的机关在此)。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (lua_gettop(L) < 1 || !lua_istable(L, 1)) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::InvalidRequest, "luban.http.request 须收一张请求表", 0, false);
            return 2;
        }
        auto request = ParseHttpRequestTable(L);
        if (!request.has_value()) {
            lua_pushnil(L);
            PushErrorTable(L, request.error().code, request.error().message, 0, false);
            return 2;
        }
        auto result = ExecutePluginHttp(*request, context->http);
        if (!result.has_value()) {
            lua_pushnil(L);
            PushErrorTable(L, result.error());
            return 2;
        }
        PushResponseTable(L, *result);
        return 1;
    } catch (const std::exception&) {
        // 文案不透 e.what():seam 底下异常文本没过 §11 的禁令,不猜。
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::NetworkFailed, "宿主执行 HTTP 时发生内部异常", 0, true);
        return 2;
    } catch (...) {
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::NetworkFailed, "宿主执行 HTTP 时发生内部异常", 0, true);
        return 2;
    }
}

// ---------------------------------------------------------------------------
// luban.secrets.available / ref(§6.3)
// ---------------------------------------------------------------------------

int LuaSecretsAvailable(lua_State* L) {
    try {
        // 顶层零解析(§6.3):context 都没有就不碰 resolver。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (lua_type(L, 1) != LUA_TSTRING) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::InvalidRequest, "available 须收 Secret 逻辑 id 字符串", 0, false);
            return 2;
        }
        const std::string id = StackString(L, 1);
        const SecretDeclaration* declaration = nullptr;
        for (const SecretDeclaration& candidate : context->http.secrets) {
            if (candidate.id == id) {
                declaration = &candidate;
                break;
            }
        }
        if (declaration == nullptr) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::SecretNotDeclared, "Secret 未声明: " + id, 0, false);
            return 2;
        }
        if (context->http.secret_resolver == nullptr) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::SecretMissing, "SecretResolver 未接线(宿主装配缺口)", 0, false);
            return 2;
        }
        // Describe 只查状态不取值(inspect/doctor 同款口);available/missing
        // 就是 Lua 能看到的全部。
        const SecretStatus status = context->http.secret_resolver->Describe(*declaration);
        lua_pushboolean(L, status.available ? 1 : 0);
        return 1;
    } catch (const std::exception&) {
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::SecretMissing, "查询 Secret 状态时发生内部异常", 0, false);
        return 2;
    } catch (...) {
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::SecretMissing, "查询 Secret 状态时发生内部异常", 0, false);
        return 2;
    }
}

int LuaSecretsRef(lua_State* L) {
    try {
        // 顶层零解析:context 为空即退,resolver 一根毛不碰。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (lua_type(L, 1) != LUA_TSTRING) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::InvalidRequest, "ref 须收 Secret 逻辑 id 字符串", 0, false);
            return 2;
        }
        const std::string id = StackString(L, 1);
        bool declared = false;
        for (const SecretDeclaration& candidate : context->http.secrets) {
            if (candidate.id == id) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            lua_pushnil(L);
            PushErrorTable(L, LuaHostErrorCode::SecretNotDeclared, "Secret 未声明: " + id, 0, false);
            return 2;
        }
        // ref 只是不透明引用:不解析、不持值(解析在 request.auth 的注入链
        // 里,宿主侧 SecretValue RAII 管寿命)。
        PushSecretRef(L, id);
        return 1;
    } catch (const std::exception&) {
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::SecretMissing, "创建 Secret 引用时发生内部异常", 0, false);
        return 2;
    } catch (...) {
        lua_pushnil(L);
        PushErrorTable(L, LuaHostErrorCode::SecretMissing, "创建 Secret 引用时发生内部异常", 0, false);
        return 2;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 模块注册与动态调用上下文
// ---------------------------------------------------------------------------

void RegisterLuaHostModule(lua_State* L) {
    lua_createtable(L, 0, 2);  // luban

    lua_createtable(L, 0, 1);  // luban.http
    lua_pushcfunction(L, LuaHttpRequest);
    lua_setfield(L, -2, "request");
    lua_setfield(L, -2, "http");

    lua_createtable(L, 0, 2);  // luban.secrets
    lua_pushcfunction(L, LuaSecretsAvailable);
    lua_setfield(L, -2, "available");
    lua_pushcfunction(L, LuaSecretsRef);
    lua_setfield(L, -2, "ref");
    lua_setfield(L, -2, "secrets");

    lua_setglobal(L, "luban");
}

LuaCallContext* CurrentLuaCallContext(lua_State* L) {
    if (L == nullptr) {
        return nullptr;
    }
    lua_pushstring(L, kCallContextRegistryKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* context = static_cast<LuaCallContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return context;
}

ScopedLuaCallContext::ScopedLuaCallContext(lua_State* L, LuaCallContext* context) : lua_(L) {
    lua_pushstring(lua_, kCallContextRegistryKey);
    lua_pushlightuserdata(lua_, context);
    lua_rawset(lua_, LUA_REGISTRYINDEX);
}

ScopedLuaCallContext::~ScopedLuaCallContext() {
    if (lua_ != nullptr) {
        // §九第 6 步:lua_pcall 返回/异常/取消路径都走这里——清成 nil,
        // 第二次调用不见上次的 context(旧 Secret/取消旗全断)。
        lua_pushstring(lua_, kCallContextRegistryKey);
        lua_pushnil(lua_);
        lua_rawset(lua_, LUA_REGISTRYINDEX);
    }
}

// ---------------------------------------------------------------------------
// LuaHostState
// ---------------------------------------------------------------------------

LuaHostState::~LuaHostState() {
    if (lua_ != nullptr) {
        // entry_refs_ 跟着整个 state 一并回收,不用逐枚 luaL_unref。
        lua_close(lua_);
        lua_ = nullptr;
    }
}

std::expected<std::unique_ptr<LuaHostState>, std::string> LuaHostState::Load(Options options) {
    // entry 重复在门口拒:manifest 解析(阶段 4)也会拒,这里是机制层的
    // 第二道——重复 entry 挂两枚 ref 就是两份分派账,不清不楚。
    for (std::size_t i = 0; i < options.entries.size(); ++i) {
        for (std::size_t j = i + 1; j < options.entries.size(); ++j) {
            if (options.entries[i] == options.entries[j]) {
                return std::unexpected("entry 重复: " + options.entries[i]);
            }
        }
    }

    std::unique_ptr<tools::LuaGuard> guard;
    lua_State* L = tools::NewGuardedLuaState(options.profile, guard);
    if (L == nullptr) {
        return std::unexpected("lua_newstate 失败(内存不够?)");
    }
    const auto fail = [&L](std::string message) {
        lua_close(L);
        L = nullptr;
        return std::unexpected(std::move(message));
    };

    // §九第 1 步:开 Pure 库、注册 luban 模块。三道墙(内存帽/指令预算/
    // hook)由 NewGuardedLuaState 落好,与 tools::LuaTool 同款。
    luaL_openlibs(L);
    if (options.profile.level == tools::LuaProfile::Level::Pure) {
        tools::ApplyPureLuaProfile(L);
    }
    RegisterLuaHostModule(L);

    // §九第 2/3 步:context 为空跑顶层 chunk。脚本顶层调 luban.http.request
    // / luban.secrets.* 只会拿 no_active_tool_call,零网络零解析——恶意顶
    // 层脚本在此翻不出浪;翻不出还要硬翻,error() 会被下面的 pcall 接住,
    // 整件拒挂。
    if (luaL_loadbuffer(L, options.script.data(), options.script.size(), options.chunk_name.c_str()) != LUA_OK) {
        std::string message = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "(没有错误信息)";
        return fail("脚本编译失败: " + message);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string message = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "(没有错误信息)";
        return fail("脚本执行失败: " + message);
    }
    if (!lua_istable(L, -1)) {
        return fail("脚本返回值不是表(要 return { <entry> = function, ... } 这样一张 handler 表)");
    }

    // §九第 4 步 + §6.1:逐枚 manifest entry 对账。缺 handler/非 function,
    // 整件拒挂(state 关闭,不留半个插件);多出的未声明 function 不挂成
    // 工具,留着便是。
    std::map<std::string, int> entry_refs;
    for (const std::string& entry : options.entries) {
        lua_getfield(L, -1, entry.c_str());
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return fail("manifest 工具 '" + entry + "' 在 Lua 返回表里找不到 function handler");
        }
        entry_refs[entry] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pop(L, 1);  // 弹返回表,栈清空

    auto state = std::unique_ptr<LuaHostState>(new LuaHostState());
    state->lua_ = L;
    state->guard_ = std::move(guard);
    state->profile_ = options.profile;
    state->entry_refs_ = std::move(entry_refs);
    state->entries_ = std::move(options.entries);
    L = nullptr;  // 所有权移交,fail 走不到了
    return state;
}

LuaHostState::CallResult LuaHostState::Call(const std::string& entry, const nlohmann::json& input,
                                            LuaCallContext& context) {
    // §8.5:同一 state 由 mutex 串行;不同插件各有 state,彼此并行。
    const std::lock_guard<std::mutex> lock(call_mutex_);

    const auto entry_it = entry_refs_.find(entry);
    if (entry_it == entry_refs_.end()) {
        CallResult missing;
        missing.content = "entry 未在加载时对账: " + entry;
        missing.is_error = true;
        missing.outcome = "plugin_exception";
        missing.error_code = "plugin.entry_missing";
        return missing;
    }

    // 每次调用换一轮账:指令计数清零(预算是单次的)、取消旗灌进 guard。
    // §8.4:instruction hook 与 HTTP 回调共用同一枚 flag——context 里那根
    // 既给 guard(hook 查),又随 PluginHttpCallSpec 递给 transport(阻塞
    // C 边界查),两条取消链一个真值,不造第二根。
    if (guard_ != nullptr) {
        guard_->instructions_used = 0;
        guard_->budget_hit = false;
        guard_->cancel = context.http.cancel;
    }

    lua_rawgeti(lua_, LUA_REGISTRYINDEX, entry_it->second);
    tools::PushJsonToLua(lua_, input);

    CallResult out;
    {
        // §九第 5/6 步:RAII 绑 context;pcall 返回/Lua error/取消/宿主异常
        // 展开,析构都清空。上一次调用的 Secret 与取消旗到不了这一次。
        ScopedLuaCallContext scope(lua_, &context);
        if (lua_pcall(lua_, 1, 1, 0) != LUA_OK) {
            std::string message = lua_tostring(lua_, -1) != nullptr ? lua_tostring(lua_, -1) : "(没有错误信息)";
            lua_pop(lua_, 1);
            out.content = "lua 执行出错: " + message;
            out.is_error = true;
            out.outcome = "plugin_exception";
            out.error_code = "plugin.lua_error";
            return out;
        }

        // 返回值字符串化:与 tools::LuaTool 同款(字符串原样、数字/布尔转
        // 文本、表转 JSON、nil 算错)。
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
                std::string convert_error;
                const nlohmann::json converted = tools::LuaValueToJson(lua_, -1, 0, convert_error);
                if (!convert_error.empty()) {
                    out.content = "lua 返回的表转不成 JSON: " + convert_error;
                    out.is_error = true;
                } else {
                    out.content = converted.dump();
                }
                break;
            }
            case LUA_TNIL:
                out.content = "lua 的 handler 没有返回值(要 return 一个结果)";
                out.is_error = true;
                break;
            default:
                out.content = std::string("lua 的 handler 返回了没法字符串化的类型: ") +
                              lua_typename(lua_, lua_type(lua_, -1));
                out.is_error = true;
                break;
        }
        lua_pop(lua_, 1);
    }
    return out;
}

}  // namespace lubancode::runtime
