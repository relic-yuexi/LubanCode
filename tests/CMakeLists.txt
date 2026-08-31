# 测试源码按“种类/领域”落位。目录便是清单：添一册 test_*.cpp，
# CMake 会自动编进总测试二进制，并另登一条可单跑的 CTest。
file(GLOB_RECURSE LUBANCODE_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/*/test_*.cpp")
list(SORT LUBANCODE_TEST_SOURCES)

# glob 兜底(2026-08-28):examples 域的册在主树 configure 偶发漏收
# (同 cmake 同 pattern 的独立项目能收,工头 worktree 也能收;增量与全新
# cache 均现)。显式补齐,漏收时打日志留痕。
list(FIND LUBANCODE_TEST_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/integration/examples/test_gui_agent_example.cpp" _gui_example_idx)
if(_gui_example_idx EQUAL -1)
  list(APPEND LUBANCODE_TEST_SOURCES
       "${CMAKE_CURRENT_SOURCE_DIR}/integration/examples/test_gui_agent_example.cpp")
  message(STATUS "tests: glob 漏收 examples 册,已显式补上")
endif()

if(NOT LUBANCODE_TEST_SOURCES)
  message(FATAL_ERROR "tests/unit 与 tests/integration 下没有测试源码")
endif()

# 根目录不许再堆散装测试；新文件必须先选 unit/integration，再选领域。
file(GLOB LUBANCODE_UNCLASSIFIED_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
if(LUBANCODE_UNCLASSIFIED_TESTS)
  message(FATAL_ERROR
    "发现未分类测试，请移入 unit/<领域> 或 integration/<领域>: "
    "${LUBANCODE_UNCLASSIFIED_TESTS}")
endif()

set(LUBANCODE_TEST_ENTRIES)
set(LUBANCODE_TEST_BASENAMES)
foreach(test_source IN LISTS LUBANCODE_TEST_SOURCES)
  file(RELATIVE_PATH test_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${test_source}")
  string(REPLACE "\\" "/" test_relative "${test_relative}")
  if(NOT test_relative MATCHES
      "^(unit|integration)/([^/]+)/test_([A-Za-z0-9_]+)\\.cpp$")
    message(FATAL_ERROR
      "测试路径不合规，须是 unit|integration/<领域>/test_<名字>.cpp: "
      "${test_relative}")
  endif()

  set(test_kind "${CMAKE_MATCH_1}")
  set(test_domain "${CMAKE_MATCH_2}")
  set(test_stem "${CMAKE_MATCH_3}")
  get_filename_component(test_basename "${test_source}" NAME)

  # doctest 的 --source-file 按文件名筛。重名会让两册一同被拉起，故明令禁止。
  list(FIND LUBANCODE_TEST_BASENAMES "${test_basename}" duplicate_index)
  if(NOT duplicate_index EQUAL -1)
    message(FATAL_ERROR "测试文件重名，无法单册筛选: ${test_basename}")
  endif()
  list(APPEND LUBANCODE_TEST_BASENAMES "${test_basename}")
  list(APPEND LUBANCODE_TEST_ENTRIES
    "${test_kind}|${test_domain}|${test_stem}|${test_basename}")
endforeach()

list(LENGTH LUBANCODE_TEST_SOURCES LUBANCODE_TEST_SOURCE_COUNT)
message(STATUS "lubancode: 收录 ${LUBANCODE_TEST_SOURCE_COUNT} 册测试")

add_executable(lubancode_tests
  support/main.cpp
  support/api_fixture.cpp
  # Lua 受控 HTTP 单·阶段 2:本机回环假 HTTP 服务(§13.3 假服务矩阵)。
  support/fake_http_server.cpp
  # 多渠道消息接入单阶段 1:纯内存 Channel Bridge v1 假 sidecar,供
  # tests/unit/channel 冒烟测试与未来真渠道适配器 conformance suite 复用。
  support/fake_channel_sidecar.cpp
  ${LUBANCODE_TEST_SOURCES}
)
target_link_libraries(lubancode_tests PRIVATE lubancode_app doctest::doctest)
# 批二余款:测试共用的回合事件录音器(Callbacks 显示回调退役,观察点
# 改吃事件流)住 support/。
target_include_directories(lubancode_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
# 依赖守门(显示系统剥离单第八步):测试要现扫源码树,把源码根递进去。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(lubancode_tests PRIVATE cxx_std_23)
target_precompile_headers(lubancode_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/pch.hpp")
# main.cpp 须先定义 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN，再独自包含 doctest；
# 若强塞测试 PCH，定义落在包含之后，入口便不会生成。
set_source_files_properties(support/main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(MSVC)
  target_compile_options(lubancode_tests PRIVATE /MP /FS)
endif()

# M11:test_network_timeout.cpp 手写了个原始 socket 假服务器(模拟连不上/
# 半路断流),要链 Winsock。
if(WIN32)
  target_link_libraries(lubancode_tests PRIVATE ws2_32)
endif()

# M8:真实 Python 夹具(mcp_test_server.py)测试要知道 tests/fixtures/ 的绝对
# 路径——ctest 跑的时候工作目录不一定是 tests/ 源码目录,不能瞎猜相对路径。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  LUBANCODE_TEST_OFFICIAL_SKILLS_DIR="${CMAKE_SOURCE_DIR}/skills"
  LUBANCODE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  # app-server 冒烟(阶段 4):真进程测试要找到 build 树里的主程序。
  LUBANCODE_BINARY_DIR="${CMAKE_BINARY_DIR}")

# M7 起家,plugins 单第 5 步起三平台:真库加载测试的靶子——示例插件
# hello_plugin(直接编 examples/ 里那份源码,顺带验证示例真的能编)+
# abi_tag 填错的 bad_version_plugin。两个库落在同一个输出目录,目录路径
# 喂给测试(LUBANCODE_TEST_PLUGIN_DIR),PluginHost::LoadDirectory 真扫
# 真载。CI 的 windows/ubuntu/macos 三条腿各真编各平台的产物。
add_library(hello_plugin_fixture SHARED
  ${CMAKE_SOURCE_DIR}/examples/plugins/hello_plugin/hello_plugin.c)
target_include_directories(hello_plugin_fixture PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
  set_target_properties(hello_plugin_fixture PROPERTIES OUTPUT_NAME hello_plugin PREFIX "")
endif()

add_library(bad_version_plugin SHARED fixtures/bad_version_plugin.c)
target_include_directories(bad_version_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
  set_target_properties(bad_version_plugin PROPERTIES PREFIX "")
endif()

add_dependencies(lubancode_tests hello_plugin_fixture bad_version_plugin)
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:hello_plugin_fixture>")

# 一册源码一条 CTest。仍只编一只 lubancode_tests，免去重复链接；运行时靠
# doctest 的 source-file 过滤隔离。CI 失败会直报
# integration.process.run_command_process，不再只剩一行 lubancode_tests。
foreach(test_entry IN LISTS LUBANCODE_TEST_ENTRIES)
  string(REPLACE "|" ";" test_fields "${test_entry}")
  list(GET test_fields 0 test_kind)
  list(GET test_fields 1 test_domain)
  list(GET test_fields 2 test_stem)
  list(GET test_fields 3 test_basename)
  set(test_name "${test_kind}.${test_domain}.${test_stem}")

  add_test(NAME "${test_name}"
    COMMAND lubancode_tests "--source-file=*${test_basename}")
  if(test_kind STREQUAL "integration")
    set(test_timeout 300)
  else()
    set(test_timeout 180)
  endif()
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${test_kind};${test_domain};${test_kind}.${test_domain}"
    TIMEOUT "${test_timeout}")
endforeach()

# 工具文案语言档端到端驱动(不进 ctest,验收手动跑):
#   tool_text_lang_driver <zh-CN|en>
# 把试点工具的模型可见文案按所选语言打到 stdout,供与迁移前原文逐字节
# 比对(缺省 zh-CN)与英文档生效验证(en)。
add_executable(tool_text_lang_driver EXCLUDE_FROM_ALL manual/tool_text_lang_driver.cpp)
target_link_libraries(tool_text_lang_driver PRIVATE lubancode_core)
target_compile_features(tool_text_lang_driver PRIVATE cxx_std_23)

# Prompt Profile 单(阶段 2)的黄金 dump 驱动(不进 ctest,验收手动跑):
#   prompt_profile_driver default    未选 Profile 的 default 拼装全文
#   prompt_profile_driver persona    法替换 core 的对照全文
# 基线与改后各跑一遍 default 落盘 diff,即"未选 Profile 时零 diff"的
# 字节证据;测试册 test_prompt_profile.cpp 的重构断言是数学证据。
add_executable(prompt_profile_driver EXCLUDE_FROM_ALL manual/prompt_profile_driver.cpp)
target_link_libraries(prompt_profile_driver PRIVATE lubancode_core)
target_compile_features(prompt_profile_driver PRIVATE cxx_std_23)

# 自定义 Agent 单·阶段 5(Workflow 接入)的冒烟驱动(不进 ctest,验收手动跑):
#   workflow_custom_agent_driver validate <agents_dir> <workflow_dir>
#       真扫 AgentCatalog 走编译期校验(unknown_agent 正反两面)
#   workflow_custom_agent_driver run <agents_dir> <workflow_dir>
#       假后端跑一幕,打印系统提示首行/工具表/节点回执身份(resolved 名)
# 夹具:tests/fixtures/agents/smoke-probe.yaml 与
# tests/fixtures/workflows/custom-agent-smoke/。
add_executable(workflow_custom_agent_driver EXCLUDE_FROM_ALL manual/workflow_custom_agent_driver.cpp)
target_link_libraries(workflow_custom_agent_driver PRIVATE lubancode_core)
target_compile_features(workflow_custom_agent_driver PRIVATE cxx_std_23)

# DeepSeek 真机 e2e(前缀缓存守恒单):opt-in,不进 ctest——没设
# DEEPSEEK_API_KEY 自己就跳过(exit 0),要跑先导钥匙:
#   deepseek_e2e   (可配 DEEPSEEK_MODEL / DEEPSEEK_BASE_URL)
add_executable(deepseek_e2e EXCLUDE_FROM_ALL manual/deepseek_e2e.cpp)
target_link_libraries(deepseek_e2e PRIVATE lubancode_core)
target_compile_features(deepseek_e2e PRIVATE cxx_std_23)

# 任意 provider 的真端点抽验模板(模型怪癖矩阵单,opt-in,不进 ctest):
#   model_probe_e2e
#     MODEL_PROBE_PROVIDER=<catalog id>(必填) + 该 provider 的 key_env
#     可配 MODEL_PROBE_MODEL / MODEL_PROBE_KEY / MODEL_PROBE_BASE_URL /
#     MODEL_PROBE_EFFORT;MODEL_PROBE_STUB=1 离线自测模板自身(无网络)。
add_executable(model_probe_e2e EXCLUDE_FROM_ALL manual/model_probe_e2e.cpp)
target_link_libraries(model_probe_e2e PRIVATE lubancode_core)
target_compile_features(model_probe_e2e PRIVATE cxx_std_23)

# 手工靶子也得有人守门(模型协议兼容实录矩阵单 P0/P3):stub 模式离线自测
# (MODEL_PROBE_STUB=1,进程内假后端,不发一个网络包)进 ctest——模板年久
# 失修(编译断链/事件序坏/usage 落账丢/manifest 报告链路坏)当场红。
# manifest 模式跑全部哨兵骨架:没钥匙全 SKIP(unavailable/unverified),
# 不算 PASS,退出码 0。真机跑法仍是手动 opt-in(带钥匙指环境变量)。
add_dependencies(lubancode_tests model_probe_e2e)
add_test(NAME manual.model_probe_stub COMMAND model_probe_e2e)

# SearchTool 内置 Ripgrep 后端迁移单 P0-1:旧内核照相机(不进 ctest,手动跑):
#   search_golden_driver golden <输出JSON路径>   固化 fixtures/search/corpus/
#       这批夹具在旧内核(std::regex + recursive_directory_iterator)上的
#       行为,格式见驱动源码头注释,供 P0-5 拿新 ripgrep 后端跑同一份场景表
#       做自动化差分。
#   search_golden_driver bench <输出JSON路径> [语料目录=src]
#       旧内核基准(P50/P95、扫描文件数),默认对本仓 src/ 目录跑。
add_executable(search_golden_driver EXCLUDE_FROM_ALL manual/search_golden_driver.cpp)
target_link_libraries(search_golden_driver PRIVATE lubancode_core)
target_compile_features(search_golden_driver PRIVATE cxx_std_23)
target_compile_definitions(search_golden_driver PRIVATE
  LUBANCODE_SEARCH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/search"
  LUBANCODE_SOURCE_ROOT_DIR="${CMAKE_SOURCE_DIR}")

# Browser MCP 自家测试(MCP 富结果单 P1.8):真 node 子进程起 server.js +
# 真 Playwright,本地验收站(127.0.0.1 动态端口)跑核心 E2E/profile/崩溃
# 矩阵。机器上没有 node 就不注册;有 node 没装 playwright/浏览器时 runner
# 逐项打 SKIP 并退 0——真机项如实标 SKIP,不冒充通过。与 C++ 册分开记,
# 报告不混成一句"全过"。
find_program(LUBANCODE_NODE_EXECUTABLE node)
if(LUBANCODE_NODE_EXECUTABLE)
  add_test(NAME browser.mcp.selftest
    COMMAND "${LUBANCODE_NODE_EXECUTABLE}" "browser/test/run-tests.js"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(browser.mcp.selftest PROPERTIES
    LABELS "browser;mcp" TIMEOUT 900
    # 真浏览器起落吃机器:并行 -j 下与其他重测抢资源会抖出假失败,
    # 这条独跑(其余测试照旧并行)。
    RUN_SERIAL TRUE)
endif()
set_tests_properties(manual.model_probe_stub PROPERTIES
  ENVIRONMENT "MODEL_PROBE_STUB=1;MODEL_PROBE_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/manual/model_probe_manifest.json;MODEL_PROBE_REPORT_DIR=${CMAKE_BINARY_DIR}/test-evidence/model-probe-stub"
  LABELS "manual;api;manual.api"
  TIMEOUT 120)

# Kimi 保留式思考真机探针(Kimi 保留式思考历史回传兼容设计单 P2,
# opt-in,EXCLUDE_FROM_ALL 不进 ctest——没钥匙自己 SKIP 退 0):
#   direct  直打 Moonshot 真端点四案(K3 两轮纯对话/K2.6 两步工具/
#           K2.6 keep=all/K2.7-code 两轮)+ GET /models 对账 catalog
#   resume  真 lubancode.exe + 临时 USERPROFILE(只带 moonshot 一个
#           provider)+ 本地录制代理:/exit 后 --continue 续聊,抓第二笔
#           请求体核对 reasoning_content 逐字回传
# 手动跑法(带钥匙指环境变量):
#   python tests/manual/model_probe_kimi_preserved_thinking.py direct
#   python tests/manual/model_probe_kimi_preserved_thinking.py resume
# 钥匙只认 env MOONSHOT_API_KEY 或用户 config,驱动/报告/证据里都不落。
find_program(LUBANCODE_PYTHON_EXECUTABLE python)
if(LUBANCODE_PYTHON_EXECUTABLE)
  add_custom_target(model_probe_kimi_preserved_thinking
    COMMAND "${LUBANCODE_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/manual/model_probe_kimi_preserved_thinking.py" direct
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Kimi preserved thinking live probe (direct; SKIPs without MOONSHOT_API_KEY)"
    EXCLUDE_FROM_ALL VERBATIM)
else()
  message(STATUS "python 未找到: model_probe_kimi_preserved_thinking 探针靶子不注册")
endif()

# 多行字符公式的视觉小样：直连正式渲染器，不进 ctest。
add_executable(latex_box_experiment EXCLUDE_FROM_ALL manual/latex_box_experiment.cpp)
target_link_libraries(latex_box_experiment PRIVATE lubancode_core)
target_compile_features(latex_box_experiment PRIVATE cxx_std_23)

# 可追回 artifact(第二期)的布局实测驱动器:手动跑,验收记录用。
#   artifact_layout_dump <输出根目录>
add_executable(artifact_layout_dump EXCLUDE_FROM_ALL manual/artifact_layout_dump.cpp)
target_link_libraries(artifact_layout_dump PRIVATE lubancode_core)
target_compile_features(artifact_layout_dump PRIVATE cxx_std_23)

# 0.17.0:集成验证用的刮屏驱动器(AllocConsole + WriteConsoleInput +
# ReadConsoleOutput,真控制台专用),不进 ctest——要真钥匙、要弹控制台
# 窗口,只在集成验证时手动跑:
#   screen_driver <lubancode.exe> <子进程工作目录> <报告文件> [--composer-only]
if(WIN32)
  add_executable(screen_driver EXCLUDE_FROM_ALL manual/screen_driver.cpp)
  target_compile_features(screen_driver PRIVATE cxx_std_23)

  # markdown(0.18.x):markdown 重画专用的刮屏驱动器,同样不进 ctest,
  # 集成验证时手动跑:markdown_screen_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(markdown_screen_driver EXCLUDE_FROM_ALL manual/markdown_screen_driver.cpp)
  target_compile_features(markdown_screen_driver PRIVATE cxx_std_23)

  # bracketed paste:整段多行内容折成一枚占位符，提交时才展开。
  add_executable(paste_screen_driver EXCLUDE_FROM_ALL manual/paste_screen_driver.cpp)
  target_compile_features(paste_screen_driver PRIVATE cxx_std_23)

  # 流式脚注框化(0.22.x):验流式期间正文下方那个跟 composer 视觉一致的框
  # (上横线/输入行/下横线/状态行)、排队回显、ESC 打断、可视区域自然滚屏时
  # 框还贴得住,同样不进 ctest,集成验证时手动跑:
  #   stream_footer_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(stream_footer_driver EXCLUDE_FROM_ALL manual/stream_footer_driver.cpp)
  target_compile_features(stream_footer_driver PRIVATE cxx_std_23)
  # 思考流中预览幕的进程内假 SSE 服务要 winsock。
  target_link_libraries(stream_footer_driver PRIVATE ws2_32)

  # 忙碌排队输入框 Tab 补全(0.30.x):验流式期间排队输入框的候选可见、
  # Tab 当场补全/连按轮转带标记、空正文 Tab no-op、轮末队列泵按 slash
  # 分派(不进 SharedEditor 历史)。要真钥匙,不进 ctest,手动跑:
  #   busy_tab_driver <lubancode.exe> <子进程工作目录> <报告文件>
  add_executable(busy_tab_driver EXCLUDE_FROM_ALL manual/busy_tab_driver.cpp)
  target_compile_features(busy_tab_driver PRIVATE cxx_std_23)

  # Esc 取消真机 P50/P95(MiniCPM5-1B 真机巡检单 P1):真 vLLM 长答流式
  # 期间按 Esc,量 Esc→"[已打断]"与 Esc→"Stopped after"两笔时延,轮末
  # 验下一轮可续与半截正文保留。端点用环境变量喂(巡检单"现场配置"节),
  # 不进 ctest,手动跑:
  #   esc_cancel_driver <lubancode.exe> <子进程工作目录> <报告文件> [轮数 1-5]
  add_executable(esc_cancel_driver EXCLUDE_FROM_ALL manual/esc_cancel_driver.cpp)
  target_compile_features(esc_cancel_driver PRIVATE cxx_std_23)

  # /clear、provider 切换清屏、ask_user、diff 残色 + 子代理内层工具展示
  # (重复行/紧凑折叠)专用刮屏驱动器,同样不进 ctest,集成验证时手动跑:
  #   fold_dup_clear_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(fold_dup_clear_driver EXCLUDE_FROM_ALL manual/fold_dup_clear_driver.cpp)
  target_compile_features(fold_dup_clear_driver PRIVATE cxx_std_23)

  # 子代理面板移到输入框上方(0.28.x):验面板在上横线之上、输入框/状态行
  # 钉底、焦点/查看态短标题/两段确认。数据走 LUBANCODE_AGENT_PANEL_DEMO
  # 假代理钩子,不碰网络:
  #   agent_panel_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_panel_driver EXCLUDE_FROM_ALL manual/agent_panel_driver.cpp)
  target_compile_features(agent_panel_driver PRIVATE cxx_std_23)

  # 真前台任务 + 真流式监听(本单新增):进程内起假 anthropic SSE 服务,真
  # lubancode.exe 连上去跑一条带 title 的前台 agent 调用,子代理卡在真工具
  # (ping)里数秒,流式期间逐帧断言代理面板/详情/Esc 分层。工作目录建议给
  # 一个空临时目录(避开项目记忆 identity):
  #   agent_stream_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_stream_driver EXCLUDE_FROM_ALL manual/agent_stream_driver.cpp)
  target_compile_features(agent_stream_driver PRIVATE cxx_std_23)
  target_link_libraries(agent_stream_driver PRIVATE ws2_32)

  # Ctrl+R 提问历史搜索驱动器(实测问题 8:同一条提问列两遍):真控制台
  # 跑真回合真落盘,按真 Ctrl+R 键刮屏断言搜索面板"不重不漏"——两条已落
  # 盘只显两条、未落盘尾巴补上、落盘后不重复、同文两次真发两行都留:
  #   history_search_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(history_search_driver EXCLUDE_FROM_ALL manual/history_search_driver.cpp)
  target_compile_features(history_search_driver PRIVATE cxx_std_23)
  target_link_libraries(history_search_driver PRIVATE ws2_32)

  # 可视区重排驱动器(多智能体真机回归单):真控制台跑"主回合派两只后台
  # 代理随即收口"的完整回合,断言 composer 顶边/代理坞/状态栏全在可视窗
  # 口里(不许靠改字号/滚轮救场)。五幕:80×24、120×35、全屏、改字号三
  # 时机各十轮、滚动缓冲区非底部;每幕把 viewport 快照写进报告。假
  # anthropic SSE 服务供模型,不碰真网络:
  #   viewport_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(viewport_driver EXCLUDE_FROM_ALL manual/viewport_driver.cpp)
  target_compile_features(viewport_driver PRIVATE cxx_std_23)
  target_link_libraries(viewport_driver PRIVATE ws2_32)

  # 流式正文渲染冒烟驱动器(真实实测问题 1:粗体不渲染):进程内假
  # anthropic SSE 服务喂"正文段(含 **粗体**、无空行结尾)后直接跟
  # tool_use"的实测形状 + 逐字滴的收尾段,真控制台刮屏断言让路定格、
  # 增量重画、代码块字面量与转义:
  #   body_render_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(body_render_driver EXCLUDE_FROM_ALL manual/body_render_driver.cpp)
  target_compile_features(body_render_driver PRIVATE cxx_std_23)
  target_link_libraries(body_render_driver PRIVATE ws2_32)
endif()