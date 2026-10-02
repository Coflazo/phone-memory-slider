#include <cstdint>
#include <vector>

#include "pms/review_session.hpp"
#include "test_harness.hpp"

void run_review_session_tests() {
    std::vector<pms::ReviewItem> items{
        {{"keep-me", 0.1F, 0.9F, 0.2F, false, false}, pms::MediaKind::Photo, 100, "Keep me", {pms::ReasonCode::LowPersonalMatch}},
        {{"delete-me", 0.9F, 0.1F, 0.8F, false, false}, pms::MediaKind::Video, 900, "Delete me", {pms::ReasonCode::LargeVideo}},
    };
    pms::ReviewSession session{std::move(items)};

    pms_check(session.remaining() == 2, "review session starts with every candidate");
    pms_check(session.current().has_value() && session.current()->asset.asset_id == "delete-me", "session starts at highest priority");
    pms_check(session.decide_current(pms::Decision::Delete), "delete decision advances the deck");
    pms_check(session.current().has_value() && session.current()->asset.asset_id == "keep-me", "next card becomes current");
    pms_check(session.pending_delete_bytes() == 900, "session reports queued storage");
    pms_check(session.decide_current(pms::Decision::Keep), "keep decision completes the deck");
    pms_check(!session.current().has_value() && session.remaining() == 0, "completed deck has no current item");

    pms_check(session.undo(), "undo restores the last card");
    pms_check(session.current().has_value() && session.current()->asset.asset_id == "keep-me", "undo rewinds deck position");
    pms_check(session.pending_delete_bytes() == 900, "undoing keep preserves earlier delete");
}

