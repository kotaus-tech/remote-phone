cmake_minimum_required(VERSION 3.22.1)
project(remote_phone_android_pairing LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)
set(PAIRING_BRIDGE_PATH
    "${CMAKE_CURRENT_LIST_DIR}/../../../build/generated/pairingStatic/${ANDROID_ABI}/libremote_phone_pairing_bridge.a")
if(NOT EXISTS "${PAIRING_BRIDGE_PATH}")
    message(FATAL_ERROR "Не собран статический Rust pairing-core: ${PAIRING_BRIDGE_PATH}")
endif()

add_library(remote_phone_pairing_bridge STATIC IMPORTED)
set_target_properties(remote_phone_pairing_bridge PROPERTIES
    IMPORTED_LOCATION "${PAIRING_BRIDGE_PATH}")

add_library(remote_phone_jni SHARED remote_phone_jni.cpp)
target_include_directories(remote_phone_jni PRIVATE
    "${REPOSITORY_ROOT}/native/pairing-bridge/include")
target_link_libraries(remote_phone_jni PRIVATE remote_phone_pairing_bridge android log)
target_link_options(remote_phone_jni PRIVATE "-Wl,--no-undefined")
target_compile_definitions(remote_phone_jni PRIVATE
    $<$<CONFIG:Debug>:REMOTE_PHONE_ENABLE_TEST_PEER>)
