"""Configured Lua ownership and profile rules; pure data, no native calls."""
from __future__ import annotations

LUA_ENGINE_SOURCE = "src/tools/lua_tool.cpp"
LUA_RUNTIME_SOURCES = frozenset({
    "src/runtime/plugin_lua.cpp", "src/runtime/plugin_lua_host.cpp",
    "src/runtime/plugin_lua_manifest.cpp", "src/runtime/middleware_assembly.cpp",
    "src/runtime/hook_package_check.cpp",
})
LUA_SOURCES = LUA_RUNTIME_SOURCES | {LUA_ENGINE_SOURCE}
LUA_LOAD = "src/sdk/lua_load.cpp"
LUA_UNAVAILABLE = "src/sdk/lua_unavailable.cpp"
TRUE = {"ON", "TRUE", "YES", "1"}
FALSE = {"OFF", "FALSE", "NO", "0"}


def read_lua_profile(entries: dict, expected: str | None = None) -> bool:
    raw = entries.get("LUBANCORE_WITH_LUA")
    if raw is None:
        if expected is not None:
            raise ValueError("configured Lua build profile is missing")
        return True  # Older pure graph fixtures predate this cache option.
    value = str(raw).upper()
    if value not in TRUE | FALSE:
        raise ValueError("configured Lua build profile is not boolean")
    enabled = value in TRUE
    if expected is not None and enabled != (expected == "on"):
        raise ValueError("configured Lua build profile differs from requested evidence")
    if not enabled and (str(entries.get("LUBANCODE_BUILD_CLI", "")).upper() not in FALSE or
                        str(entries.get("LUBANCODE_BUILD_SDK", "")).upper() not in TRUE):
        raise ValueError("Lua OFF requires CLI OFF and SDK ON")
    return enabled


def lua_graph_violations(targets: dict, enabled: bool) -> list[str]:
    errors = []
    def owners(source):
        return [(target["name"], target["type"]) for target in targets.values()
                for item in target["projectSources"] if item == source]
    wanted, wrong = (LUA_LOAD, LUA_UNAVAILABLE) if enabled else (LUA_UNAVAILABLE, LUA_LOAD)
    if owners(wanted) != [("lubancore_sdk", "SHARED_LIBRARY")]:
        errors.append("Lua selected loading implementation must belong once to the shared SDK: " + wanted)
    if owners(wrong):
        errors.append("Lua unselected loading implementation is compiled: " + wrong)
    lua = [target for target in targets.values() if target["name"] == "lubancode_lua"]
    if enabled:
        if len(lua) != 1 or lua[0]["type"] != "STATIC_LIBRARY":
            errors.append("Lua ON requires one static lubancode_lua target")
        for source in sorted(LUA_SOURCES):
            wanted_owner = "lubancode_engine" if source == LUA_ENGINE_SOURCE else "lubancode_runtime"
            if owners(source) != [(wanted_owner, "STATIC_LIBRARY")]:
                errors.append("Lua source must retain its sole static owner: " + source)
    else:
        if lua or any(target["name"].casefold() in {"lubancode_lua", "lua", "lua_static", "lua_shared"} for target in targets.values()):
            errors.append("Lua OFF defines a Lua native target")
        for source in sorted(LUA_SOURCES):
            if owners(source): errors.append("Lua OFF compiles an excluded implementation: " + source)
        for target in targets.values():
            for source in target["projectSources"]:
                if "lua-src/" in source.replace("\\", "/") or "lua-build/" in source.replace("\\", "/"):
                    errors.append("Lua OFF compiles native Lua dependency material: " + source)
    return errors


def focused_roster(full: set[str], enabled: bool) -> set[str]:
    return set(full) if enabled else full - {"sdk.focused.lubancore_lua", "sdk.focused.lua_protected"}


def consumer_roster(full: set[str], enabled: bool) -> set[str]:
    return set(full) if enabled else full - {"sdk.consumer.lua", "sdk.consumer.lua_seed", "sdk.consumer.lua_resume"}
