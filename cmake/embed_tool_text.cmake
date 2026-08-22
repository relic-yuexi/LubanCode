# 工具文案(模型可见:工具描述 + schema 参数说明)按语言分档编译期嵌入。
# 源头是 src/prompts/tools/<语言码>/<工具名>.md,每个文件内部按"## 节名"
# 分键(## description 一节,## param.<参数路径> 每参数一节)。构建期生成
# <build>/generated/embedded_tool_text.hpp:
#   namespace lubancode::tools::embedded_text {
#     inline constexpr const char k<语言>_<工具>__<键>[] = R"LUBAN_TT(...)LUBAN_TT";
#     struct EmbeddedToolKey { const char* lang; const char* tool; const char* key; const char* text; };
#     inline constexpr EmbeddedToolKey kAllKeys[] = {...};
#   }
# 运行时 src/tools/tool_text.cpp 首次用时装载成全局查表,回退链
# 当前语言 → zh-CN → 调用方兜底(C++ 原文案,迁移期)。
# 与 embed_prompts.cmake 同一套"构建期生成"的路子:.md 改一字,时间戳触发
# 重生成 + 重编,不必重跑 configure;GLOB CONFIGURE_DEPENDS 管增删文件。
#
# 注意:正文可能含 ASCII 分号(英文文案里就有),CMake 的 list 恰用分号做
# 元素分隔——按行劈表会把分号吃掉。所以这里全程用 string(FIND/SUBSTRING)
# 逐行走原文,一个 list 都不建(目录/文件名列表没有分号,不受此限)。
#
# 调用:cmake -DTOOLS_DIR=<src/prompts/tools> -DOUTPUT=<hpp> -P embed_tool_text.cmake

if(NOT DEFINED TOOLS_DIR OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "embed_tool_text.cmake 需要 -DTOOLS_DIR=... -DOUTPUT=...")
endif()

set(_header "// 自动生成,别手改——源头是 src/prompts/tools/<语言>/<工具>.md,\n")
string(APPEND _header "// 构建时由 cmake/embed_tool_text.cmake 重新生成(改 .md 后重新构建即生效)。\n")
string(APPEND _header "#pragma once\n\n")
string(APPEND _header "namespace lubancode::tools::embedded_text {\n\n")
string(APPEND _header "struct EmbeddedToolKey {\n")
string(APPEND _header "    const char* lang;\n")
string(APPEND _header "    const char* tool;\n")
string(APPEND _header "    const char* key;\n")
string(APPEND _header "    const char* text;\n};\n\n")

# 把(key, body)落成一对生成物:一个 constexpr 常量 + 一条 kAllKeys 记录。
# 结果累积进调用方的 _local / _entries(普通字符串变量,分号安全)。
function(_flush_key lang tool stem key body_var)
  string(STRIP "${${body_var}}" _body)
  if(_body STREQUAL "")
    message(FATAL_ERROR "工具文案节是空的: ${lang}/${tool}.md 的 ## ${key}")
  endif()
  if(_body MATCHES "\\)LUBAN_TT\"")
    message(FATAL_ERROR "工具文案正文撞上了 raw string 定界符 \\)LUBAN_TT\": ${lang}/${tool}.md 的 ## ${key}")
  endif()
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" _ident "${key}")
  set(_const "${stem}__${_ident}")
  string(APPEND _local "inline constexpr const char ${_const}[] = R\"LUBAN_TT(${_body})LUBAN_TT\";\n")
  string(APPEND _entries "    {\"${lang}\", \"${tool}\", \"${key}\", ${_const}},\n")
  set(_local "${_local}" PARENT_SCOPE)
  set(_entries "${_entries}" PARENT_SCOPE)
endfunction()

# 解析一个工具 .md:文件 = 若干"## <键>"节,节体到下一个 ## 或文件尾。
# ## 之前的内容忽略(文件头说明性文字)。逐行走,不用 list。_flush_key 在
# 本函数栈帧里累积 _local/_entries,收尾整体递回顶层。
function(_embed_tool_file lang tool stem file)
  file(READ "${file}" _content)
  string(REPLACE "\r\n" "\n" _content "${_content}")
  set(_cur_key "")
  set(_cur_body "")
  set(_remaining "${_content}")
  while(NOT _remaining STREQUAL "")
    string(FIND "${_remaining}" "\n" _nl)
    if(_nl EQUAL -1)
      set(_line "${_remaining}")
      set(_remaining "")
    else()
      string(SUBSTRING "${_remaining}" 0 "${_nl}" _line)
      math(EXPR _next "${_nl} + 1")
      string(SUBSTRING "${_remaining}" "${_next}" "-1" _remaining)
    endif()
    if(_line MATCHES "^##[ \\t]+([A-Za-z0-9_.]+)[ \\t]*$")
      if(NOT _cur_key STREQUAL "")
        _flush_key("${lang}" "${tool}" "${stem}" "${_cur_key}" _cur_body)
      endif()
      set(_cur_key "${CMAKE_MATCH_1}")
      set(_cur_body "")
    elseif(NOT _cur_key STREQUAL "")
      set(_cur_body "${_cur_body}${_line}\n")
    endif()
  endwhile()
  if(NOT _cur_key STREQUAL "")
    _flush_key("${lang}" "${tool}" "${stem}" "${_cur_key}" _cur_body)
  endif()
  # _flush_key 的 PARENT_SCOPE 只到本函数栈帧;再递一层到顶层。
  set(_local "${_local}" PARENT_SCOPE)
  set(_entries "${_entries}" PARENT_SCOPE)
endfunction()

set(_local "")
set(_entries "")
file(GLOB _lang_dirs LIST_DIRECTORIES true "${TOOLS_DIR}/*")
list(SORT _lang_dirs)
foreach(_lang_dir IN LISTS _lang_dirs)
  if(NOT IS_DIRECTORY "${_lang_dir}")
    continue()
  endif()
  get_filename_component(_lang "${_lang_dir}" NAME)
  file(GLOB _files "${_lang_dir}/*.md")
  list(SORT _files)
  foreach(_file IN LISTS _files)
    get_filename_component(_stem "${_file}" NAME_WE)
    if(_stem STREQUAL "README")
      continue()
    endif()
    # 常量名:k<语言>_<工具>;语言码里的非字母数字(如 zh-CN 的 '-')也
    # 归一成下划线。
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _lang_ident "${_lang}")
    _embed_tool_file("${_lang}" "${_stem}" "k${_lang_ident}_${_stem}" "${_file}")
  endforeach()
endforeach()

string(APPEND _header "${_local}\n")
string(APPEND _header "// 全部 {语言, 工具名, 键, 正文} 总表,语言目录序、目录内按文件名序。\n")
string(APPEND _header "// 运行时装进 tool_text 的全局查表(键 = 工具 + 键 + 语言)。\n")
string(APPEND _header "inline constexpr EmbeddedToolKey kAllKeys[] = {\n${_entries}};\n\n")
string(APPEND _header "}  // namespace lubancode::tools::embedded_text\n")

# 内容没变就不动文件(时间戳不变,免得下游无谓重编)。
set(_old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _header)
  file(WRITE "${OUTPUT}" "${_header}")
endif()
