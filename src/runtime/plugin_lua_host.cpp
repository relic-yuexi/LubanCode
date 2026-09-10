// Lua Host API 与动态调用上下文的实现(阶段 3)。§六的模块形状、§九的
// 六步时序、§11 的错误合同都在这里落地;HTTP/Secret 的真活全在阶段 0-2
// 的件里(ExecutePluginHttp/SecretResolver/transport seam),本文件只做
// Lua 边界的形状转换与分派——所以这里也没有第二份网络执法,越权、超帽、
// DNS 安全都还是 transport 的账。
#include "runtime/plugin_lua_host.hpp"

#include <new>
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

// LuaHook 单 P0-A:hook 调用作用域不配工具 Host API(§二:不能伪造 tool
// call 来开权限)。nil + err(not_tool_context),零网络、零 Secret 解析。
int PushNotToolContext(lua_State* L) {
    lua_pushnil(L);
    PushErrorTable(L, LuaHostErrorCode::NotToolContext,
                   std::string(LuaHostErrorCodeDefaultMessage(LuaHostErrorCode::NotToolContext)), 0, false);
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
        // LuaHook 单 P0-A:hook 作用域同样一票否决(not_tool_context)——
        // hook 不能借道工具 Host API 开权限。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (context->kind != LuaCallContext::Kind::Tool) {
            return PushNotToolContext(L);
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
        // 顶层零解析(§6.3):context 都没有就不碰 resolver;hook 作用域
        // 同拒(P0-A:hook 不开 Secret 能力)。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (context->kind != LuaCallContext::Kind::Tool) {
            return PushNotToolContext(L);
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
        // 顶层零解析:context 为空即退,resolver 一根毛不碰;hook 作用域同拒。
        LuaCallContext* context = CurrentLuaCallContext(L);
        if (context == nullptr) {
            return PushNoActiveToolCall(L);
        }
        if (context->kind != LuaCallContext::Kind::Tool) {
            return PushNotToolContext(L);
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

    // §九第 1 步:按画像开库(Pure/Trusted 开全库按需关门;Whitelisted
    // 是 LuaHook 单 P0-A 的 hook state——从零开显式白名单,§五)。三道墙
    // (内存帽/指令预算+墙钟/hook)由 NewGuardedLuaState 落好。
    tools::OpenLuaLibraries(L, options.profile);
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

// ---------------------------------------------------------------------------
// LuaHook 单 P0-A:hook 中间件调用(handler(ctx, input, next))。
// ---------------------------------------------------------------------------

namespace {

// next 槽:宿主续体 + 一次性/过期账。闭包经 userdata 持 shared_ptr(Lua GC
// 管寿命);CallHook 收口时置 expired——同一 state 被复用时,脚本偷存的旧
// next 只能拿到 hook.next.expired,不是悬垂指针。
struct LuaHookNextSlot {
    std::function<std::expected<LuaHostState::LuaHookDownstream, LuaHostState::LuaHookNextError>(
        const std::optional<nlohmann::json>&)>
        invoke;
    bool consumed = false;
    bool expired = false;
    int calls = 0;
};

const char* kHookNextSlotMetatable = "luban.hook.nextslot";
const char* kHookDenyMarker = "__luban_hook_deny";

int HookNextSlotGc(lua_State* L) {
    auto* holder = static_cast<std::shared_ptr<LuaHookNextSlot>*>(lua_touserdata(L, 1));
    if (holder != nullptr) {
        std::destroy_at(holder);
    }
    return 0;
}

// 协议违规:错误值是带稳定码的表(CallHook 据此分型,不解析文案)。
int RaiseHookError(lua_State* L, std::string_view code, std::string_view message) {
    lua_createtable(L, 0, 2);
    lua_pushlstring(L, code.data(), code.size());
    lua_setfield(L, -2, "code");
    lua_pushlstring(L, message.data(), message.size());
    lua_setfield(L, -2, "message");
    return lua_error(L);
}

void PushDownstreamTable(lua_State* L, const LuaHostState::LuaHookDownstream& downstream) {
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, downstream.status.data(), downstream.status.size());
    lua_setfield(L, -2, "status");
    tools::PushJsonToLua(L, downstream.value);
    lua_setfield(L, -2, "value");
    if (!downstream.code.empty()) {
        lua_pushlstring(L, downstream.code.data(), downstream.code.size());
        lua_setfield(L, -2, "code");
    }
    if (!downstream.message.empty()) {
        lua_pushlstring(L, downstream.message.data(), downstream.message.size());
        lua_setfield(L, -2, "message");
    }
}

int HookNextClosure(lua_State* L) {
    auto* holder = static_cast<std::shared_ptr<LuaHookNextSlot>*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (holder == nullptr || *holder == nullptr) {
        return RaiseHookError(L, "hook.next.expired", "next 槽位已失效");
    }
    LuaHookNextSlot& slot = **holder;
    ++slot.calls;
    if (slot.expired) {
        return RaiseHookError(L, "hook.next.expired", "next 已过终态(终态后/跨 invocation 调用一律拒绝)");
    }
    if (slot.consumed) {
        return RaiseHookError(L, "hook.next.already_consumed", "next 至多调用一次;本次调用未执行下游");
    }
    if (!slot.invoke) {
        return RaiseHookError(L, "hook.next.not_allowed", "本 handler 没有 next");
    }
    std::optional<nlohmann::json> candidate;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        if (!lua_istable(L, 1)) {
            return RaiseHookError(L, "hook.next.bad_candidate", "next 的候选须是表或 nil");
        }
        std::string convert_error;
        nlohmann::json converted = tools::LuaValueToJson(L, 1, 0, convert_error);
        if (!convert_error.empty()) {
            return RaiseHookError(L, "hook.next.bad_candidate", "候选转 JSON 失败: " + convert_error);
        }
        candidate = std::move(converted);
    }
    auto result = slot.invoke(candidate);
    if (!result.has_value()) {
        return RaiseHookError(L, result.error().code, result.error().message);
    }
    slot.consumed = true;
    PushDownstreamTable(L, *result);
    return 1;
}

// ctx.deny(code, message):返回拒绝标记表,handler 直接 return 即短路拒绝。
int HookCtxDeny(lua_State* L) {
    const char* code = luaL_optstring(L, 1, "input_rejected");
    const char* message = luaL_optstring(L, 2, "");
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, kHookDenyMarker);
    lua_pushstring(L, code);
    lua_setfield(L, -2, "code");
    lua_pushstring(L, message);
    lua_setfield(L, -2, "message");
    return 1;
}

