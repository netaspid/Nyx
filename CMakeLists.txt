cmake_minimum_required(VERSION 3.16)
project(nyx VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NYX_BUILD_TESTS "Build unit tests" ON)
option(NYX_BUILD_GUI "Build nyx-app Qt GUI" ON)

include(FetchContent)
FetchContent_Declare(
  noise-c
  GIT_REPOSITORY https://github.com/rweather/noise-c.git
  GIT_TAG master
)
FetchContent_MakeAvailable(noise-c)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  opus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG v1.5.2
)
FetchContent_MakeAvailable(opus)

set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_APP OFF CACHE BOOL "" FORCE)
set(CONFIG_AV1_ENCODER 1 CACHE STRING "" FORCE)
set(CONFIG_AV1_DECODER 1 CACHE STRING "" FORCE)
set(AOM_TARGET_CPU "generic" CACHE STRING "" FORCE)
FetchContent_Declare(
  libaom
  GIT_REPOSITORY https://aomedia.googlesource.com/aom.git
  GIT_TAG v3.9.1
  GIT_SHALLOW TRUE
)
# libaom generate-config scripts need Perl (e.g. Strawberry Perl on Windows).
find_package(Perl)
if(NOT PERL_FOUND)
  message(FATAL_ERROR "Perl is required to build libaom (AV1). Install Strawberry Perl and reconfigure.")
endif()
FetchContent_MakeAvailable(libaom)

