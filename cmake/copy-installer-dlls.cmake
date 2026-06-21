# Copy runtime files from build dir into installer staging.
file(GLOB _nyx_dlls "${SRC}/*.dll")
foreach(_dll ${_nyx_dlls})
  file(COPY "${_dll}" DESTINATION "${DST}")
endforeach()

set(_plugin_dirs platforms qml tls styles imageformats iconengines generic translations
    networkinformation sqldrivers)
foreach(_d ${_plugin_dirs})
  if(EXISTS "${SRC}/${_d}")
    file(COPY "${SRC}/${_d}" DESTINATION "${DST}")
  endif()
endforeach()
