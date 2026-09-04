// eval 装置冒烟(Q2 量化评测单 P0):证明 opt-in 构建树里假 backend 与
// 事件工厂真的编得过、跑得动。一个进程一页账:
//   eval_smoke   跑 FakeStreamingBackend 自检五案,全过打 OK 退 0。
// 不碰网络、不碰真钥匙、不落文件——重活都在 tool_search_threshold 驱动。

#include <cstdio>
#include <string>

#include "fake_backend.hpp"

int main() {
    std::string error;
    if (!lubancode_eval::RunFakeBackendSelfCheck(&error)) {
        std::fprintf(stderr, "eval_smoke: FAIL %s\n", error.c_str());
        return 1;
    }
    std::printf("eval_smoke: OK (FakeStreamingBackend 5 案自检全过)\n");
    return 0;
}
