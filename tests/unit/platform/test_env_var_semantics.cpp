// platform::GetEnvVar 的空值语义三格钉住(src 收口审计 P3 候选:
// run_command 私有的 ReadEnvironmentVariable 把"空字符串"当
// optional("") 返回,平台入口把"未设置/空串"都当 nullopt——先钉住平台
// 侧合同,再决定并入还是具名分叉)。
//
// 三格:未设置 -> nullopt;设成空串 -> nullopt;非空 -> 值。
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "platform/paths.hpp"

namespace {

#ifdef _WIN32
void SetEnvForTest(const char* name, const char* value) {
    if (value != nullptr) {
        REQUIRE(_putenv_s(name, value) == 0);
    } else {
        REQUIRE(_putenv_s(name, "") == 0);  // Windows 删环境变量的正路:置空
    }
}
#else
void SetEnvForTest(const char* name, const char* value) {
    if (value != nullptr) {
        REQUIRE(::setenv(name, value, 1) == 0);
    } else {
        ::unsetenv(name);
    }
}
#endif

}  // namespace

TEST_CASE("GetEnvVar: 未设置/空串/非空 三格语义") {
    const char* name = "LUBANCODE_TEST_ENV_CELL";

    SetEnvForTest(name, nullptr);
    CHECK_FALSE(lubancode::platform::GetEnvVar(name).has_value());

    SetEnvForTest(name, "");
    CHECK_FALSE(lubancode::platform::GetEnvVar(name).has_value());

    SetEnvForTest(name, "v");
    const auto value = lubancode::platform::GetEnvVar(name);
    REQUIRE(value.has_value());
    CHECK(*value == "v");

    SetEnvForTest(name, nullptr);  // 还原,别漏给同进程的其他测试
}
