# Vendored PDQ (third_party/pdq/README.md). STATIC, so objects nothing references (pdqutils' Timer,
# pdqhashtypes.cpp's global std::random_device) are never linked. Warnings stay visible but not fatal.
# SYSTEM keeps warnings from its headers out of our /WX code.
set(PMS_PDQ_DIR ${PROJECT_SOURCE_DIR}/third_party/pdq)
add_library(pms_pdq STATIC
  ${PMS_PDQ_DIR}/common/pdqhashtypes.cpp
  ${PMS_PDQ_DIR}/common/pdqutils.cpp
  ${PMS_PDQ_DIR}/downscaling/downscaling.cpp
  ${PMS_PDQ_DIR}/hashing/pdqhashing.cpp
  ${PMS_PDQ_DIR}/hashing/torben.cpp
)
target_include_directories(pms_pdq SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(pms_pdq PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX-,-Wno-error>)

add_library(pms_core STATIC
  src/ranking.cpp
  src/media_format.cpp
  src/media_meta.cpp
  src/media_kind.cpp
  src/review_queue.cpp
  src/review_session.cpp
  src/fixture_catalog.cpp
  src/media_analysis.cpp
  src/preference_model.cpp
  src/quality.cpp
  src/pdq.cpp
  src/embeddings.cpp
  src/taste.cpp
  src/delete_model.cpp
  src/link/frames.cpp
  src/link/messages.cpp
  src/link/qr.cpp
)

target_include_directories(pms_core PUBLIC include)
target_link_libraries(pms_core PRIVATE pms_pdq)

# Phone link Noise session (noise-c). A library of its own so that core/tests can build a second copy with the
# fixed-ephemeral test hook (PMS_LINK_TEST_HOOKS); this copy, the one the app links, has no hook. Desktop and fuzz
# targets link pms_link (pms_core alone has no session). The test copy compiles into the inline namespace
# pms::link::testing_hooks (PMS_LINK_ABI_BEGIN in session.hpp), so its symbols never collide with these: a binary that
# ends up with both, by any link path, keeps both and each caller gets the copy it was compiled against.
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
