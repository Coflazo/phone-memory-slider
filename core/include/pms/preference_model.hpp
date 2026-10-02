#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "pms/media_analysis.hpp"

namespace pms {

enum class PreferenceLabel : unsigned char { Unlabeled, Favorite, Keep, Delete };

struct PreferenceExample {
    FixedEmbedding embedding;
    PreferenceLabel label{PreferenceLabel::Unlabeled};
};

class PreferenceModel final {
public:
    void train(std::span<const PreferenceExample> examples) noexcept;
    void update(const PreferenceExample& example) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] float score(const FixedEmbedding& embedding) const noexcept;
    [[nodiscard]] std::size_t favorite_count() const noexcept;
    [[nodiscard]] std::size_t explicit_label_count() const noexcept;

private:
    void observe_label(const PreferenceExample& example) noexcept;
    void apply_gradient(const PreferenceExample& example, float learning_rate) noexcept;
    [[nodiscard]] float one_class_score(const FixedEmbedding& embedding) const noexcept;

    std::array<float, embedding_dimensions> weights_{};
    std::array<float, embedding_dimensions> positive_mean_{};
    std::array<float, embedding_dimensions> positive_m2_{};
    float bias_{};
    std::size_t favorite_count_{};
    std::size_t explicit_label_count_{};
    std::size_t positive_count_{};
    std::size_t negative_count_{};
};

}  // namespace pms
