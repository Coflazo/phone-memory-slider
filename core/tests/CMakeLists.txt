add_executable(pms_core_tests
  test_main.cpp
  ranking_tests.cpp
  review_queue_tests.cpp
)

target_link_libraries(pms_core_tests PRIVATE pms_core)
add_test(NAME pms_core_tests COMMAND pms_core_tests)