// ctx 只读:改写落子即错(身份由宿主发行,Lua 只引用)。
int HookCtxReadOnly(lua_State* L) {
    return RaiseHookError(L, "hook.result.invalid", "ctx 是只读的(身份字段由宿主发行)");
}

// 造 ctx 表:meta 字段 + deny;封 __newindex 锁写。
void PushHookCtxTable(lua_State* L, const nlohmann::json& meta) {
    lua_createtable(L, 0, 8);
    if (meta.is_object()) {
        for (const auto& item : meta.items()) {
            lua_pushlstring(L, item.key().data(), item.key().size());
            tools::PushJsonToLua(L, item.value());
            lua_rawset(L, -3);
        }
    }
    lua_pushcfunction(L, HookCtxDeny);
    lua_setfield(L, -2, "deny");
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, HookCtxReadOnly);
    lua_setfield(L, -2, "__newindex");
    lua_pushliteral(L, "luban.hook.ctx");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
}

}  // namespace

LuaHostState::LuaHookCallResult LuaHostState::CallHook(const std::string& entry, const LuaHookCall& call,
                                                       LuaCallContext& context) {
    LuaHookCallResult out;

    // §4.1:嵌套 handler 不重入同一 state。next 续体里若再进同一 state,
    // 这里拒绝(hook.lua.state_reentry),不拿互斥递归装作支持——
    // call_mutex_ 恒非递归,重入若先去锁只会死锁;探针原子无锁快查,
    // 在摸 mutex 之前就退。
    if (hook_in_flight_.exchange(true)) {
        out.error_code = "hook.lua.state_reentry";
        out.message = "嵌套 handler 不重入同一 state(§4.1);给同一脚本的多个槽位各配独立实现";
        return out;
    }
    struct InFlightRelease {
        std::atomic<bool>& flag;
        ~InFlightRelease() { flag.store(false); }
    } in_flight{hook_in_flight_};

    const std::lock_guard<std::mutex> lock(call_mutex_);
    const int stack_base = lua_gettop(lua_);

    const auto entry_it = entry_refs_.find(entry);
    if (entry_it == entry_refs_.end()) {
        out.error_code = "hook.lua.runtime_error";
        out.message = "entry 未在加载时对账: " + entry;
        return out;
    }

    // 预算:每次调用换一轮账(§8.4/§六)。墙钟从 profile 折;取消旗既给
    // guard(指令 hook 查)又是 Host API 的取消真值。
    if (guard_ != nullptr) {
        guard_->instructions_used = 0;
        guard_->budget_hit = false;
        guard_->last_budget_kind = tools::LuaGuard::BudgetKind::None;
        guard_->cancel = call.cancel;
        guard_->wall_deadline = profile_.wall_budget > std::chrono::milliseconds(0)
                                    ? std::chrono::steady_clock::now() + profile_.wall_budget
                                    : std::chrono::steady_clock::time_point{};
    }

    auto slot = std::make_shared<LuaHookNextSlot>();
    if (call.next) {
        slot->invoke = call.next;
    }

    lua_rawgeti(lua_, LUA_REGISTRYINDEX, entry_it->second);  // handler
    PushHookCtxTable(lua_, call.ctx_meta);                   // ctx
    tools::PushJsonToLua(lua_, call.input);                  // input
    {
        // next 闭包的 upvalue:持 shared_ptr 的 userdata(__gc 释放引用,
        // 不悬垂)。
        void* ud = lua_newuserdatauv(lua_, sizeof(std::shared_ptr<LuaHookNextSlot>), 0);
        if (ud == nullptr) {
            lua_settop(lua_, stack_base);
            out.error_code = "hook.lua.runtime_error";
            out.message = "next 槽位分配失败";
            return out;
        }
        new (ud) std::shared_ptr<LuaHookNextSlot>(slot);
        if (luaL_newmetatable(lua_, kHookNextSlotMetatable) != 0) {
            lua_pushcfunction(lua_, HookNextSlotGc);
            lua_setfield(lua_, -2, "__gc");
            lua_pushstring(lua_, kHookNextSlotMetatable);
            lua_setfield(lua_, -2, "__name");
        }
        lua_setmetatable(lua_, -2);
    }
    lua_pushcclosure(lua_, HookNextClosure, 1);

    ScopedLuaCallContext scope(lua_, &context);
    if (lua_pcall(lua_, 3, 1, 0) != LUA_OK) {
        slot->expired = true;
        out.next_calls = slot->calls;
        // 表错误带稳定码(hook.next.* 协议违规);串错误按预算分型。
        if (lua_istable(lua_, -1)) {
            lua_getfield(lua_, -1, "code");
            const std::string code = lua_type(lua_, -1) == LUA_TSTRING ? StackString(lua_, -1) : std::string();
            lua_pop(lua_, 1);
            if (!code.empty()) {
                lua_getfield(lua_, -1, "message");
                const std::string message =
                    lua_type(lua_, -1) == LUA_TSTRING ? StackString(lua_, -1) : std::string();
                lua_pop(lua_, 1);
                lua_settop(lua_, stack_base);
                out.error_code = code;
                out.message = message;
                return out;
            }
            lua_settop(lua_, stack_base);
            out.error_code = "hook.lua.runtime_error";
            out.message = "(错误值是表但不带稳定码)";
            return out;
        }
        std::string message = lua_tostring(lua_, -1) != nullptr ? lua_tostring(lua_, -1) : "(没有错误信息)";
        lua_settop(lua_, stack_base);
        if (guard_ != nullptr && guard_->budget_hit) {
            using BudgetKind = tools::LuaGuard::BudgetKind;
            switch (guard_->last_budget_kind) {
                case BudgetKind::Instruction:
                    out.error_code = "hook.lua.budget_instruction";
                    break;
                case BudgetKind::Memory:
                    out.error_code = "hook.lua.budget_memory";
                    break;
                case BudgetKind::WallClock:
                    out.error_code = "hook.lua.budget_wallclock";
                    break;
                case BudgetKind::None:
                    break;
            }
        }
        if (out.error_code.empty() && call.cancel != nullptr && call.cancel->load()) {
            out.error_code = "hook.dispatch.cancelled";
        }
        if (out.error_code.empty()) {
            out.error_code = "hook.lua.runtime_error";
        }
        out.message = std::move(message);
        return out;
    }

    out.next_consumed = slot->consumed;
    out.next_calls = slot->calls;
    slot->expired = true;

    switch (lua_type(lua_, -1)) {
        case LUA_TNIL:
            out.ok = true;  // 无返回 = 纯透传(已消费 next)或纯观察
            break;
        case LUA_TSTRING:
        case LUA_TNUMBER:
        case LUA_TBOOLEAN: {
            std::string convert_error;
            out.output = tools::LuaValueToJson(lua_, -1, 0, convert_error);
            out.ok = convert_error.empty();
            if (!out.ok) {
                out.error_code = "hook.result.invalid";
                out.message = convert_error;
            }
            break;
        }
        case LUA_TTABLE: {
            // ctx.deny 的标记表:短路拒绝。
            lua_getfield(lua_, -1, kHookDenyMarker);
            const bool denied = lua_toboolean(lua_, -1) != 0;
            lua_pop(lua_, 1);
            if (denied) {
                lua_getfield(lua_, -1, "code");
                out.deny_code = StackString(lua_, -1);
                lua_pop(lua_, 1);
                lua_getfield(lua_, -1, "message");
                out.deny_message = StackString(lua_, -1);
                lua_pop(lua_, 1);
                out.ok = true;
                out.deny = true;
                break;
            }
            lua_getfield(lua_, -1, "output");
            if (!lua_isnil(lua_, -1)) {
                std::string convert_error;
                out.output = tools::LuaValueToJson(lua_, -1, 0, convert_error);
                lua_pop(lua_, 1);
                if (!convert_error.empty()) {
                    lua_settop(lua_, stack_base);
                    out.error_code = "hook.result.invalid";
                    out.message = "output 字段: " + convert_error;
                    break;
                }
            } else {
                lua_pop(lua_, 1);
            }
            lua_getfield(lua_, -1, "effects");
            if (!lua_isnil(lua_, -1)) {
                if (!lua_istable(lua_, -1)) {
                    lua_settop(lua_, stack_base);
                    out.error_code = "hook.result.invalid";
                    out.message = "effects 字段不是表";
                    break;
                }
                const std::size_t count = lua_rawlen(lua_, -1);
                for (std::size_t i = 1; i <= count; ++i) {
                    lua_rawgeti(lua_, -1, static_cast<lua_Integer>(i));
                    lua_getfield(lua_, -1, "type");
                    if (lua_type(lua_, -1) != LUA_TSTRING) {
                        out.error_code = "hook.result.invalid";
                        out.message = "effects 项缺 type 字符串";
                        lua_pop(lua_, 2);
                        break;
                    }
                    lua_pop(lua_, 1);  // type(项本身转 JSON 时自带)
                    std::string convert_error;
                    nlohmann::json effect = tools::LuaValueToJson(lua_, -1, 0, convert_error);
                    lua_pop(lua_, 1);  // 项
                    if (!convert_error.empty()) {
                        out.error_code = "hook.result.invalid";
                        out.message = "效果项转 JSON 失败: " + convert_error;
                        break;
                    }
                    out.effects.push_back(std::move(effect));
                }
                lua_pop(lua_, 1);  // effects 表
                if (!out.error_code.empty()) {
                    break;
                }
            } else {
                lua_pop(lua_, 1);
            }
            out.ok = true;
            break;
        }
        default:
            out.error_code = "hook.result.invalid";
            out.message = std::string("handler 返回了没法解释的类型: ") + lua_typename(lua_, lua_type(lua_, -1));
            break;
    }
    lua_settop(lua_, stack_base);
    return out;
}

