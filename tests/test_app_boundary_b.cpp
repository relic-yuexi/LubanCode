// app 编译边界钉子(TU B):与 test_app_boundary_a.cpp 同 include 一批 app
// 公共头。返回 0 = 探针符号都接上了;链接失败(重复定义/缺符号)时这只
// TU 根本进不了测试程序,失败本身就是测试结果。
#include "app/backend_stack.hpp"

int BoundaryTuBProbe() {
    return (&lubancode::app::BuildBackend == nullptr) ? 1 : 0;
}
