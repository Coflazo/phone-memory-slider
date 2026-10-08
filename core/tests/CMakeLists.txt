add_executable(pms_core_tests
  test_main.cpp
  delete_model_tests.cpp
  embeddings_tests.cpp
  eta_tests.cpp
  fixture_catalog_tests.cpp
  media_analysis_tests.cpp
  media_format_tests.cpp
  media_kind_tests.cpp
  media_meta_tests.cpp
  pdq_tests.cpp
  preference_model_tests.cpp
  quality_tests.cpp
  ranking_tests.cpp
  review_session_tests.cpp
  review_queue_tests.cpp
  taste_tests.cpp
)

target_link_libraries(pms_core_tests PRIVATE pms_core)
target_compile_definitions(pms_core_tests PRIVATE PMS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME pms_core_tests COMMAND pms_core_tests)
