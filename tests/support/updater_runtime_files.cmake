cmake_minimum_required(VERSION 3.21)

foreach(required UPDATER_PROBE_EXE UPDATER_RUNTIME_LIST
                 CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM
                 CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL
                 CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Updater fixture dependency scan requires ${required}")
  endif()
endforeach()
if(NOT EXISTS "${UPDATER_PROBE_EXE}" OR "$ENV{SystemRoot}" STREQUAL "")
  message(FATAL_ERROR "Updater fixture requires a built EXE and SystemRoot")
endif()

file(REAL_PATH "${UPDATER_PROBE_EXE}" probe_exe)
get_filename_component(probe_dir "${probe_exe}" DIRECTORY)
file(REAL_PATH "$ENV{SystemRoot}" system_root)
string(REPLACE "\\" "/" system_root "${system_root}")
string(TOLOWER "${system_root}" system_root_lower)

# Unlike TARGET_RUNTIME_DLLS, the PE scan includes indirect imports such as z.dll.
# No producer directory is added to the test process's PATH.
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${probe_exe}"
  DIRECTORIES "${probe_dir}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  CONFLICTING_DEPENDENCIES_PREFIX conflicts
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-")
if(unresolved OR conflicts_FILENAMES)
  message(FATAL_ERROR
    "Updater fixture has unresolved or conflicting PE imports: ${unresolved};${conflicts_FILENAMES}")
endif()

set(runtime_files)
foreach(dependency IN LISTS resolved)
  file(REAL_PATH "${dependency}" dependency_real)
  string(REPLACE "\\" "/" dependency_real "${dependency_real}")
  string(TOLOWER "${dependency_real}" dependency_lower)
  cmake_path(IS_PREFIX system_root_lower "${dependency_lower}" NORMALIZE is_system)
  if(is_system)
    continue()
  endif()
  get_filename_component(name "${dependency_real}" NAME)
  get_filename_component(extension "${name}" LAST_EXT)
  string(TOLOWER "${extension}" extension)
  if(NOT extension STREQUAL ".dll" OR NOT EXISTS "${probe_dir}/${name}")
    message(FATAL_ERROR "Updater fixture dependency is not an app-local DLL: ${dependency_real}")
  endif()
  file(REAL_PATH "${probe_dir}/${name}" local_real)
  string(REPLACE "\\" "/" local_real "${local_real}")
  string(TOLOWER "${local_real}" local_lower)
  if(NOT local_lower STREQUAL dependency_lower)
    message(FATAL_ERROR "Updater fixture dependency resolved outside the EXE directory: ${dependency_real}")
  endif()
  list(APPEND runtime_files "${name}")
endforeach()
list(REMOVE_DUPLICATES runtime_files)
list(SORT runtime_files)
file(WRITE "${UPDATER_RUNTIME_LIST}" "")
foreach(name IN LISTS runtime_files)
  file(APPEND "${UPDATER_RUNTIME_LIST}" "${name}\n")
endforeach()
message(STATUS "Updater fixture PE dependencies: ${probe_exe} -> ${runtime_files}")