// ---------------------------------------------------------------------------
// Lua 声明 -> 中间件 Handler(每次 invocation 独立 state)。
// ---------------------------------------------------------------------------

std::expected<hooks::middleware::Handler, std::string> MakeLuaHookHandler(
    const hooks::middleware::LuaHandlerSpec& spec, const hooks::middleware::HandlerLimits& limits) {
    // 物化即试编译(发布期把语法/对账错误顶出来,不带半个定义入池)。正文
    // 留在闭包里缓存;invocation 各自再建 state——§4.1:活动调用独立 state,
    // 脚本内存不跨调用保留,不缓存业务结果。
    tools::LuaProfile profile = tools::LuaProfile::HookDefault();
    profile.instruction_budget = limits.instruction_budget;
    profile.memory_cap_bytes = limits.memory_cap_bytes;
    profile.wall_budget = limits.wall_budget;

    LuaHostState::Options probe;
    probe.script = spec.script;
    probe.chunk_name = spec.chunk_name.empty() ? "hook" : spec.chunk_name;
    probe.entries = {spec.entry};
    probe.profile = profile;
    auto probe_state = LuaHostState::Load(probe);
    if (!probe_state.has_value()) {
        return std::unexpected(probe_state.error());
    }

    return hooks::middleware::Handler(
        [spec, profile](const hooks::middleware::InvocationCtx& ctx, const nlohmann::json& input,
                        hooks::middleware::NextCall& next)
            -> std::expected<hooks::middleware::HandlerReturn, hooks::middleware::HandlerError> {
            LuaHostState::Options options;
            options.script = spec.script;
            options.chunk_name = spec.chunk_name.empty() ? "hook" : spec.chunk_name;
            options.entries = {spec.entry};
            options.profile = profile;
            auto state = LuaHostState::Load(options);
            if (!state.has_value()) {
                return std::unexpected(
                    hooks::middleware::HandlerError{"hook.lua.compile_error", state.error()});
            }

            LuaCallContext::HookIdentity identity;
            identity.dispatch_id = ctx.dispatch_id;
            identity.invocation_id = ctx.invocation_id;
            identity.hook_id = ctx.hook_id;
            identity.hook_point = std::string(hooks::middleware::ToString(ctx.point));
            identity.stage = std::string(hooks::middleware::ToString(ctx.stage));
            identity.depth = ctx.depth;
            LuaCallContext context = LuaCallContext::ForHook(std::move(identity));

            LuaHostState::LuaHookCall call;
            call.ctx_meta = nlohmann::json{{"dispatchId", ctx.dispatch_id},
                                           {"invocationId", ctx.invocation_id},
                                           {"hookId", ctx.hook_id},
                                           {"hookPoint", identity.hook_point},
                                           {"stage", identity.stage},
                                           {"depth", ctx.depth},
                                           {"registryRevision", ctx.registry_revision}};
            call.input = input;
            call.cancel = ctx.cancel;
            call.next = [&next](const std::optional<nlohmann::json>& candidate)
                -> std::expected<LuaHostState::LuaHookDownstream, LuaHostState::LuaHookNextError> {
                const auto outcome = next.Call(candidate);
                if (outcome.kind == hooks::middleware::DownstreamOutcome::Kind::Invalid) {
                    LuaHostState::LuaHookNextError error;
                    error.code = outcome.code;
                    error.message = outcome.message;
                    return std::unexpected(error);
                }
                LuaHostState::LuaHookDownstream downstream;
                switch (outcome.kind) {
                    case hooks::middleware::DownstreamOutcome::Kind::Value:
                        downstream.status = "value";
                        break;
                    case hooks::middleware::DownstreamOutcome::Kind::Denied:
                        downstream.status = "denied";
                        break;
                    case hooks::middleware::DownstreamOutcome::Kind::Failed:
                        downstream.status = "failed";
                        break;
                    case hooks::middleware::DownstreamOutcome::Kind::Invalid:
                        break;
                }
                downstream.value = outcome.value;
                downstream.code = outcome.code;
                downstream.message = outcome.message;
                return downstream;
            };

            const auto result = (*state)->CallHook(spec.entry, call, context);
            if (!result.ok) {
                return std::unexpected(hooks::middleware::HandlerError{result.error_code, result.message});
            }
            hooks::middleware::HandlerReturn out;
            out.output = result.output;
            out.deny = result.deny;
            out.deny_code = result.deny_code;
            out.deny_message = result.deny_message;
            for (const auto& raw : result.effects) {
                hooks::middleware::EffectType type{};
                const auto type_it = raw.find("type");
                if (type_it == raw.end() || !type_it->is_string() ||
                    !hooks::middleware::ParseEffectType(type_it->get_ref<const std::string&>(), type)) {
                    return std::unexpected(hooks::middleware::HandlerError{
                        "hook.result.invalid", "效果 type 不认识: " + (type_it == raw.end() ? "(缺)" : type_it->dump())});
                }
                hooks::middleware::Effect effect;
                effect.type = type;
                effect.payload = raw;
                out.effects.push_back(std::move(effect));
            }
            return out;
        });
}

}  // namespace lubancode::runtime
