cmake_minimum_required(VERSION 3.25)
project(RemotePhoneVirtualCameraHost LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Тестовый узел виртуальной камеры собирается только для Windows.")
endif()

add_executable(RemotePhone.VirtualCameraHost
    main.cpp
    ../frame-transport/SharedNv12FrameBuffer.cpp
)
target_compile_features(RemotePhone.VirtualCameraHost PRIVATE cxx_std_17)
target_compile_definitions(RemotePhone.VirtualCameraHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX REMOTE_PHONE_FRAME_TRANSPORT_TESTING)
if(MSVC)
    # The host contains Russian wide-string literals used for the camera name and console output.
    target_compile_options(RemotePhone.VirtualCameraHost PRIVATE /utf-8)
endif()
target_include_directories(RemotePhone.VirtualCameraHost PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource"
    "${CMAKE_CURRENT_LIST_DIR}/../frame-transport"
)
target_link_libraries(RemotePhone.VirtualCameraHost PRIVATE
    advapi32
    mfsensorgroup
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    shell32
    uuid
)
