# The installed target has only the public header tree in its usage requirements.
# Internal archives and third-party headers remain private to this shared library.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(lubancore_sdk SHARED
  src/sdk/core.cpp
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
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LubanCore
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LubanCore
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LubanCore)
if(WIN32)
  install(FILES $<TARGET_RUNTIME_DLLS:lubancore_sdk>
    DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LubanCore)
endif()
install(DIRECTORY include/lubancore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT LubanCore)
install(FILES docs/development/lubancore-sdk.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore COMPONENT LubanCore)
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
