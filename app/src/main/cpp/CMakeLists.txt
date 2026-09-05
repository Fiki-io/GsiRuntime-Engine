cmake_minimum_required(VERSION 3.22.1)

project("gsi_runtime")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)

# 1. Main Core Engine Library (JNI Interface)
add_library(
    gsi_core
    SHARED
    gsi_engine.cpp
    image_verifier.cpp
    display_pipeline.cpp
    block_device.cpp
    ext4_reader.cpp
    sandbox_manager.cpp
    process_spawner.cpp
    virtual_framebuffer.cpp
    input_bridge.cpp
    virtual_binder.cpp
    hook_engine.cpp
    property_service.cpp
    init_parser.cpp
    boot_manager.cpp
    gralloc_bridge.cpp
    network_bridge.cpp
    logcat_broker.cpp
    audio_bridge.cpp
    battery_bridge.cpp
    sensor_bridge.cpp
)

target_link_libraries(
    gsi_core
    ${log-lib}
    ${android-lib}
    ${dl-lib}
)

# 2. Standalone In-Process Hook Library for LD_PRELOAD
add_library(
    gsi_hook
    SHARED
    hook_engine.cpp
    virtual_binder.cpp
    gralloc_bridge.cpp
    virtual_framebuffer.cpp
)

target_link_libraries(
    gsi_hook
    ${log-lib}
    ${dl-lib}
)
