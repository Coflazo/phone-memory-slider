add_library(pms_core STATIC
  src/ranking.cpp
  src/review_queue.cpp
  src/review_session.cpp
  src/fixture_catalog.cpp
  src/media_analysis.cpp
  src/preference_model.cpp
  src/link/frames.cpp
  src/link/messages.cpp
  src/link/qr.cpp
)

target_include_directories(pms_core PUBLIC include)

# Phone link Noise session (noise-c). A library of its own so that core/tests can build a second copy with the
# fixed-ephemeral test hook (PMS_LINK_TEST_HOOKS); this copy, the one the app links, has no hook.
add_library(pms_link STATIC
  src/link/noise_ik.cpp
  src/link/session.cpp
)
target_link_libraries(pms_link PUBLIC pms_core PRIVATE noise_c)

# noise-c calls noise_rand_bytes for every key it generates; ours reads the OS CSPRNG and is compiled with our warnings.
add_library(pms_noise_rand OBJECT src/link/random.cpp)
target_link_libraries(noise_c PRIVATE pms_noise_rand)
if(WIN32)
  target_link_libraries(noise_c PRIVATE bcrypt)
endif()
