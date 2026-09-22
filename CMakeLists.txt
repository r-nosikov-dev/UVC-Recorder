cmake_minimum_required(VERSION 3.20)
project(uvc_chunk_recorder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0)

add_executable(uvc-recorder
    src/main.cpp
    src/device/DeviceEnumerator.cpp
    src/capture/ChunkRecorder.cpp
    src/upload/ChunkUploader.cpp
    src/ui/MainWindow.cpp
)

target_include_directories(uvc-recorder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(uvc-recorder PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)

target_link_libraries(uvc-recorder PRIVATE
    PkgConfig::GTK4
    PkgConfig::GST
    CURL::libcurl
)

install(TARGETS uvc-recorder RUNTIME DESTINATION bin)
