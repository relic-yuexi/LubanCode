# 提示词模块编译期嵌入:把 src/prompts/{core,features,platforms}/*.md 读进
# 一个生成头文件,每个模块一个 constexpr 字符串常量(raw string,定界符
# LUBAN_MD),core 另给一个按文件名排序的指针数组(拼默认人格用);另生成
# 全模块的 {相对路径, 正文} 总表 kAllModules——运行时化(0.21.x)靠它:
# 播种 ~/.lubancode/prompts/ 用户模块、拼装时逐模块判"用户文件还是内置"。
# 构建期由 add_custom_command 调:
#   cmake -DPROMPTS_DIR=<src/prompts> -DOUTPUT=<hpp> -P embed_prompts.cmake
# 选"构建期生成"而非 configure 期 file(READ):.md 改一字,靠时间戳就能触发
# 重生成 + 重编,不必重新跑 configure。

if(NOT DEFINED PROMPTS_DIR OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "embed_prompts.cmake 需要 -DPROMPTS_DIR=... -DOUTPUT=...")
endif()

set(_header "// 自动生成,别手改——源头是 src/prompts/ 下的 .md 模块,\n")
string(APPEND _header "// 构建时由 cmake/embed_prompts.cmake 重新生成(改 .md 后重新构建即生效)。\n")
string(APPEND _header "#pragma once\n\n")
string(APPEND _header "namespace lubancode::agent::embedded {\n\n")

# 一组模块:group_prefix 是常量名前缀(kCore/kFeature/kPlatform),subdir 是
# src/prompts 下的子目录;array_name 非空时,额外生成该组的指针数组。
function(embed_group group_prefix subdir array_name)
  file(GLOB _files "${PROMPTS_DIR}/${subdir}/*.md")
  list(SORT _files)
  set(_names "")
  foreach(_file IN LISTS _files)
    get_filename_component(_stem "${_file}" NAME_WE)
    if(_stem STREQUAL "README")
      continue()
    endif()
    file(READ "${_file}" _content)
    string(REPLACE "\r\n" "\n" _content "${_content}")
    string(STRIP "${_content}" _content)
    if(_content STREQUAL "")
      message(FATAL_ERROR "提示词模块是空的: ${_file}")
    endif()
    if(_content MATCHES "\\)LUBAN_MD\"")
      message(FATAL_ERROR "提示词模块正文撞上了 raw string 定界符 )LUBAN_MD\": ${_file}")
    endif()
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _ident "${_stem}")
    set(_name "${group_prefix}_${_ident}")
    list(APPEND _names "${_name}")
    string(APPEND _local "// ${subdir}/${_stem}.md\n")
    string(APPEND _local "inline constexpr const char ${_name}[] = R\"LUBAN_MD(${_content})LUBAN_MD\";\n\n")
    string(APPEND _local_entries "    {\"${subdir}/${_stem}.md\", ${_name}},\n")
  endforeach()
  if(NOT _names)
    message(FATAL_ERROR "目录里一个可嵌入的 .md 都没有: ${PROMPTS_DIR}/${subdir}")
  endif()
  if(NOT array_name STREQUAL "")
    string(APPEND _local "// ${subdir}/ 全部模块,按文件名排序\n")
    string(REPLACE ";" ", " _joined "${_names}")
    string(APPEND _local "inline constexpr const char* ${array_name}[] = {${_joined}};\n\n")
  endif()
  set(_body "${_body}${_local}" PARENT_SCOPE)
  set(_entries "${_entries}${_local_entries}" PARENT_SCOPE)
endfunction()

set(_body "")
set(_entries "")
embed_group(kCore core kCoreModules)
embed_group(kFeature features "")
embed_group(kPlatform platforms "")

string(APPEND _header "${_body}")
string(APPEND _header "// 全部模块的 {相对路径, 嵌入正文} 总表,分组内按文件名排序、组序\n")
string(APPEND _header "// core -> features -> platforms。播种用户目录模块、运行时逐模块判\n")
string(APPEND _header "// \"用户文件还是内置\"都以这张表为准。\n")
string(APPEND _header "struct EmbeddedModule {\n")
string(APPEND _header "    const char* rel_path;\n")
string(APPEND _header "    const char* content;\n")
string(APPEND _header "};\n")
string(APPEND _header "inline constexpr EmbeddedModule kAllModules[] = {\n${_entries}};\n\n")
string(APPEND _header "}  // namespace lubancode::agent::embedded\n")

# 内容没变就不动文件(时间戳不变,免得下游无谓重编)。
set(_old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _header)
  file(WRITE "${OUTPUT}" "${_header}")
endif()
