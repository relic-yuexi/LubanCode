# Q2 量化评测装置(工具与上下文治理量化评测单 P0)。
#
# 本文件只在 tests/CMakeLists.txt 的 opt-in 段(LUBANCODE_BUILD_EVAL=ON,
# 默认 OFF)里被 add_subdirectory——默认构建与 CI 三腿不进这里,零新目标
# 零新测试。打开后:
#   cmake -B build/eval -DLUBANCODE_BUILD_EVAL=ON <源码根>
#   cmake --build build/eval --target eval_smoke eval_tool_search_threshold eval_compact_position
#   ctest --test-dir build/eval -L eval        (多配置生成器加 -C <cfg>)
#
# 目录即实验(单子 §五):
#   tool_search_threshold/  实验 A:工具检索退化阈值(P0 = A2/disabled/T1)
#   compaction_benefit/     实验 B2 备料 + collect(数据件,不编 C++)
#   compact_position/       实验 B1:位置探针(造稿+三处理驱动+collect)
#   subagent_failure/       实验 C:失败分布(collect 骨架,驱动后补)
# 共用件:fake_backend.{hpp,cpp}(通用假后端)与 collect_common.py。

# 原始账要带 commit 号(单子 §一合同),configure 时取一次。
set(LUBANCODE_EVAL_GIT_COMMIT "unknown")
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE _eval_git_commit OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET TIMEOUT 10)
if(_eval_git_commit)
  set(LUBANCODE_EVAL_GIT_COMMIT "${_eval_git_commit}")
endif()

