cmake_minimum_required(VERSION 3.22.1)
project("camswap-native-hook" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No Dobby: every interception is now data-only (GOT entries and OpenSL ES
# vtable pointers), so no inline-hook library is needed and no system
# library's code bytes change in memory.
add_library(camswap-native-hook SHARED
    native_audio_hook.cpp
    pcm_bridge.cpp
    got_hook.cpp
    integrity_check.cpp
    hook_aaudio.cpp
    hook_opensles.cpp
)

target_include_directories(camswap-native-hook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(camswap-native-hook
    log
    dl
    android
    OpenSLES
)
