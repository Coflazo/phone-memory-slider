add_library(pms_core STATIC
  src/ranking.cpp
  src/review_queue.cpp
  src/review_session.cpp
  src/fixture_catalog.cpp
)

target_include_directories(pms_core PUBLIC include)
