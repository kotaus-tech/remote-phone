cmake_minimum_required(VERSION 3.25)
project(RemotePhoneVirtualCameraHost LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Тестовый узел виртуальной камеры собирается только для Windows.")
endif()

add_executable(RemotePhone.VirtualCameraHost main.cpp)
target_compile_features(RemotePhone.VirtualCameraHost PRIVATE cxx_std_17)
target_compile_definitions(RemotePhone.VirtualCameraHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(RemotePhone.VirtualCameraHost PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource"
)
target_link_libraries(RemotePhone.VirtualCameraHost PRIVATE
    mfsensorgroup
    mfplat
    mfuuid
    ole32
)
