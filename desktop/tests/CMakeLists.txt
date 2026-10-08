foreach(test_name IN ITEMS app_controller catalog_store device_connector phone_client visual_encoder recovery_vault)
  add_executable(pms_${test_name}_tests ${test_name}_tests.cpp)
  target_include_directories(pms_${test_name}_tests PRIVATE ../src)
  target_link_libraries(pms_${test_name}_tests PRIVATE
    pms_desktop
    Qt6::Test
  )
  add_test(NAME pms_${test_name}_tests COMMAND pms_${test_name}_tests)
  if(WIN32)
    set_tests_properties(pms_${test_name}_tests PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
  endif()
endforeach()

# The v2 link client against a scripted phone that runs the real Noise handshake.
add_executable(pms_companion_client_tests companion_client_tests.cpp fake_phone.cpp fake_phone.hpp)
target_link_libraries(pms_companion_client_tests PRIVATE pms_desktop_link Qt6::Test)
add_test(NAME pms_companion_client_tests COMMAND pms_companion_client_tests)
if(WIN32)
  set_tests_properties(pms_companion_client_tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  )
endif()
