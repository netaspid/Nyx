if(NOT ROOT)
  message(FATAL_ERROR "materialize-lib-symlinks.cmake: ROOT is required")
endif()

execute_process(
  COMMAND find "${ROOT}" -type l \( -name "*.so" -o -name "*.so.*" \)
  OUTPUT_VARIABLE _nyx_symlinks
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _nyx_find_result
)
if(NOT _nyx_find_result EQUAL 0)
  message(FATAL_ERROR "materialize-lib-symlinks.cmake: find failed")
endif()

if(_nyx_symlinks STREQUAL "")
  return()
endif()

string(REPLACE "\n" ";" _nyx_symlink_list "${_nyx_symlinks}")
foreach(_link IN LISTS _nyx_symlink_list)
  if(_link STREQUAL "")
    continue()
  endif()
  execute_process(
    COMMAND readlink -f "${_link}"
    OUTPUT_VARIABLE _nyx_target
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _nyx_readlink_result
  )
  if(NOT _nyx_readlink_result EQUAL 0 OR NOT EXISTS "${_nyx_target}")
    continue()
  endif()
  file(REMOVE "${_link}")
  execute_process(
    COMMAND cp -f "${_nyx_target}" "${_link}"
    RESULT_VARIABLE _nyx_cp_result
  )
  if(NOT _nyx_cp_result EQUAL 0)
    message(FATAL_ERROR "materialize-lib-symlinks.cmake: cp failed for ${_link}")
  endif()
endforeach()
