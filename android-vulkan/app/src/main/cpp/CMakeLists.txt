cmake_minimum_required(VERSION 3.22.1)
project(gtav_native_android LANGUAGES CXX)

add_library(gtav_native SHARED native-lib.cpp)
target_compile_features(gtav_native PRIVATE cxx_std_17)
target_compile_options(gtav_native PRIVATE -Wall -Wextra)
find_library(android-lib android)
find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(gtav_native PRIVATE
    ${android-lib}
    ${log-lib}
    ${vulkan-lib}
)
