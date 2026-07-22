# Fetch Qt 6.5.3 for Android (arm64-v8a) via aqtinstall when not already present.
# Host tools (gcc_64) are also required by qt-cmake / androiddeployqt.

set(NYX_QT_ANDROID_VERSION "6.5.3")
set(NYX_QT_ANDROID_ARCH "android_arm64_v8a")
set(NYX_QT_HOST_ARCH "gcc_64")

if(DEFINED ENV{NYX_QT_ANDROID_ROOT} AND EXISTS "$ENV{NYX_QT_ANDROID_ROOT}")
  set(NYX_QT_ANDROID_ROOT "$ENV{NYX_QT_ANDROID_ROOT}" CACHE PATH "Qt Android prefix")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}")
  set(NYX_QT_ANDROID_ROOT "${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}" CACHE PATH "Qt Android prefix")
elseif(EXISTS "$ENV{HOME}/.local/Qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}")
  set(NYX_QT_ANDROID_ROOT "$ENV{HOME}/.local/Qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}" CACHE PATH "Qt Android prefix")
else()
  set(NYX_QT_ANDROID_ROOT "${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_ANDROID_ARCH}" CACHE PATH "Qt Android prefix")
endif()

if(DEFINED ENV{NYX_QT_HOST_ROOT} AND EXISTS "$ENV{NYX_QT_HOST_ROOT}")
  set(NYX_QT_HOST_ROOT "$ENV{NYX_QT_HOST_ROOT}" CACHE PATH "Qt host tools prefix")
elseif(EXISTS "$ENV{HOME}/.local/Qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_HOST_ARCH}")
  set(NYX_QT_HOST_ROOT "$ENV{HOME}/.local/Qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_HOST_ARCH}" CACHE PATH "Qt host tools prefix")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_HOST_ARCH}")
  set(NYX_QT_HOST_ROOT "${CMAKE_SOURCE_DIR}/build/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_HOST_ARCH}" CACHE PATH "Qt host tools prefix")
else()
  set(NYX_QT_HOST_ROOT "${CMAKE_BINARY_DIR}/_qt/${NYX_QT_ANDROID_VERSION}/${NYX_QT_HOST_ARCH}" CACHE PATH "Qt host tools prefix")
endif()

function(_nyx_ensure_aqt)
  find_program(NYX_AQT_EXECUTABLE aqt)
  if(NYX_AQT_EXECUTABLE)
    return()
  endif()
  find_program(NYX_PIP_EXECUTABLE pip3 pip)
  if(NOT NYX_PIP_EXECUTABLE)
    message(FATAL_ERROR "pip3/pip required to install aqtinstall for Qt Android fetch")
  endif()
  execute_process(
    COMMAND ${NYX_PIP_EXECUTABLE} install --user aqtinstall
    RESULT_VARIABLE _aqt_pip_result
  )
  if(NOT _aqt_pip_result EQUAL 0)
    message(FATAL_ERROR "Failed to install aqtinstall")
  endif()
  find_program(NYX_AQT_EXECUTABLE aqt HINTS "$ENV{HOME}/.local/bin")
  if(NOT NYX_AQT_EXECUTABLE)
    message(FATAL_ERROR "aqt not found after pip install")
  endif()
endfunction()

if(NOT EXISTS "${NYX_QT_HOST_ROOT}/bin/qmake")
  message(STATUS "Fetching Qt ${NYX_QT_ANDROID_VERSION} host (${NYX_QT_HOST_ARCH}) into ${NYX_QT_HOST_ROOT}")
  _nyx_ensure_aqt()
  get_filename_component(_nyx_qt_host_parent "${NYX_QT_HOST_ROOT}" DIRECTORY)
  get_filename_component(_nyx_qt_host_parent "${_nyx_qt_host_parent}" DIRECTORY)
  file(MAKE_DIRECTORY "${_nyx_qt_host_parent}")
  execute_process(
    COMMAND ${NYX_AQT_EXECUTABLE} install-qt linux desktop ${NYX_QT_ANDROID_VERSION} ${NYX_QT_HOST_ARCH}
            -O "${_nyx_qt_host_parent}"
            -m qtmultimedia
    RESULT_VARIABLE _nyx_host_qt_result
  )
  if(NOT _nyx_host_qt_result EQUAL 0 OR NOT EXISTS "${NYX_QT_HOST_ROOT}/bin/qmake")
    message(FATAL_ERROR "Failed to fetch Qt host tools")
  endif()
endif()

if(NOT EXISTS "${NYX_QT_ANDROID_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
  message(STATUS "Fetching Qt ${NYX_QT_ANDROID_VERSION} Android (${NYX_QT_ANDROID_ARCH}) into ${NYX_QT_ANDROID_ROOT}")
  _nyx_ensure_aqt()
  get_filename_component(_nyx_qt_android_parent "${NYX_QT_ANDROID_ROOT}" DIRECTORY)
  get_filename_component(_nyx_qt_android_parent "${_nyx_qt_android_parent}" DIRECTORY)
  file(MAKE_DIRECTORY "${_nyx_qt_android_parent}")
  execute_process(
    COMMAND ${NYX_AQT_EXECUTABLE} install-qt linux android ${NYX_QT_ANDROID_VERSION} ${NYX_QT_ANDROID_ARCH}
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
