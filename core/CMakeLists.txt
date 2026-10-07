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
