# Bundle PDF CLI tools into installer staging (Linux).
# Optional: skips quietly if host tools are missing.
# Usage: cmake -DDST=<staging> -P bundle-doc-tools-linux.cmake

if(NOT DST)
  message(FATAL_ERROR "bundle-doc-tools-linux.cmake: DST required")
endif()

set(_tools "${DST}/tools")
set(_lib "${_tools}/lib")
file(MAKE_DIRECTORY "${_tools}")
file(MAKE_DIRECTORY "${_lib}")

set(_nyx_host_exclude
  libc.so.6
  libm.so.6
  libpthread.so.0
  librt.so.1
  libdl.so.2
  ld-linux-x86-64.so.2
  linux-vdso.so.1
  libGL.so.1
  libOpenGL.so.0
  libEGL.so.1
  libGLdispatch.so.0
  libGLX.so.0
  libdrm.so.2
  libgbm.so.1
)

function(_nyx_copy_tool_bin _name)
  set(_src "")
  foreach(_cand "/usr/bin/${_name}" "/bin/${_name}")
    if(EXISTS "${_cand}")
      set(_src "${_cand}")
      break()
    endif()
  endforeach()
  if(NOT _src)
    return()
  endif()
  execute_process(COMMAND cp -L "${_src}" "${_tools}/${_name}" RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(WARNING "bundle-doc-tools: failed to copy ${_name}")
    return()
  endif()
  execute_process(COMMAND chmod a+x "${_tools}/${_name}")

  execute_process(
    COMMAND ldd "${_tools}/${_name}"
    OUTPUT_VARIABLE _ldd
    ERROR_QUIET
  )
  string(REPLACE "\n" ";" _lines "${_ldd}")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "=> (/[^ ]+)")
      set(_dep "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[\t ]*(/[^ ]+)")
      set(_dep "${CMAKE_MATCH_1}")
    else()
      continue()
    endif()
    get_filename_component(_base "${_dep}" NAME)
    list(FIND _nyx_host_exclude "${_base}" _idx)
    if(NOT _idx EQUAL -1)
      continue()
    endif()
    if(_base MATCHES "^lib(GL|OpenGL|EGL|GLdispatch|GLX|drm|gbm)")
      continue()
    endif()
    if(NOT EXISTS "${_dep}")
      continue()
    endif()
    if(EXISTS "${_lib}/${_base}")
      continue()
    endif()
    execute_process(COMMAND cp -L "${_dep}" "${_lib}/${_base}" RESULT_VARIABLE _cprc)
    if(NOT _cprc EQUAL 0)
      message(WARNING "bundle-doc-tools: failed to copy ${_dep}")
    endif()
  endforeach()
endfunction()

_nyx_copy_tool_bin(mutool)
_nyx_copy_tool_bin(pdfinfo)
_nyx_copy_tool_bin(pdftoppm)

if(EXISTS "${_tools}/mutool" OR EXISTS "${_tools}/pdftoppm")
  message(STATUS "bundle-doc-tools: PDF tools staged under ${_tools}")
else()
  message(STATUS "bundle-doc-tools: no host PDF tools found — installer will use package manager")
endif()
