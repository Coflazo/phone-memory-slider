add_executable(pms_core_tests
  test_main.cpp
  fixture_catalog_tests.cpp
  media_analysis_tests.cpp
  preference_model_tests.cpp
  ranking_tests.cpp
  review_session_tests.cpp
  review_queue_tests.cpp
)

target_link_libraries(pms_core_tests PRIVATE pms_core)
add_test(NAME pms_core_tests COMMAND pms_core_tests)
