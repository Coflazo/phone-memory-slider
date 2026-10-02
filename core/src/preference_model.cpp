#include "pms/preference_model.hpp"

#include <algorithm>
#include <cmath>

namespace pms {
namespace {

[[nodiscard]] bool positive_label(const PreferenceLabel label) noexcept {
    return label == PreferenceLabel::Favorite || label == PreferenceLabel::Keep;
}

[[nodiscard]] bool explicit_label(const PreferenceLabel label) noexcept {
    return label == PreferenceLabel::Keep || label == PreferenceLabel::Delete;
}

[[nodiscard]] float sigmoid(const float value) noexcept {
    if (value >= 0.0F) {
        const auto exponential = std::exp(-value);
        return 1.0F / (1.0F + exponential);
    }
    const auto exponential = std::exp(value);
    return exponential / (1.0F + exponential);
}

}  // namespace

void PreferenceModel::train(const std::span<const PreferenceExample> examples) noexcept {
    *this = PreferenceModel{};
    for (const auto& example : examples) {
        observe_label(example);
    }
    for (auto epoch = 0; epoch < 24; ++epoch) {
        const auto learning_rate = 0.18F / (1.0F + static_cast<float>(epoch) * 0.12F);
        for (const auto& example : examples) {
            apply_gradient(example, learning_rate);
        }
    }
}

void PreferenceModel::update(const PreferenceExample& example) noexcept {
    observe_label(example);
    for (auto step = 0; step < 8; ++step) {
        apply_gradient(example, 0.12F / (1.0F + static_cast<float>(step) * 0.15F));
    }
}

bool PreferenceModel::enabled() const noexcept {
    return favorite_count_ >= 20 || explicit_label_count_ >= 30;
}

float PreferenceModel::score(const FixedEmbedding& embedding) const noexcept {
    if (!enabled()) {
        return 0.5F;
    }
    if (negative_count_ == 0) {
        return one_class_score(embedding);
    }
    auto value = bias_;
    for (std::size_t index = 0; index < embedding_dimensions; ++index) {
        value += weights_[index] * (embedding[index] * 2.0F - 1.0F);
    }
    return std::clamp(sigmoid(value), 0.0F, 1.0F);
}

std::size_t PreferenceModel::favorite_count() const noexcept {
    return favorite_count_;
}

std::size_t PreferenceModel::explicit_label_count() const noexcept {
    return explicit_label_count_;
}

void PreferenceModel::observe_label(const PreferenceExample& example) noexcept {
    if (example.label == PreferenceLabel::Unlabeled) {
        return;
    }
    if (example.label == PreferenceLabel::Favorite) {
        ++favorite_count_;
    }
    if (explicit_label(example.label)) {
        ++explicit_label_count_;
    }
    if (!positive_label(example.label)) {
        ++negative_count_;
        return;
    }

    ++positive_count_;
    for (std::size_t index = 0; index < embedding_dimensions; ++index) {
        const auto delta = example.embedding[index] - positive_mean_[index];
        positive_mean_[index] += delta / static_cast<float>(positive_count_);
        const auto next_delta = example.embedding[index] - positive_mean_[index];
        positive_m2_[index] += delta * next_delta;
    }
}

void PreferenceModel::apply_gradient(const PreferenceExample& example, const float learning_rate) noexcept {
    if (example.label == PreferenceLabel::Unlabeled) {
        return;
    }
    const auto target = positive_label(example.label) ? 1.0F : 0.0F;
    auto value = bias_;
    for (std::size_t index = 0; index < embedding_dimensions; ++index) {
        value += weights_[index] * (example.embedding[index] * 2.0F - 1.0F);
    }
    const auto importance = example.label == PreferenceLabel::Favorite ? 1.0F : 2.0F;
    const auto adjustment = learning_rate * importance * (target - sigmoid(value));
    for (std::size_t index = 0; index < embedding_dimensions; ++index) {
        weights_[index] += adjustment * (example.embedding[index] * 2.0F - 1.0F);
    }
    bias_ += adjustment;
}

float PreferenceModel::one_class_score(const FixedEmbedding& embedding) const noexcept {
    if (positive_count_ == 0) {
        return 0.5F;
    }
    auto normalized_distance = 0.0F;
    for (std::size_t index = 0; index < embedding_dimensions; ++index) {
        const auto variance = positive_count_ > 1
                                  ? positive_m2_[index] / static_cast<float>(positive_count_ - 1)
                                  : 0.0F;
        const auto scale = std::max(variance, 0.01F);
        const auto delta = embedding[index] - positive_mean_[index];
        normalized_distance += delta * delta / scale;
    }
    return std::clamp(std::exp(-normalized_distance / static_cast<float>(embedding_dimensions)), 0.0F, 1.0F);
}

}  // namespace pms