# ---- 通用假后端 --------------------------------------------------------------
# FakeStreamingBackend:蓝本 test_loop.cpp 的脚本式 FakeBackend,通用化方向
# (可编程脚本/注错断流/usage 记账)见 fake_backend.hpp 头注。只给 eval 驱动
# 用,不进 lubancode_tests。
add_library(eval_fake_backend STATIC fake_backend.cpp)
target_link_libraries(eval_fake_backend PUBLIC lubancode_engine)
target_compile_features(eval_fake_backend PRIVATE cxx_std_23)
# 头与实现同目录,自身就要能 include "fake_backend.hpp"。
target_include_directories(eval_fake_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---- 冒烟:假 backend 自检五案 -------------------------------------------------
add_executable(eval_smoke eval_smoke.cpp)
target_link_libraries(eval_smoke PRIVATE eval_fake_backend)
target_compile_features(eval_smoke PRIVATE cxx_std_23)
add_test(NAME eval.smoke COMMAND eval_smoke)
set_tests_properties(eval.smoke PROPERTIES LABELS "eval;smoke" TIMEOUT 60)

# ---- 实验 A P0:A2 档 disabled 模式单步 T1 -----------------------------------
# 40 只工具(12 核心 + 28 延迟)全量直挂,假 backend 一轮(两步:调
# read_file → 收口),逐次落 JSONL;collect 步骤出 csv+md 双件。
add_executable(eval_tool_search_threshold tool_search_threshold/eval_driver.cpp)
target_link_libraries(eval_tool_search_threshold PRIVATE eval_fake_backend)
target_compile_features(eval_tool_search_threshold PRIVATE cxx_std_23)
target_compile_definitions(eval_tool_search_threshold PRIVATE
  LUBANCODE_EVAL_TOOL_SEARCH_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tool_search_threshold"
  LUBANCODE_GIT_COMMIT="${LUBANCODE_EVAL_GIT_COMMIT}")
add_test(NAME eval.tool_search_threshold.a2_disabled_t1
  COMMAND eval_tool_search_threshold)
set_tests_properties(eval.tool_search_threshold.a2_disabled_t1 PROPERTIES
  LABELS "eval;tool_search_threshold;eval.tool_search_threshold" TIMEOUT 120)

# collect:原始 JSONL → csv + md(本机 pandas/pyarrow 齐时顺手出 parquet)。
# python 不在就只 STATUS 不注册,不给默认路添堵。
find_program(LUBANCODE_EVAL_PYTHON_EXECUTABLE python)
if(LUBANCODE_EVAL_PYTHON_EXECUTABLE)
  add_test(NAME eval.tool_search_threshold.collect
    COMMAND "${LUBANCODE_EVAL_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tool_search_threshold/collect.py"
            --raw "${CMAKE_CURRENT_SOURCE_DIR}/tool_search_threshold/results/raw_a2_disabled_t1.jsonl"
            --out-dir "${CMAKE_CURRENT_SOURCE_DIR}/tool_search_threshold/results")
  set_tests_properties(eval.tool_search_threshold.collect PROPERTIES
    LABELS "eval;tool_search_threshold;eval.tool_search_threshold" TIMEOUT 120
    DEPENDS "eval.tool_search_threshold.a2_disabled_t1")
else()
  message(STATUS "tests/eval: python 未找到,collect 步骤不注册(其余 eval 测试照跑)")
endif()

# ---- 实验 B1:compact 位置探针(首仗;假后端全链管道,问答判卷) -----------
# 造稿(python,needle 分层 contract/evidence 各半)→ 三处理(FULL 原文 /
# microcompact 折叠 / compact 六栏摘要假后端替身,阈值全用产品默认)→
# 问答判卷(grounded 假后端读三版视图作答,判卷比对模型回答,不再扫视图
# 找原词;视图级子串在场仅作 FULL/microcompact 辅助诊断列)→ 落账(JSONL)
# → collect(位置-命中曲线分层 + 中段存活率 + compact 反救中段 + 折叠分桶)。
# 全链由 compact_position/pipeline.py 串成一个 ctest:
#   eval.compact_position.pipeline
# 问答判卷(含失败注入负路径:答旧值=stale、答不出=lost)与管道的自检在
# 驱动进程内(章法同 eval_smoke 的 RunFakeBackendSelfCheck),自检不过
# exit 1,管道即红。真模型跑法见 compact_position/README.md(装置阶段不点炮)。
add_executable(eval_compact_position compact_position/eval_driver.cpp)
target_link_libraries(eval_compact_position PRIVATE eval_fake_backend)
target_compile_features(eval_compact_position PRIVATE cxx_std_23)
target_compile_definitions(eval_compact_position PRIVATE
  LUBANCODE_EVAL_COMPACT_POSITION_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/compact_position"
  LUBANCODE_GIT_COMMIT="${LUBANCODE_EVAL_GIT_COMMIT}")

if(LUBANCODE_EVAL_PYTHON_EXECUTABLE)
  add_test(NAME eval.compact_position.pipeline
    COMMAND "${LUBANCODE_EVAL_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/compact_position/pipeline.py"
            --python "${LUBANCODE_EVAL_PYTHON_EXECUTABLE}"
            --driver "$<TARGET_FILE:eval_compact_position>"
            --root "${CMAKE_CURRENT_SOURCE_DIR}/compact_position")
  set_tests_properties(eval.compact_position.pipeline PROPERTIES
    LABELS "eval;compact_position;eval.compact_position" TIMEOUT 600)
else()
  message(STATUS "tests/eval: python 未找到,compact_position 管道不注册")
endif()

# ---- LoCoMo 判分器固定集(LoCoMo 评测纠偏单 P1,离线零模型零钥匙) -----------
# 旧 runner 监听不存在的 model.response.completed 后回落 stdout 捞数字,
# E2 结论已作废(见 eval/locomo/*.md 失效框)。本段锁新判分器:
#   scripts/eval_locomo_score.py(严格单数字、只认 schema v2 end_turn、
#   stdout 永不进判分)对 tests/eval/locomo_scorer/fixtures/ 的固定集——
# 19 件真源脱敏夹具(9 个 B mismatch + 9 个"不知道" + no_end_turn 工具场,
#   溯源见 fixtures/manifest.json)+ 10 件合成形状,另含 runner 装配级
#   断言(假 subprocess 塞数字汤,choice 仍按 trajectory 记)。
# 夹具重建须本机原始账(make_fixtures.py --real),ctest 只读已提交件。
if(LUBANCODE_EVAL_PYTHON_EXECUTABLE)
  add_test(NAME eval.locomo_scorer.fixtures
    COMMAND "${LUBANCODE_EVAL_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/locomo_scorer/test_locomo_scorer.py"
            --scorer "${CMAKE_SOURCE_DIR}/scripts/eval_locomo_score.py"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/locomo_scorer/fixtures")
  set_tests_properties(eval.locomo_scorer.fixtures PROPERTIES
    LABELS "eval;locomo_scorer;eval.locomo_scorer" TIMEOUT 120)
else()
  message(STATUS "tests/eval: python 未找到,locomo_scorer 固定集不注册")
endif()
