foreach(test_name IN ITEMS app_controller catalog_store phone_client visual_encoder)
  add_executable(pms_${test_name}_tests ${test_name}_tests.cpp)
  target_include_directories(pms_${test_name}_tests PRIVATE ../src)
  target_link_libraries(pms_${test_name}_tests PRIVATE
    pms_desktop
    Qt6::Test
  )
  add_test(NAME pms_${test_name}_tests COMMAND pms_${test_name}_tests)
endforeach()
