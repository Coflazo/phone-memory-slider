qt_add_executable(phone_memory_slider
  src/main.cpp
  src/review_deck_model.cpp
  src/review_deck_model.hpp
)

qt_add_qml_module(phone_memory_slider
  URI PhoneMemorySlider
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/ReviewCard.qml
    qml/Theme.qml
  RESOURCES
    resources/fixture-city.svg
    resources/fixture-lake.svg
    resources/fixture-screen.svg
)

target_link_libraries(phone_memory_slider PRIVATE
  pms_core
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Multimedia
)

set_target_properties(phone_memory_slider PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE
)

