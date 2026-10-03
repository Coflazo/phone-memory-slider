#include <cmath>
#include <limits>
#include <vector>

#include "pms/media_analysis.hpp"
#include "test_harness.hpp"

void run_media_analysis_tests() {
    {
        const pms::FixedEmbedding embedding{
            -1.0F,
            0.25F,
            2.0F,
            std::numeric_limits<float>::quiet_NaN(),
        };
        pms_check(embedding[0] == 0.0F, "embedding clamps negative values");
        pms_check(embedding[1] == 0.25F, "embedding preserves normalized values");
        pms_check(embedding[2] == 1.0F, "embedding clamps values above one");
        pms_check(embedding[3] == 0.0F, "embedding replaces non-finite values");
        for (const auto value : embedding.values()) {
            pms_check(std::isfinite(value) && value >= 0.0F && value <= 1.0F,
                      "every embedding component is finite and normalized");
        }
    }

    {
        std::vector<pms::MediaObservation> observations{
            {.asset_id = "favorite-original",
             .embedding = pms::FixedEmbedding{0.9F},
             .perceptual_hash = 42,
             .content_digest = "same",
             .storage_benefit = 0.3F,
             .favorite = true},
            {.asset_id = "duplicate-copy",
             .embedding = pms::FixedEmbedding{0.9F},
             .perceptual_hash = 42,
             .content_digest = "same",
             .storage_benefit = 0.3F},
            {.asset_id = "near-copy",
             .embedding = pms::FixedEmbedding{0.8F},
             .perceptual_hash = 42,
             .content_digest = "different",
             .storage_benefit = 0.2F},
            {.asset_id = "kept",
             .embedding = pms::FixedEmbedding{0.7F},
             .perceptual_hash = 7,
             .content_digest = "kept",
             .blur = 1.0F,
             .explicit_keep = true},
        };

        const auto analyzed = pms::analyze_media(observations);
        pms_check(analyzed.size() == observations.size(), "analysis preserves every observation");
        pms_check(analyzed[0].features.favorite && analyzed[0].features.cleanup_confidence == 0.0F,
                  "favorite representative is protected from cleanup evidence");
        pms_check(analyzed[1].has_reason(pms::ReasonCode::ExactDuplicate),
                  "confirmed copy is tagged as an exact duplicate");
        pms_check(analyzed[2].has_reason(pms::ReasonCode::NearDuplicate),
                  "same perceptual bucket with a different digest is tagged near duplicate");
        pms_check(analyzed[3].features.explicit_keep && analyzed[3].features.cleanup_confidence == 0.0F,
                  "explicit keep remains protected even when blurry");
    }
}
