cmake_minimum_required(VERSION 3.10)

project(com.lostdomain.zoom.sdPlugin VERSION 1.0)

set(
  CMAKE_INSTALL_PREFIX
  "../${CMAKE_PROJECT_NAME}"
)

set(CMAKE_CXX_STANDARD 11)
set(CXX_STANDARD_REQUIRED true)
if (MSVC)
  add_definitions("/Zc:__cplusplus")
endif()
include_directories("${CMAKE_SOURCE_DIR}")

add_subdirectory(Vendor)
add_subdirectory(StreamDeckSDK)
add_subdirectory(Sources)
add_subdirectory(sdplugin)
