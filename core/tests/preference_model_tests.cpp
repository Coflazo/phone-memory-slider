#include <vector>

#include "pms/preference_model.hpp"
#include "test_harness.hpp"

namespace {

pms::FixedEmbedding embedding(const float first, const float second = 0.0F) {
    return pms::FixedEmbedding{{first, second}};
}

}  // namespace

void run_preference_model_tests() {
    {
        std::vector<pms::PreferenceExample> favorites(19, {embedding(0.9F), pms::PreferenceLabel::Favorite});
        pms::PreferenceModel model;
        model.train(favorites);
        pms_check(!model.enabled(), "nineteen favorites do not enable personal weighting");
        favorites.push_back({embedding(0.8F), pms::PreferenceLabel::Favorite});
        model.train(favorites);
        pms_check(model.enabled(), "twenty favorites enable personal weighting");
    }

    {
        std::vector<pms::PreferenceExample> calibration;
        calibration.reserve(30);
        for (auto index = 0; index < 29; ++index) {
            calibration.push_back({embedding(index % 2 == 0 ? 0.9F : 0.1F),
                                   index % 2 == 0 ? pms::PreferenceLabel::Keep : pms::PreferenceLabel::Delete});
        }
        pms::PreferenceModel model;
        model.train(calibration);
        pms_check(!model.enabled(), "twenty-nine explicit labels do not enable personal weighting");
        calibration.push_back({embedding(0.1F), pms::PreferenceLabel::Delete});
        model.train(calibration);
        pms_check(model.enabled(), "thirty explicit labels enable personal weighting");
        pms_check(model.score(embedding(0.9F)) > model.score(embedding(0.1F)),
                  "calibration scores kept-looking media above deleted-looking media");
    }

    {
        std::vector<pms::PreferenceExample> labels;
        for (auto index = 0; index < 20; ++index) {
            labels.push_back({embedding(0.85F, 0.8F), pms::PreferenceLabel::Favorite});
        }
        pms::PreferenceModel baseline;
        baseline.train(labels);
        labels.push_back({embedding(0.0F, 0.0F), pms::PreferenceLabel::Unlabeled});
        pms::PreferenceModel with_unlabeled;
        with_unlabeled.train(labels);
        pms_check(baseline.score(embedding(0.82F, 0.78F)) ==
                      with_unlabeled.score(embedding(0.82F, 0.78F)),
                  "unlabeled media does not become negative training data");
    }

    {
        std::vector<pms::PreferenceExample> labels;
        for (auto index = 0; index < 15; ++index) {
            labels.push_back({embedding(0.9F, 0.8F), pms::PreferenceLabel::Keep});
            labels.push_back({embedding(0.1F, 0.2F), pms::PreferenceLabel::Delete});
        }
        pms::PreferenceModel first;
        pms::PreferenceModel second;
        first.train(labels);
        second.train(labels);
        const auto before = first.score(embedding(0.45F, 0.45F));
        pms_check(before == second.score(embedding(0.45F, 0.45F)),
                  "training is deterministic for identical ordered labels");
        first.update({embedding(0.45F, 0.45F), pms::PreferenceLabel::Keep});
        pms_check(first.score(embedding(0.45F, 0.45F)) > before,
                  "an explicit keep incrementally raises the matching score");
        const auto after_keep = first.score(embedding(0.05F, 0.05F));
        first.update({embedding(0.05F, 0.05F), pms::PreferenceLabel::Delete});
        pms_check(first.score(embedding(0.05F, 0.05F)) < after_keep,
                  "an explicit delete incrementally lowers the matching score");
    }
}
