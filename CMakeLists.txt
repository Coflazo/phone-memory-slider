cmake_minimum_required(VERSION 3.24)

project(PhoneMemorySlider VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PMS_BUILD_TESTS "Build Phone Memory Slider tests" ON)
option(PMS_BUILD_DESKTOP "Build the Qt desktop application" ON)

if(MSVC)
  add_compile_options(/W4 /WX /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_subdirectory(core)

if(PMS_BUILD_DESKTOP)
  find_package(Qt6 6.8 QUIET COMPONENTS Quick QuickControls2 Multimedia)
  if(Qt6_FOUND)
    add_subdirectory(desktop)
  else()
    message(STATUS "Qt 6.8+ not found; desktop target is disabled")
  endif()
endif()

if(PMS_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(core/tests)
endif()
