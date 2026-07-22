set(_SRC_PREFIX "${SRC_PREFIX}")
set(_DST_DIR "${DST_DIR}")

if(NOT _SRC_PREFIX)
  message(FATAL_ERROR "copy-qt-runtime-linux.cmake: SRC_PREFIX is required")
endif()
if(NOT _DST_DIR)
  message(FATAL_ERROR "copy-qt-runtime-linux.cmake: DST_DIR is required")
endif()

file(MAKE_DIRECTORY "${_DST_DIR}")

function(_nyx_copy_file _src _dst_dir)
  get_filename_component(_base "${_src}" NAME)
  set(_dest "${_dst_dir}/${_base}")
  if(EXISTS "${_dest}" AND NOT IS_SYMLINK "${_dest}")
    return()
  endif()
  if(IS_SYMLINK "${_dest}")
    file(REMOVE "${_dest}")
  endif()
  execute_process(
    COMMAND cp -L "${_src}" "${_dest}"
    RESULT_VARIABLE _cp_result
  )
  if(NOT _cp_result EQUAL 0)
    message(FATAL_ERROR "copy failed: ${_src} -> ${_dest}")
  endif()
endfunction()

if(EXISTS "${_SRC_PREFIX}/qml")
  file(COPY "${_SRC_PREFIX}/qml" DESTINATION "${_DST_DIR}")
endif()

file(GLOB _qt_libs "${_SRC_PREFIX}/lib/*.so*")
foreach(_lib IN LISTS _qt_libs)
  _nyx_copy_file("${_lib}" "${_DST_DIR}")
endforeach()

set(_plugin_dirs
  platforms
  xcbglintegrations
  tls
  styles
  imageformats
  iconengines
  generic
  networkinformation
  sqldrivers
  multimedia
)

foreach(_d IN LISTS _plugin_dirs)
  if(EXISTS "${_SRC_PREFIX}/plugins/${_d}")
    file(COPY "${_SRC_PREFIX}/plugins/${_d}" DESTINATION "${_DST_DIR}")
  endif()
endforeach()

if(EXISTS "${_SRC_PREFIX}/translations")
  file(COPY "${_SRC_PREFIX}/translations" DESTINATION "${_DST_DIR}")
endif()

# Keep host OpenGL stack (GLX/EGL/Mesa/NVIDIA). Bundling these breaks
# "neither GLX nor EGL are enabled" at runtime under LD_LIBRARY_PATH.
set(_nyx_excluded_deps
  libc.so.6
  libm.so.6
  libpthread.so.0
  librt.so.1
  libdl.so.2
  ld-linux-x86-64.so.2
  linux-vdso.so.1
  libGL.so.1
  libGL.so
  libOpenGL.so.0
  libOpenGL.so
  libEGL.so.1
  libEGL.so
  libGLdispatch.so.0
  libGLdispatch.so
  libGLX.so.0
  libGLX.so
  libGLX_mesa.so.0
  libGLX_nvidia.so.0
  libEGL_mesa.so.0
  libEGL_nvidia.so.0
  libdrm.so.2
  libgbm.so.1
)

function(_nyx_is_host_gl_lib _name _out)
  if(_name MATCHES "^lib(GL|OpenGL|EGL|GLdispatch|GLX|drm|gbm)([._].*)?$")
    set(${_out} TRUE PARENT_SCOPE)
  else()
    set(${_out} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(_nyx_bundle_ldd_deps _root)
  file(GLOB_RECURSE _targets
    "${_root}/*.so"
    "${_root}/*.so.*"
    "${_root}/platforms/*.so"
    "${_root}/multimedia/*.so"
  )
  set(_copied "")
  foreach(_round RANGE 1 8)
    set(_added FALSE)
    foreach(_t IN LISTS _targets)
      if(NOT EXISTS "${_t}")
        continue()
      endif()
      execute_process(
        COMMAND ldd "${_t}"
        OUTPUT_VARIABLE _nyx_ldd_out
        ERROR_VARIABLE _nyx_ldd_out
      )
      string(REPLACE "\n" ";" _nyx_ldd_lines "${_nyx_ldd_out}")
      foreach(_line IN LISTS _nyx_ldd_lines)
        if(NOT _line MATCHES "=>[ \t]+([^ \t]+)")
          continue()
        endif()
        set(_dep_path "${CMAKE_MATCH_1}")
        if(NOT EXISTS "${_dep_path}")
          continue()
        endif()
        get_filename_component(_dep_base "${_dep_path}" NAME)
        list(FIND _nyx_excluded_deps "${_dep_base}" _excluded_idx)
        if(NOT _excluded_idx EQUAL -1)
          continue()
        endif()
        _nyx_is_host_gl_lib("${_dep_base}" _is_gl)
        if(_is_gl)
          continue()
        endif()
        if(_dep_base MATCHES "^libQt6")
          continue()
        endif()
        set(_dest "${_root}/${_dep_base}")
        if(EXISTS "${_dest}" AND NOT IS_SYMLINK "${_dest}")
          continue()
        endif()
        if(IS_SYMLINK "${_dest}")
          file(REMOVE "${_dest}")
        endif()
        list(FIND _copied "${_dep_base}" _copied_idx)
        if(NOT _copied_idx EQUAL -1)
          continue()
        endif()
        execute_process(
          COMMAND cp -L "${_dep_path}" "${_dest}"
          RESULT_VARIABLE _cp_result
        )
        if(NOT _cp_result EQUAL 0)
          continue()
        endif()
        list(APPEND _copied "${_dep_base}")
        set(_added TRUE)
      endforeach()
    endforeach()
    if(NOT _added)
      break()
    endif()
    file(GLOB_RECURSE _targets
      "${_root}/*.so"
      "${_root}/*.so.*"
      "${_root}/platforms/*.so"
      "${_root}/multimedia/*.so"
    )
  endforeach()
endfunction()

_nyx_bundle_ldd_deps("${_DST_DIR}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -DROOT=${_DST_DIR}
          -P "${CMAKE_CURRENT_LIST_DIR}/materialize-lib-symlinks.cmake"
)
