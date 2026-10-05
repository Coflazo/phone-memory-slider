add_library(pms_desktop STATIC
  src/app_controller.cpp
  src/app_controller.hpp
  src/catalog_store.cpp
  src/catalog_store.hpp
  src/device_connector.cpp
  src/device_connector.hpp
  src/face_presence_detector.cpp
  src/face_presence_detector.hpp
  src/recovery_vault.cpp
  src/recovery_vault.hpp
  src/review_deck_model.cpp
  src/review_deck_model.hpp
  src/visual_encoder.cpp
  src/visual_encoder.hpp
)
target_include_directories(pms_desktop PUBLIC src)
target_link_libraries(pms_desktop PUBLIC
  pms_core
  Qt6::Concurrent
  Qt6::Gui
  Qt6::Multimedia
  Qt6::Sql
)

if(WIN32)
  target_link_libraries(pms_desktop PUBLIC Bcrypt Crypt32 PortableDeviceGuids Propsys)
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(pms_desktop PUBLIC OpenSSL::Crypto)
endif()

qt_add_executable(phone_memory_slider
  src/main.cpp
)

set_source_files_properties(qml/Theme.qml PROPERTIES
  QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(phone_memory_slider
  URI PhoneMemorySlider
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/DevicePanel.qml
    qml/ReviewCard.qml
    qml/ReviewSummary.qml
    qml/ScanPanel.qml
    qml/SeedPanel.qml
    qml/Theme.qml
  RESOURCES
    resources/fonts/Geist-Bold.ttf
    resources/fonts/Geist-Regular.ttf
    resources/fonts/Geist-SemiBold.ttf
    resources/fonts/GeistMono-Regular.ttf
    resources/fixture-city.svg
    resources/fixture-lake.svg
    resources/fixture-screen.svg
)

target_link_libraries(phone_memory_slider PRIVATE
  pms_desktop
  Qt6::Quick
  Qt6::QuickControls2
)

if(PMS_ENABLE_DEMO_AUTOMATION)
  target_compile_definitions(phone_memory_slider PRIVATE PMS_ENABLE_DEMO_AUTOMATION=1)
endif()

set_target_properties(phone_memory_slider PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE
)

install(TARGETS phone_memory_slider
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
install(FILES
  ${PROJECT_SOURCE_DIR}/LICENSE
  ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
  ${PROJECT_SOURCE_DIR}/docs/PRIVACY.md
  ${PROJECT_SOURCE_DIR}/docs/USER_GUIDE.md
  DESTINATION .
)

if(WIN32)
  qt_generate_deploy_qml_app_script(
    TARGET phone_memory_slider
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
    NO_UNSUPPORTED_PLATFORM_ERROR
    DEPLOY_TOOL_OPTIONS
      --skip-plugin-types
      networkinformation,tls,qmltooling
  )
else()
  qt_generate_deploy_qml_app_script(
    TARGET phone_memory_slider
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
    NO_UNSUPPORTED_PLATFORM_ERROR
  )
endif()

# Qt 6.8 emits an unquoted EXECUTABLE value in its generated deployment script.
# Wrap it so packaging also works when the source/build path contains spaces.
set(deploy_wrapper "${CMAKE_CURRENT_BINARY_DIR}/qt_deploy_wrapper-$<CONFIG>.cmake")
set(patched_deploy_script "${CMAKE_CURRENT_BINARY_DIR}/qt_deploy_patched-$<CONFIG>.cmake")
set(deploy_wrapper_template [=[
file(READ [==[@DEPLOY_SCRIPT@]==] qt_deploy_contents)
string(REGEX REPLACE "EXECUTABLE ([^\r\n]+)" "EXECUTABLE \"\\1\"" qt_deploy_contents "${qt_deploy_contents}")
file(WRITE [==[@PATCHED_DEPLOY_SCRIPT@]==] "${qt_deploy_contents}")
include([==[@PATCHED_DEPLOY_SCRIPT@]==])
]=])
set(DEPLOY_SCRIPT "${deploy_script}")
set(PATCHED_DEPLOY_SCRIPT "${patched_deploy_script}")
string(CONFIGURE "${deploy_wrapper_template}" deploy_wrapper_contents @ONLY)
file(GENERATE OUTPUT "${deploy_wrapper}" CONTENT "${deploy_wrapper_contents}")
install(SCRIPT "${deploy_wrapper}")

if(APPLE)
  install(CODE [[
    file(REMOVE_RECURSE
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/phone_memory_slider.app/Contents/PlugIns/networkinformation"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/phone_memory_slider.app/Contents/PlugIns/tls"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/phone_memory_slider.app/Contents/PlugIns/qmltooling"
    )
  ]])
endif()

if(PMS_BUILD_TESTS)
  add_subdirectory(tests)
  add_executable(pms_evaluate_gallery ../tools/pms-evaluate-gallery.cpp)
  target_include_directories(pms_evaluate_gallery PRIVATE src)
  target_link_libraries(pms_evaluate_gallery PRIVATE pms_desktop Qt6::Gui)
endif()