# noise-c has no upstream CMakeLists — build ref backend (no libsodium)
file(GLOB NOISE_C_BACKEND_REF ${noise-c_SOURCE_DIR}/src/backend/ref/*.c)
list(FILTER NOISE_C_BACKEND_REF EXCLUDE REGEX "dh-curve448|dh-newhope")
file(GLOB NOISE_C_PROTOCOL ${noise-c_SOURCE_DIR}/src/protocol/*.c)
list(FILTER NOISE_C_PROTOCOL EXCLUDE REGEX "rand_sodium")

set(NOISE_C_CRYPTO
  ${noise-c_SOURCE_DIR}/src/crypto/aes/rijndael-alg-fst.c
  ${noise-c_SOURCE_DIR}/src/crypto/blake2/blake2b.c
  ${noise-c_SOURCE_DIR}/src/crypto/blake2/blake2s.c
  ${noise-c_SOURCE_DIR}/src/crypto/chacha/chacha.c
  ${noise-c_SOURCE_DIR}/src/crypto/donna/poly1305-donna.c
  ${noise-c_SOURCE_DIR}/src/crypto/ed25519/ed25519.c
  ${noise-c_SOURCE_DIR}/src/crypto/ghash/ghash.c
  ${noise-c_SOURCE_DIR}/src/crypto/sha2/sha256.c
  ${noise-c_SOURCE_DIR}/src/crypto/sha2/sha512.c
)

add_library(noise-c-lib STATIC ${NOISE_C_CRYPTO} ${NOISE_C_BACKEND_REF} ${NOISE_C_PROTOCOL})
target_sources(noise-c-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/noise_dh_stubs.c)
if(WIN32)
  target_sources(noise-c-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/noise_mingw_compat.c)
  target_compile_options(noise-c-lib PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/cmake/noise_mingw_compat.h")
endif()
target_include_directories(noise-c-lib PUBLIC
  ${noise-c_SOURCE_DIR}/include
  ${noise-c_SOURCE_DIR}/src
  ${noise-c_SOURCE_DIR}/src/protocol
)
target_compile_definitions(noise-c-lib PUBLIC NOISE_STATICLIB ED25519_CUSTOMHASH ED25519_CUSTOMRANDOM)

add_library(nyx 
  src/util.cpp
  src/proto.cpp
  src/crypto.cpp
  src/transport.cpp
  src/mux.cpp
  src/udp.cpp
  src/rendezvous.cpp
  src/connection.cpp
  src/console.cpp
  src/paths.cpp
  src/identity.cpp
  src/profile_meta.cpp
  src/avatar_store.cpp
  src/avatar_proto.cpp
  src/markdown_format.cpp
  src/call_proto.cpp
  src/call_session.cpp
  src/call_media.cpp
  src/call_opus.cpp
  src/call_mesh.cpp
  src/call_av1.cpp
  src/profile_crypto.cpp
  src/recovery_phrase.cpp
  src/account_store.cpp
  src/app.cpp
  src/messaging.cpp
  src/message_store.cpp
  src/chat_id.cpp
  src/outbox.cpp
  src/chat_service.cpp
  src/file_hash.cpp
  src/blob_store.cpp
  src/file_index.cpp
  src/file_proto.cpp
  src/file_transfer.cpp
  src/file_access.cpp
  src/group.cpp
  src/group_proto.cpp
  src/group_hub.cpp
  src/group_member.cpp
  src/mdns.cpp
  src/nat.cpp
  src/log.cpp
  src/conversation.cpp
  src/network_config.cpp
  src/rendezvous_pool.cpp
  src/rendezvous_server.cpp
  src/session_intent.cpp
)
target_include_directories(nyx PUBLIC
  include
  ${libaom_SOURCE_DIR}
  ${libaom_BINARY_DIR}
)
target_link_libraries(nyx PUBLIC noise-c-lib opus aom)

if(WIN32)
  target_link_libraries(nyx PUBLIC ws2_32 crypt32)
  foreach(_app nyx-node nyx-rendezvous nyx-tests nyx-appcore-tests nyx-app)
    if(TARGET ${_app})
      target_compile_options(${_app} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
  endforeach()
endif()

add_library(nyx-app-common
  apps/common/connection_label.cpp
  apps/common/connect_via_hint.cpp
  apps/common/direct_chat_loop.cpp
)
target_include_directories(nyx-app-common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/common
)
target_link_libraries(nyx-app-common PUBLIC nyx)

add_executable(nyx-node
  apps/nyx-node/main.cpp
  apps/nyx-node/cli.cpp
  apps/nyx-node/cli_console.cpp
  apps/nyx-node/chat_session.cpp
  apps/nyx-node/node_session.cpp
  apps/nyx-node/group_session.cpp
)
target_include_directories(nyx-node PRIVATE apps/nyx-node)
target_link_libraries(nyx-node PRIVATE nyx nyx-app-common)

add_library(nyx-appcore
  apps/nyx-app/appcore/node_service.cpp
  apps/nyx-app/appcore/node_service_connect.cpp
  apps/nyx-app/appcore/node_service_chat.cpp
  apps/nyx-app/appcore/node_service_files.cpp
  apps/nyx-app/appcore/node_service_group.cpp
  apps/nyx-app/appcore/node_service_avatar.cpp
  apps/nyx-app/appcore/node_service_call.cpp
)
target_include_directories(nyx-appcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/appcore
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/common
)
target_link_libraries(nyx-appcore PUBLIC nyx nyx-app-common)

add_executable(nyx-rendezvous apps/nyx-rendezvous/main.cpp)
target_link_libraries(nyx-rendezvous PRIVATE nyx)

if(NYX_BUILD_GUI)
  if(WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/detect-qt-mingw.cmake)
  endif()
  set(QT_MIN_VERSION 6.5)
  find_package(Qt6 ${QT_MIN_VERSION} QUIET COMPONENTS Quick Qml QuickControls2 LabsSettings Gui Widgets Svg Multimedia)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    qt_standard_project_setup(REQUIRES ${QT_MIN_VERSION})

    qt_add_executable(nyx-app
      apps/nyx-app/main.cpp
      apps/nyx-app/qt/node_controller.cpp
      apps/nyx-app/qt/win_chrome.cpp
      apps/nyx-app/qt/call_audio_io.cpp
      apps/nyx-app/qt/call_video_io.cpp
      apps/nyx-app/qt/message_model.cpp
      apps/nyx-app/qt/lan_peer_model.cpp
      apps/nyx-app/qt/chat_list_model.cpp
      apps/nyx-app/qt/file_list_model.cpp
      apps/nyx-app/resources.qrc
    )
    if(WIN32)
      # windres+gcc из ProgramData ломается в build/ из‑за чужих libgcc*.dll рядом с exe.
      # Собираем .res в чистом подкаталоге без Qt-DLL.
      set(NYX_WINRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/_winrc")
      file(MAKE_DIRECTORY "${NYX_WINRC_DIR}")
      configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/resources/icons/nyx.ico"
        "${NYX_WINRC_DIR}/nyx.ico"
        COPYONLY)
      set(NYX_APP_ICON_RC "nyx.ico")
      configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/nyx-app.rc.in"
        "${NYX_WINRC_DIR}/nyx-app.rc"
        @ONLY)
      set(NYX_APP_RC_OBJ "${NYX_WINRC_DIR}/nyx-app_res.o")
      add_custom_command(
        OUTPUT "${NYX_APP_RC_OBJ}"
        COMMAND "${CMAKE_RC_COMPILER}" -O coff nyx-app.rc nyx-app_res.o
        DEPENDS
          "${NYX_WINRC_DIR}/nyx-app.rc"
          "${NYX_WINRC_DIR}/nyx.ico"
        WORKING_DIRECTORY "${NYX_WINRC_DIR}"
        COMMENT "windres: nyx-app icon"
        VERBATIM)
      target_sources(nyx-app PRIVATE "${NYX_APP_RC_OBJ}")
    endif()
    target_include_directories(nyx-app PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/qt
      ${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/appcore
    )
    target_link_libraries(nyx-app PRIVATE
      nyx-appcore
      Qt6::Gui
      Qt6::Widgets
      Qt6::Quick
      Qt6::Qml
      Qt6::QuickControls2
      Qt6::LabsSettings
    )
    if(TARGET Qt6::Svg)
      target_link_libraries(nyx-app PRIVATE Qt6::Svg)
    endif()
    if(TARGET Qt6::Multimedia)
      target_link_libraries(nyx-app PRIVATE Qt6::Multimedia)
      target_compile_definitions(nyx-app PRIVATE NYX_HAS_QT_MULTIMEDIA=1)
    endif()
    if(WIN32)
      set_target_properties(nyx-app PROPERTIES WIN32_EXECUTABLE TRUE)
      target_link_libraries(nyx-app PRIVATE dwmapi)
      target_compile_options(nyx-app PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
      find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin" "${Qt6_INSTALL_PREFIX}/bin")
      if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET nyx-app POST_BUILD
          COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                  --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/ui"
                  --no-compiler-runtime
                  "$<TARGET_FILE:nyx-app>"
          COMMENT "windeployqt: Qt DLL + QML"
          VERBATIM)
        get_filename_component(_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_command(TARGET nyx-app POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_MINGW_BIN}/libgcc_s_seh-1.dll"
            "${_MINGW_BIN}/libstdc++-6.dll"
            "${_MINGW_BIN}/libwinpthread-1.dll"
            "$<TARGET_FILE_DIR:nyx-app>"
          COMMENT "MinGW runtime DLL рядом с nyx-app.exe"
          VERBATIM)
      endif()
    endif()
    message(STATUS "nyx-app: Qt ${Qt6_VERSION} found, GUI will be built")

    if(WIN32)
      set(NYX_INSTALLER_DIR "${CMAKE_BINARY_DIR}/_installer")
      set(NYX_INSTALLER_STAGING "${NYX_INSTALLER_DIR}/staging")
      set(NYX_INSTALLER_OUT "${CMAKE_BINARY_DIR}/NyxSetup.exe")
      set(NYX_SETUP_STUB "${NYX_INSTALLER_DIR}/stub.exe")
      set(NYX_PACK_INSTALLER_EXE "${NYX_INSTALLER_DIR}/pack.exe")

      add_executable(nyx-setup-stub apps/nyx-setup/main.cpp apps/nyx-setup/setup_util.cpp)
      target_include_directories(nyx-setup-stub PRIVATE apps/nyx-setup)
      target_link_libraries(nyx-setup-stub PRIVATE shell32 ole32 comctl32 shlwapi)
      target_compile_options(nyx-setup-stub PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
      set_target_properties(nyx-setup-stub PROPERTIES
        WIN32_EXECUTABLE TRUE
        RUNTIME_OUTPUT_DIRECTORY "${NYX_INSTALLER_DIR}"
        OUTPUT_NAME stub
      )
      target_link_options(nyx-setup-stub PRIVATE -municode)

      add_executable(nyx-uninstall apps/nyx-uninstall/main.cpp)
      target_link_libraries(nyx-uninstall PRIVATE shell32)
      set_target_properties(nyx-uninstall PROPERTIES WIN32_EXECUTABLE TRUE)
      target_link_options(nyx-uninstall PRIVATE -municode)

      add_executable(nyx-installer-pack apps/nyx-setup/pack_installer.cpp)
      target_compile_options(nyx-installer-pack PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
      set_target_properties(nyx-installer-pack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${NYX_INSTALLER_DIR}"
        OUTPUT_NAME pack
      )

      configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-pack-installer.bat.in
        ${NYX_INSTALLER_DIR}/run-pack-installer.bat
        @ONLY
      )

      add_custom_command(
        OUTPUT ${NYX_INSTALLER_OUT}
        COMMAND ${CMAKE_COMMAND} -E rm -f
                ${CMAKE_BINARY_DIR}/nyx-setup.exe
                ${CMAKE_BINARY_DIR}/nyx-setup-stub.exe
                ${CMAKE_BINARY_DIR}/nyx-setup-packed.exe
                ${CMAKE_BINARY_DIR}/Nyx-Setup.exe
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${NYX_INSTALLER_STAGING}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NYX_INSTALLER_STAGING}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nyx-app> ${NYX_INSTALLER_STAGING}/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nyx-node> ${NYX_INSTALLER_STAGING}/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nyx-rendezvous> ${NYX_INSTALLER_STAGING}/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nyx-uninstall> ${NYX_INSTALLER_STAGING}/
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                $<TARGET_FILE_DIR:nyx-app>/platforms ${NYX_INSTALLER_STAGING}/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                $<TARGET_FILE_DIR:nyx-app>/qml ${NYX_INSTALLER_STAGING}/qml
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                $<TARGET_FILE_DIR:nyx-app>/multimedia ${NYX_INSTALLER_STAGING}/multimedia
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/README.md ${NYX_INSTALLER_STAGING}/README.md
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_BINARY_DIR} -DDST=${NYX_INSTALLER_STAGING}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy-installer-dlls.cmake
        COMMAND ${NYX_INSTALLER_DIR}/run-pack-installer.bat
        DEPENDS nyx-app nyx-node nyx-rendezvous nyx-setup-stub nyx-uninstall nyx-installer-pack
        COMMENT "NyxSetup.exe"
        VERBATIM
      )

      add_custom_target(nyx-installer ALL DEPENDS ${NYX_INSTALLER_OUT})
      add_custom_command(
        TARGET nyx-installer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Installer: ${NYX_INSTALLER_OUT}"
        VERBATIM
      )
    endif()
  else()
    message(WARNING "Qt6 Quick not found — nyx-app skipped. Set CMAKE_PREFIX_PATH to Qt 6.x mingw_64.")
  endif()
endif()

if(NYX_BUILD_TESTS)
  enable_testing()
  # Release иначе глушит assert — тесты должны падать явно.
  add_executable(nyx-tests tests/test_main.cpp)
  target_link_libraries(nyx-tests PRIVATE nyx)
  if(MSVC)
    target_compile_options(nyx-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(nyx-tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME nyx-tests COMMAND nyx-tests)

  add_executable(nyx-appcore-tests tests/test_node_service.cpp)
  target_link_libraries(nyx-appcore-tests PRIVATE nyx-appcore)
  if(MSVC)
    target_compile_options(nyx-appcore-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(nyx-appcore-tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME nyx-appcore-tests COMMAND nyx-appcore-tests)
endif()
