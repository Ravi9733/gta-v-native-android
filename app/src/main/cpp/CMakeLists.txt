cmake_minimum_required(VERSION 3.22.1)
project(gtav_android LANGUAGES CXX)

add_library(gtav_android SHARED native_bridge.cpp)
target_compile_features(gtav_android PRIVATE cxx_std_20)
target_compile_options(gtav_android PRIVATE -Wall -Wextra -Werror)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)
find_library(aaudio-lib aaudio)
target_link_libraries(gtav_android PRIVATE ${log-lib} ${android-lib} ${vulkan-lib} ${aaudio-lib})
