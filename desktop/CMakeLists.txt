add_library(pms_desktop STATIC
  src/app_controller.cpp
  src/app_controller.hpp
  src/catalog_store.cpp
  src/catalog_store.hpp
  src/face_presence_detector.cpp
  src/face_presence_detector.hpp
  src/phone_client.cpp
  src/phone_client.hpp
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
  Qt6::Network
  Qt6::Sql
)

qt_add_executable(phone_memory_slider
  src/main.cpp
)

qt_add_qml_module(phone_memory_slider
  URI PhoneMemorySlider
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/PairingPanel.qml
    qml/ReviewCard.qml
    qml/ReviewSummary.qml
    qml/ScanPanel.qml
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

qt_generate_deploy_qml_app_script(
  TARGET phone_memory_slider
  OUTPUT_SCRIPT deploy_script
  NO_TRANSLATIONS
  NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

if(PMS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
