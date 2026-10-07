cmake_minimum_required(VERSION 3.24)
project(decode_spike LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(libheif REQUIRED)
set(QT_FFMPEG "${CMAKE_SOURCE_DIR}/../../../.tools/deps/qt-ffmpeg")

add_executable(decode_bench decode_bench.cpp)
target_include_directories(decode_bench PRIVATE "${QT_FFMPEG}/include")
target_link_libraries(decode_bench PRIVATE Qt6::Core Qt6::Gui heif
    "${QT_FFMPEG}/lib/avformat.lib" "${QT_FFMPEG}/lib/avcodec.lib" "${QT_FFMPEG}/lib/avutil.lib" "${QT_FFMPEG}/lib/swscale.lib")
