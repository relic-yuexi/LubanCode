# The installed target has only the public header tree in its usage requirements.
# Internal archives and third-party headers remain private to this shared library.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(lubancore_sdk SHARED
  src/sdk/core.cpp
  src/sdk/event_queue.cpp
  src/sdk/approval.cpp
  src/sdk/operation_ledger.cpp
  src/sdk/skills.cpp
  src/sdk/subagents.cpp
  src/sdk/memory.cpp
  src/sdk/memory_blobs.cpp
  src/sdk/memory_write.cpp
  src/sdk/packages.cpp
  src/sdk/lua.cpp
  src/sdk/extensions.cpp
  src/sdk/action_opening.cpp
  src/sdk/action_dispatch.cpp
  src/sdk/results.cpp
  src/sdk/result_projection.cpp
  src/sdk/adapters.cpp)
add_library(LubanCore::Core ALIAS lubancore_sdk)
set_target_properties(lubancore_sdk PROPERTIES
  EXPORT_NAME Core
  OUTPUT_NAME lubancore
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(lubancore_sdk PUBLIC cxx_std_23)
target_compile_definitions(lubancore_sdk PRIVATE
  LUBANCORE_BUILDING
  LUBANCORE_VERSION="${PROJECT_VERSION}")
target_include_directories(lubancore_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(lubancore_sdk PRIVATE lubancode_runtime)
if(UNIX AND NOT APPLE)
  # Static internals must not become a second accidental public ABI.
  target_link_options(lubancore_sdk PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
endif()
if(APPLE)
  set_target_properties(lubancore_sdk PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(lubancore_sdk PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

if(MSVC)
  # cpr can override the CRT for its static target. All build-tree targets that
  # share C++ objects with this DLL must use the same dynamic CRT. SDK-off release
  # builds keep the historical fully static CRT choice.
  function(lubancore_set_shared_crt directory)
    get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS local_targets)
      get_target_property(kind "${target}" TYPE)
      if(NOT kind STREQUAL "INTERFACE_LIBRARY" AND NOT kind STREQUAL "UTILITY")
        set_property(TARGET "${target}" PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
      endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
      lubancore_set_shared_crt("${child}")
    endforeach()
  endfunction()
  lubancore_set_shared_crt("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

install(TARGETS lubancore_sdk EXPORT LubanCoreTargets
  RUNTIME_DEPENDENCY_SET LubanCoreRuntimeDependencies
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LubanCore
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LubanCore
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LubanCore)
if(WIN32)
  # Imported targets do not always expose their complete DLL dependency graph:
  # vcpkg's libcurl target, for example, can omit its indirect z.dll dependency.
  # Scan the actual PE imports after the build has populated the app-local DLL
  # directory. Missing non-system dependencies fail installation explicitly.
  # PE import names are normalized, but resolved paths can preserve case and
  # either separator. Match the actual Windows root without excluding similarly
  # named directories elsewhere in the SDK or a dependency installation.
  set(_lubancore_windows_root "$ENV{SystemRoot}")
  if(_lubancore_windows_root STREQUAL "")
    message(FATAL_ERROR "SystemRoot is required to classify Windows SDK runtime dependencies")
  endif()
  string(REPLACE "\\" "/" _lubancore_windows_root "${_lubancore_windows_root}")
  string(REGEX MATCHALL "." _lubancore_windows_root_chars "${_lubancore_windows_root}")
  set(_lubancore_windows_root_regex "^")
  foreach(_char IN LISTS _lubancore_windows_root_chars)
    if(_char MATCHES "[A-Za-z]")
      string(TOLOWER "${_char}" _lower)
      string(TOUPPER "${_char}" _upper)
      string(APPEND _lubancore_windows_root_regex "[${_lower}${_upper}]")
    elseif(_char STREQUAL "/" OR _char STREQUAL "\\")
      string(APPEND _lubancore_windows_root_regex [=[[/\\]]=])
    elseif(_char MATCHES "[0-9]" OR _char STREQUAL ":" OR _char STREQUAL " " OR
           _char STREQUAL "_" OR _char STREQUAL "-")
      string(APPEND _lubancore_windows_root_regex "${_char}")
    else()
      string(APPEND _lubancore_windows_root_regex "\\${_char}")
    endif()
  endforeach()
  string(APPEND _lubancore_windows_root_regex [=[[/\\]]=])
  install(RUNTIME_DEPENDENCY_SET LubanCoreRuntimeDependencies
    DIRECTORIES "$<TARGET_FILE_DIR:lubancore_sdk>"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "${_lubancore_windows_root_regex}"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LubanCore
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LubanCore)
endif()
install(DIRECTORY include/lubancore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT LubanCore)
install(FILES docs/development/lubancore-sdk.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore COMPONENT LubanCore)
# Search consumes an explicitly staged, manifest-pinned backend. SDK-only does
# not discover HOME/PATH resources or download anything during configuration.
# Without a stage the SDK remains usable for its other tools; selecting search
# then fails its exact resource-root preflight.
if(NOT LUBANCODE_BUNDLED_RG_DIR STREQUAL "")
  if(WIN32)
    set(_lubancore_rg_name rg.exe)
  else()
    set(_lubancore_rg_name rg)
  endif()
  set(_lubancore_rg "${LUBANCODE_BUNDLED_RG_DIR}/${_lubancore_rg_name}")
  if(NOT EXISTS "${_lubancore_rg}" OR IS_DIRECTORY "${_lubancore_rg}")
    message(FATAL_ERROR "SDK search resource is missing: ${_lubancore_rg}")
  endif()
  install(PROGRAMS "${_lubancore_rg}"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore/libexec COMPONENT LubanCore)
  install(FILES third_party/ripgrep/LICENSE-MIT
    DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore/licenses/ripgrep COMPONENT LubanCore)
  install(FILES third_party/ripgrep/manifest.json RENAME ripgrep-manifest.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore COMPONENT LubanCore)
endif()
install(EXPORT LubanCoreTargets NAMESPACE LubanCore::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LubanCore COMPONENT LubanCore)
configure_package_config_file(cmake/LubanCoreConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/LubanCoreConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LubanCore)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/LubanCoreConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LubanCoreConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/LubanCoreConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LubanCore COMPONENT LubanCore)
