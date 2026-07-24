# Copy runtime files from build dir into installer staging.

function(_nyx_copy_runtime_file _src _dst_dir)
  get_filename_component(_base "${_src}" NAME)
  set(_dest "${_dst_dir}/${_base}")
  if(EXISTS "${_dest}" AND NOT IS_SYMLINK "${_dest}")
    return()
  endif()
  if(IS_SYMLINK "${_dest}")
    file(REMOVE "${_dest}")
  endif()
  if(IS_SYMLINK "${_src}")
    execute_process(
      COMMAND bash -c "test -e '${_src}'"
      RESULT_VARIABLE _link_ok
    )
    if(NOT _link_ok EQUAL 0)
      return()
    endif()
  endif()
  execute_process(
    COMMAND cp -L "${_src}" "${_dest}"
    RESULT_VARIABLE _cp_result
  )
  if(NOT _cp_result EQUAL 0)
    message(FATAL_ERROR "copy failed: ${_src} -> ${_dest}")
  endif()
endfunction()

if(WIN32)
  file(GLOB _nyx_libs "${SRC}/*.dll")
  foreach(_lib ${_nyx_libs})
    file(COPY "${_lib}" DESTINATION "${DST}")
  endforeach()
else()
  file(GLOB _nyx_libs "${SRC}/*.so*")
  foreach(_lib ${_nyx_libs})
    get_filename_component(_base "${_lib}" NAME)
    # Host OpenGL/Mesa/NVIDIA must stay on the system (see copy-qt-runtime-linux).
    if(_base MATCHES "^lib(GL|OpenGL|EGL|GLdispatch|GLX|drm|gbm)([._].*)?$")
      continue()
    endif()
    _nyx_copy_runtime_file("${_lib}" "${DST}")
  endforeach()
endif()

set(_plugin_dirs
  platforms
  xcbglintegrations
  qml
  tls
  styles
  imageformats
  iconengines
  generic
  translations
  networkinformation
  sqldrivers
  multimedia
)
foreach(_d ${_plugin_dirs})
  if(EXISTS "${SRC}/${_d}")
    file(COPY "${SRC}/${_d}" DESTINATION "${DST}")
  endif()
endforeach()
