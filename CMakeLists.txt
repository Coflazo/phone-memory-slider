cmake_minimum_required(VERSION 3.24)

project(PhoneMemorySlider VERSION 1.0.0 LANGUAGES CXX)

set(CPACK_PACKAGE_NAME "PhoneMemorySlider")
set(CPACK_PACKAGE_VENDOR "Phone Memory Slider contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-only phone gallery review")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "PhoneMemorySlider-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PMS_BUILD_TESTS "Build Phone Memory Slider tests" ON)
option(PMS_BUILD_DESKTOP "Build the Qt desktop application" ON)
option(PMS_ENABLE_DEMO_AUTOMATION "Compile the screenshot/demo automation harness" OFF)

if(PMS_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

if(MSVC)
  add_compile_options(/W4 /WX /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_subdirectory(core)
add_subdirectory(tools)

if(PMS_BUILD_DESKTOP)
  find_package(Qt6 6.8 QUIET COMPONENTS Concurrent Quick QuickControls2 Multimedia Sql Test)
  if(Qt6_FOUND)
    qt_standard_project_setup(REQUIRES 6.8)
    add_subdirectory(desktop)
  else()
    message(STATUS "Qt 6.8+ not found; desktop target is disabled")
  endif()
endif()

if(PMS_BUILD_TESTS)
  add_subdirectory(core/tests)
endif()

include(CPack)
