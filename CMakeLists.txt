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
)
target_include_directories(nyx PUBLIC include)
target_link_libraries(nyx PUBLIC noise-c-lib)

if(WIN32)
  target_link_libraries(nyx PUBLIC ws2_32)
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
  apps/nyx-app/appcore/node_service_group.cpp
)
target_include_directories(nyx-appcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/nyx-app/appcore
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/common
)
target_link_libraries(nyx-appcore PUBLIC nyx nyx-app-common)

add_executable(nyx-rendezvous apps/nyx-rendezvous/main.cpp)
target_link_libraries(nyx-rendezvous PRIVATE nyx)

if(NYX_BUILD_GUI)
  set(QT_MIN_VERSION 6.5)
  find_package(Qt6 ${QT_MIN_VERSION} QUIET COMPONENTS Quick Qml QuickControls2 LabsSettings Gui Widgets Svg)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    qt_standard_project_setup(REQUIRES ${QT_MIN_VERSION})

    qt_add_executable(nyx-app
      apps/nyx-app/main.cpp
      apps/nyx-app/qt/node_controller.cpp
      apps/nyx-app/qt/message_model.cpp
      apps/nyx-app/qt/lan_peer_model.cpp
      apps/nyx-app/qt/chat_list_model.cpp
      apps/nyx-app/resources.qrc
    )
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
    if(WIN32)
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
  else()
    message(WARNING "Qt6 Quick not found — nyx-app skipped. Set CMAKE_PREFIX_PATH to Qt 6.x mingw_64.")
  endif()
endif()

if(NYX_BUILD_TESTS)
  enable_testing()
  add_executable(nyx-tests tests/test_main.cpp)
  target_link_libraries(nyx-tests PRIVATE nyx)
  add_test(NAME nyx-tests COMMAND nyx-tests)

  add_executable(nyx-appcore-tests tests/test_node_service.cpp)
  target_link_libraries(nyx-appcore-tests PRIVATE nyx-appcore)
  add_test(NAME nyx-appcore-tests COMMAND nyx-appcore-tests)
endif()
