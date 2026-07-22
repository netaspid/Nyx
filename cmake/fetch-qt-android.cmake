# Resolve Qt Android kit via aqtinstall, or reuse an existing install.

set(NYX_QT_ANDROID_VERSION "6.5.3")
set(NYX_QT_ANDROID_ARCH "android_arm64_v8a")

if(CMAKE_HOST_WIN32)
  set(_nyx_qt_host_arch_candidates mingw_64 win64_mingw msvc2019_64 win64_msvc2019_64 msvc2022_64)
  set(_nyx_aqt_host windows)
  set(_nyx_aqt_host_arch_install win64_mingw)
  if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
    set(_nyx_user_qt "$ENV{LOCALAPPDATA}/Qt")
  else()
    set(_nyx_user_qt "$ENV{USERPROFILE}/AppData/Local/Qt")
  endif()
  set(_nyx_qmake_name qmake.exe)
elseif(CMAKE_HOST_APPLE)
  set(_nyx_qt_host_arch_candidates clang_64 macos)
  set(_nyx_aqt_host mac)
  set(_nyx_aqt_host_arch_install clang_64)
  set(_nyx_user_qt "$ENV{HOME}/Qt")
  set(_nyx_qmake_name qmake)
else()
  set(_nyx_qt_host_arch_candidates gcc_64 linux_gcc_64)
  set(_nyx_aqt_host linux)
  set(_nyx_aqt_host_arch_install gcc_64)
  set(_nyx_user_qt "$ENV{HOME}/.local/Qt")
  set(_nyx_qmake_name qmake)
endif()

