add_library(pms_core STATIC
  src/ranking.cpp
  src/review_queue.cpp
  src/review_session.cpp
  src/fixture_catalog.cpp
  src/fixture_phone_transport.cpp
  src/media_analysis.cpp
  src/preference_model.cpp
)

target_include_directories(pms_core PUBLIC include)
