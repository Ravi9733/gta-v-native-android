cmake_minimum_required(VERSION 3.22.1)
project(gtav_android LANGUAGES C CXX)

add_library(gtav_android SHARED native_bridge.cpp)
target_compile_features(gtav_android PRIVATE cxx_std_20)
target_compile_options(gtav_android PRIVATE -Wall -Wextra -Werror)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)
find_library(aaudio-lib aaudio)
target_link_libraries(gtav_android PRIVATE ${log-lib} ${android-lib} ${vulkan-lib} ${aaudio-lib})


# Compile-check the Android clock implementation from the real host runtime.
# It is intentionally not linked into the smoke-test APK yet: the complete
# runtime target still depends on the unported NVN/Metal backend and services.
add_library(gtav_android_time_portability OBJECT
    ../../../../runtime/time_host.c
)
target_compile_features(gtav_android_time_portability PRIVATE c_std_11)
target_include_directories(gtav_android_time_portability PRIVATE
    ../../../../runtime
)
target_compile_options(gtav_android_time_portability PRIVATE -Wall -Wextra)