function(_nyx_first_existing_dir out_var)
  foreach(_p IN LISTS ARGN)
    if(_p AND EXISTS "${_p}")
      set(${out_var} "${_p}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(_nyx_find_qt_host_under root out_var)
  set(_found "")
  if(root AND EXISTS "${root}")
    foreach(_arch IN LISTS _nyx_qt_host_arch_candidates)
      if(EXISTS "${root}/${_arch}/bin/${_nyx_qmake_name}")
        set(_found "${root}/${_arch}")
        break()
      endif()
    endforeach()
    # Flat prefix that already points at host kit
    if(NOT _found AND EXISTS "${root}/bin/${_nyx_qmake_name}")
      set(_found "${root}")
    endif()
  endif()
  set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# --- Android kit ---
if(NYX_QT_ANDROID_ROOT AND EXISTS "${NYX_QT_ANDROID_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
elseif(DEFINED ENV{NYX_QT_ANDROID_ROOT} AND EXISTS "$ENV{NYX_QT_ANDROID_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
  set(NYX_QT_ANDROID_ROOT "$ENV{NYX_QT_ANDROID_ROOT}" CACHE PATH "Qt Android prefix" FORCE)
else()
  _nyx_first_existing_dir(_nyx_android_found
    "${_nyx_user_qt}/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}"
    "${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}"
    "${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}"
  )
  if(_nyx_android_found)
    set(NYX_QT_ANDROID_ROOT "${_nyx_android_found}" CACHE PATH "Qt Android prefix" FORCE)
  else()
    set(NYX_QT_ANDROID_ROOT "${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}" CACHE PATH "Qt Android prefix")
  endif()
endif()

# --- Host tools ---
set(_nyx_host_found "")
if(NYX_QT_HOST_ROOT AND EXISTS "${NYX_QT_HOST_ROOT}/bin/${_nyx_qmake_name}")
  set(_nyx_host_found "${NYX_QT_HOST_ROOT}")
elseif(DEFINED ENV{NYX_QT_HOST_ROOT} AND EXISTS "$ENV{NYX_QT_HOST_ROOT}/bin/${_nyx_qmake_name}")
  set(_nyx_host_found "$ENV{NYX_QT_HOST_ROOT}")
elseif(DEFINED ENV{QT_HOST_PATH} AND EXISTS "$ENV{QT_HOST_PATH}/bin/${_nyx_qmake_name}")
  set(_nyx_host_found "$ENV{QT_HOST_PATH}")
elseif(QT_HOST_PATH AND EXISTS "${QT_HOST_PATH}/bin/${_nyx_qmake_name}")
  set(_nyx_host_found "${QT_HOST_PATH}")
endif()
if(NOT _nyx_host_found)
  _nyx_find_qt_host_under("${_nyx_user_qt}/${NYX_QT_ANDROID_VERSION}" _nyx_host_found)
endif()
if(NOT _nyx_host_found)
  _nyx_find_qt_host_under("${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}" _nyx_host_found)
endif()
if(NOT _nyx_host_found)
  _nyx_find_qt_host_under("${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}" _nyx_host_found)
endif()

if(_nyx_host_found)
  set(NYX_QT_HOST_ROOT "${_nyx_host_found}" CACHE PATH "Qt host tools prefix" FORCE)
else()
  list(GET _nyx_qt_host_arch_candidates 0 _nyx_default_host_arch)
  set(NYX_QT_HOST_ROOT "${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}/${_nyx_default_host_arch}" CACHE PATH "Qt host tools prefix")
endif()

function(_nyx_ensure_aqt)
  if(NYX_AQT_COMMAND)
    return()
  endif()
  find_program(NYX_AQT_EXECUTABLE NAMES aqt aqt.exe)
  if(NYX_AQT_EXECUTABLE)
    set(NYX_AQT_COMMAND "${NYX_AQT_EXECUTABLE}" PARENT_SCOPE)
    return()
  endif()

  find_program(NYX_PYTHON_EXECUTABLE NAMES python3 python py
    HINTS
      "$ENV{LOCALAPPDATA}/Programs/Python/Python312"
      "$ENV{LOCALAPPDATA}/Programs/Python/Python313"
      "/usr/bin"
  )
  find_program(NYX_PIP_EXECUTABLE NAMES pip3 pip)

  if(NYX_PYTHON_EXECUTABLE)
    execute_process(
      COMMAND ${NYX_PYTHON_EXECUTABLE} -m aqt version
      RESULT_VARIABLE _aqt_mod_ok
      OUTPUT_QUIET ERROR_QUIET
    )
    if(_aqt_mod_ok EQUAL 0)
      set(NYX_AQT_COMMAND "${NYX_PYTHON_EXECUTABLE};-m;aqt" PARENT_SCOPE)
      return()
    endif()
    execute_process(
      COMMAND ${NYX_PYTHON_EXECUTABLE} -m pip install --user aqtinstall
      RESULT_VARIABLE _aqt_pip_result
    )
    if(NOT _aqt_pip_result EQUAL 0)
      message(FATAL_ERROR "Failed to install aqtinstall via python -m pip")
    endif()
    set(NYX_AQT_COMMAND "${NYX_PYTHON_EXECUTABLE};-m;aqt" PARENT_SCOPE)
    return()
  endif()

  if(NOT NYX_PIP_EXECUTABLE)
    message(FATAL_ERROR "python/pip required to install aqtinstall for Qt Android fetch")
  endif()
  execute_process(
    COMMAND ${NYX_PIP_EXECUTABLE} install --user aqtinstall
    RESULT_VARIABLE _aqt_pip_result
  )
  if(NOT _aqt_pip_result EQUAL 0)
    message(FATAL_ERROR "Failed to install aqtinstall")
  endif()
  find_program(NYX_AQT_EXECUTABLE NAMES aqt aqt.exe
    HINTS
      "$ENV{APPDATA}/Python/Python312/Scripts"
      "$ENV{APPDATA}/Python/Python313/Scripts"
      "$ENV{HOME}/.local/bin"
  )
  if(NYX_AQT_EXECUTABLE)
    set(NYX_AQT_COMMAND "${NYX_AQT_EXECUTABLE}" PARENT_SCOPE)
    return()
  endif()
  message(FATAL_ERROR "aqt not found after pip install (try: python -m pip install --user aqtinstall)")
endfunction()

if(NOT EXISTS "${NYX_QT_HOST_ROOT}/bin/${_nyx_qmake_name}")
  message(STATUS "Fetching Qt ${NYX_QT_ANDROID_VERSION} host (${_nyx_aqt_host_arch_install}) into ${NYX_QT_HOST_ROOT}")
  _nyx_ensure_aqt()
  get_filename_component(_nyx_qt_host_parent "${NYX_QT_HOST_ROOT}" DIRECTORY)
  get_filename_component(_nyx_qt_host_parent "${_nyx_qt_host_parent}" DIRECTORY)
  file(MAKE_DIRECTORY "${_nyx_qt_host_parent}")
  execute_process(
    COMMAND ${NYX_AQT_COMMAND} install-qt ${_nyx_aqt_host} desktop ${NYX_QT_ANDROID_VERSION} ${_nyx_aqt_host_arch_install}
            -O "${_nyx_qt_host_parent}"
            -m qtmultimedia
    RESULT_VARIABLE _nyx_host_qt_result
  )
  _nyx_find_qt_host_under("${_nyx_qt_host_parent}/${NYX_QT_ANDROID_VERSION}" _nyx_host_after)
  if(_nyx_host_after)
    set(NYX_QT_HOST_ROOT "${_nyx_host_after}" CACHE PATH "Qt host tools prefix" FORCE)
  endif()
  if(NOT _nyx_host_qt_result EQUAL 0 OR NOT EXISTS "${NYX_QT_HOST_ROOT}/bin/${_nyx_qmake_name}")
    message(FATAL_ERROR "Failed to fetch Qt host tools (looked for ${_nyx_qmake_name} under ${NYX_QT_HOST_ROOT})")
  endif()
endif()

if(NOT EXISTS "${NYX_QT_ANDROID_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
  message(STATUS "Fetching Qt ${NYX_QT_ANDROID_VERSION} Android (${NYX_QT_ANDROID_ARCH}) into ${NYX_QT_ANDROID_ROOT}")
  _nyx_ensure_aqt()
  get_filename_component(_nyx_qt_android_parent "${NYX_QT_ANDROID_ROOT}" DIRECTORY)
  get_filename_component(_nyx_qt_android_parent "${_nyx_qt_android_parent}" DIRECTORY)
  file(MAKE_DIRECTORY "${_nyx_qt_android_parent}")
  execute_process(
    COMMAND ${NYX_AQT_COMMAND} install-qt ${_nyx_aqt_host} android ${NYX_QT_ANDROID_VERSION} ${NYX_QT_ANDROID_ARCH}
            -O "${_nyx_qt_android_parent}"
            -m qtmultimedia
            --autodesktop
    RESULT_VARIABLE _nyx_android_qt_result
  )
  if(NOT _nyx_android_qt_result EQUAL 0 OR NOT EXISTS "${NYX_QT_ANDROID_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR "Failed to fetch Qt Android")
  endif()
endif()

list(PREPEND CMAKE_PREFIX_PATH "${NYX_QT_ANDROID_ROOT}")
set(QT_HOST_PATH "${NYX_QT_HOST_ROOT}" CACHE PATH "Qt host path for androiddeployqt" FORCE)
message(STATUS "Qt Android: ${NYX_QT_ANDROID_ROOT}")
message(STATUS "Qt host:    ${NYX_QT_HOST_ROOT}")
