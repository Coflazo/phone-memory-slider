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
)

target_include_directories(pms_core PUBLIC include)
