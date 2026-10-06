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
