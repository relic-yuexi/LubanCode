-- AnySearch 参考插件(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 5)。
--
-- 章法(设计单 §一/§十二):
--   - 只用 Host API:luban.http.request 与 auth 的 secret 糖。socket、
--     TLS、DNS、超时、字节帽、取消全在宿主手里,Lua 只描述请求形状,
--     不碰任何宿主未开放的能力(io、os、package 一概没动)。
--   - 工具合同只认 plugin.json,这里不抄第二份 schema;返回 handler 表,
--     键名与 manifest 的 tools[].entry 一一对应。
--   - Key 铁律(§12.2):ANYSEARCH_API_KEY 声明为 optional,宿主解析后
--     代填 Authorization 头,Lua 与模型都摸不到原文。API 响应若带回
--     auto_registered.api_key,本脚本丢弃该值——不回给模型、不写 .env,
--     只附一句非敏感提示。日后有交互式 Secret Store 写入 API 再另开
--     保存流程,须用户确认。
--   - batch_search 第一版串行:一笔发完等回应再发下一笔;中途取消则
--     停手,余笔标 skipped,不装作发过。
--
-- 返回形状(宿主把表转 JSON 交模型):
--   成功:  { status, data, request_id?, key_notice? }
--   失败:  { status, error = { code, message, retryable? } }
--   batch: { results = { 上述形状按序 }, cancelled?, key_notice? }

local API_BASE = "https://api.anysearch.com"

local CLIENT_HEADER = "lubancode-lua/0.1.0"
local TIMEOUT_MS = 20000 -- 与 manifest limits.http_timeout_ms 同数;只许下调

-- auth 糖(§6.3):secret 按逻辑 id 递宿主解析;optional=true 时缺失即
-- 匿名发,不注入 Authorization。
local AUTH = { type = "bearer", secret = "api_key", optional = true }

local KEY_NOTICE = "服务端提供新 Key,当前宿主不自动保存"

-- Key 铁律的落地:响应里 auto_registered(顶层或 data 内)一律摘除。
-- 就地改净化后的表,返回提示语;值本身谁也不给。
local function drop_auto_registered(payload)
    local notice = nil
    local holders = { payload }
    if type(payload.data) == "table" then
        holders[#holders + 1] = payload.data
    end
    for _, holder in ipairs(holders) do
        if holder.auto_registered ~= nil then
            holder.auto_registered = nil
            notice = KEY_NOTICE
        end
    end
    return notice
end

-- 传输/宿主错误(§11 的 err 表)翻统一形状。
local function transport_error(err)
    local failure = { code = err.code, message = err.message }
    if err.retryable then
        failure.retryable = true
    end
    return { status = err.status or 0, error = failure }
end

-- HTTP 非 2xx 翻人话(§11:status 原样带回,厂商语义由 Lua 定)。
local function status_message(status)
    if status == 401 then
        return "未授权:API Key 缺失或无效;匿名访问仍可用,配置 ANYSEARCH_API_KEY 可提高限额"
    elseif status == 429 then
        return "限流:请求太密;稍候再试,或配置 ANYSEARCH_API_KEY 提高限额"
    elseif status >= 500 then
        return "AnySearch 服务端错误"
    end
    return nil
end

-- 一笔请求的终态整理:宿主 err / 非 2xx / 业务 code 非 0 / 成功,四路归一。
local function finish(response, err)
    if err ~= nil then
        return transport_error(err)
    end
    local payload = type(response.json) == "table" and response.json or {}
    local notice = drop_auto_registered(payload)
    local status = response.status

    if status >= 400 then
        local message = status_message(status) or ("HTTP " .. status)
        if type(payload.message) == "string" and #payload.message > 0 then
            message = message .. ";服务端话:" .. payload.message
        end
        return { status = status, error = { code = "http_" .. status, message = message }, key_notice = notice }
    end

    if payload.code ~= nil and payload.code ~= 0 then
        local message = type(payload.message) == "string" and payload.message or "AnySearch 业务错误"
        return {
            status = status,
            error = { code = "api_error_" .. tostring(payload.code), message = message },
            key_notice = notice,
        }
    end

    local out = { status = status, data = payload.data }
    if type(payload.request_id) == "string" then
        out.request_id = payload.request_id
    end
    if notice ~= nil then
        out.key_notice = notice
    end
    return out
end

-- RFC 3986 未保留字符之外的字节按 %XX 编码(域名/参数值进 query 用)。
local function url_encode(text)
    return (text:gsub("[^%w%-._~]", function(ch)
        return string.format("%%%02X", ch:byte())
    end))
end

local function request_get(path, query)
    local url = API_BASE .. path
    if query ~= nil and #query > 0 then
        url = url .. "?" .. query
    end
    local response, err = luban.http.request({
        method = "GET",
        url = url,
        headers = { Accept = "application/json", ["X-Anysearch-Client"] = CLIENT_HEADER },
        auth = AUTH,
        timeout_ms = TIMEOUT_MS,
    })
    return finish(response, err)
end

local function request_post(path, body)
    local response, err = luban.http.request({
        method = "POST",
        url = API_BASE .. path,
        headers = { Accept = "application/json", ["X-Anysearch-Client"] = CLIENT_HEADER },
        json = body,
        auth = AUTH,
        timeout_ms = TIMEOUT_MS,
    })
    return finish(response, err)
end

-- search 请求体:笔内字段优先,共享层兜底(batch 用)。
local function search_body(item, shared)
    local body = { query = item.query }
    local domain = item.domain or shared.domain
    local sub_domain = item.sub_domain or shared.sub_domain
    local params = item.sub_domain_params or shared.sub_domain_params
    local max_results = item.max_results or shared.max_results
    if domain ~= nil then
        body.domain = domain
    end
    if sub_domain ~= nil then
        body.sub_domain = sub_domain
    end
    if params ~= nil then
        body.sub_domain_params = params
    end
    if max_results ~= nil then
        body.max_results = max_results
    end
    return body
end

return {
    get_sub_domains = function(input)
        local parts = {}
        for _, domain in ipairs(input.domains) do
            parts[#parts + 1] = "domain=" .. url_encode(domain)
        end
        return request_get("/v1/sub-domains", table.concat(parts, "&"))
    end,

    search = function(input)
        return request_post("/v1/search", search_body(input, {}))
    end,

    batch_search = function(input)
        local shared = input
        local results = {}
        local notice = nil
        local cancelled = false
        for index, item in ipairs(input.queries) do
            if cancelled then
                results[index] = { status = 0, error = { code = "skipped_after_cancel", message = "前一笔已取消,本笔未发出" } }
            else
                local outcome = request_post("/v1/search", search_body(item, shared))
                if outcome.key_notice ~= nil then
                    notice = outcome.key_notice
                end
                results[index] = outcome
                if outcome.error ~= nil and outcome.error.code == "cancelled" then
                    cancelled = true
                end
            end
        end
        local out = { results = results }
        if notice ~= nil then
            out.key_notice = notice
        end
        if cancelled then
            out.cancelled = true
        end
        return out
    end,

    extract = function(input)
        return request_post("/v1/extract", { url = input.url })
    end,
}
