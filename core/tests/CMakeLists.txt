add_executable(pms_core_tests
  test_main.cpp
  fixture_catalog_tests.cpp
  media_analysis_tests.cpp
  preference_model_tests.cpp
  ranking_tests.cpp
  review_session_tests.cpp
  review_queue_tests.cpp
  link_frames_tests.cpp
  link_messages_tests.cpp
  link_qr_tests.cpp
  link_session_tests.cpp
)

# The tests' own copy of the link session, built with PMS_LINK_TEST_HOOKS: set_fixed_ephemeral_for_tests exists only
# here. The app links pms_link (core/CMakeLists.txt), compiled without the hook.
add_library(pms_link_testing STATIC
  ${PROJECT_SOURCE_DIR}/core/src/link/noise_ik.cpp
  ${PROJECT_SOURCE_DIR}/core/src/link/session.cpp
)
target_compile_definitions(pms_link_testing PUBLIC PMS_LINK_TEST_HOOKS)
target_link_libraries(pms_link_testing PUBLIC pms_core PRIVATE noise_c)
# A target that links both copies fails at generate time (PMS_LINK_VARIANT, see core/CMakeLists.txt).
set_property(TARGET pms_link_testing PROPERTY INTERFACE_PMS_LINK_VARIANT testing)
set_property(TARGET pms_link_testing APPEND PROPERTY COMPATIBLE_INTERFACE_STRING PMS_LINK_VARIANT)

# noise_c directly too: the tests forge a message 1 token by token with noise-c's SymmetricState.
target_link_libraries(pms_core_tests PRIVATE pms_core pms_link_testing noise_c)
target_include_directories(pms_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)  # internal link/noise_ik.hpp
target_compile_definitions(pms_core_tests PRIVATE PMS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME pms_core_tests COMMAND pms_core_tests)
