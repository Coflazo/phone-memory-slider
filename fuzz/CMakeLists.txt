# libFuzzer targets for every parser that reads phone or file bytes (fuzz/README.md). The top-level CMakeLists.txt
# instruments every target when PMS_BUILD_FUZZ is on; only these executables link the libFuzzer driver.
foreach(target IN ITEMS frames messages qr handshake media_meta sniff)
  add_executable(fuzz_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_${target} PRIVATE pms_core)
  if(MSVC)
    target_compile_options(fuzz_${target} PRIVATE /fsanitize=fuzzer)
  else()
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
  endif()
endforeach()
target_link_libraries(fuzz_handshake PRIVATE pms_link)
