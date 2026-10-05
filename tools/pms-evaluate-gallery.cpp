#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "pms/media_analysis.hpp"
#include "pms/preference_model.hpp"
#include "visual_encoder.hpp"

namespace {

struct EvaluatedImage {
    QString path;
    int sourceId{};
    QString kind;
    pms::desktop::VisualEncoding visual;
    std::string digest;
};

[[nodiscard]] QByteArray read_all(const QString& path) {
    QFile file{path};
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

[[nodiscard]] bool has_duplicate_reason(const pms::AnalysisResult& result) {
    return result.has_reason(pms::ReasonCode::ExactDuplicate) || result.has_reason(pms::ReasonCode::NearDuplicate);
}

[[nodiscard]] double safe_ratio(const std::size_t numerator, const std::size_t denominator) {
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application{argc, argv};
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: pms_evaluate_gallery <gallery-directory> [report.json]\n";
        return 2;
    }
    const QDir root{QString::fromLocal8Bit(argv[1])};
    const auto manifest_bytes = read_all(root.filePath(QStringLiteral("manifest.json")));
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(manifest_bytes, &parse_error);
    if (document.isNull() || !document.isObject()) {
        std::cerr << "invalid manifest: " << parse_error.errorString().toStdString() << '\n';
        return 2;
    }

    std::vector<EvaluatedImage> images;
    const auto manifest = document.object();
    const auto originals = manifest.value(QStringLiteral("originals")).toArray();
    const auto variants = manifest.value(QStringLiteral("variants")).toArray();
    images.reserve(static_cast<std::size_t>(originals.size() + variants.size()));
    for (const auto& value : originals) {
        const auto object = value.toObject();
        images.push_back({
            root.filePath(object.value(QStringLiteral("file")).toString()),
            object.value(QStringLiteral("id")).toInt(),
            QStringLiteral("original"),
            {},
            {},
        });
    }
    for (const auto& value : variants) {
        const auto object = value.toObject();
        images.push_back({
            root.filePath(object.value(QStringLiteral("file")).toString()),
            object.value(QStringLiteral("source_id")).toInt(),
            object.value(QStringLiteral("kind")).toString(),
            {},
            {},
        });
    }

    QElapsedTimer timer;
    timer.start();
    for (auto& item : images) {
        const auto bytes = read_all(item.path);
        const auto image = QImage::fromData(bytes);
        if (bytes.isEmpty() || image.isNull()) {
            std::cerr << "unreadable image: " << item.path.toStdString() << '\n';
            return 2;
        }
        item.visual = pms::desktop::VisualEncoder::encode(
            image, false, static_cast<std::uint64_t>(bytes.size()), 0);
        item.digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
    }

    std::vector<std::size_t> preference_order(images.size());
    std::iota(preference_order.begin(), preference_order.end(), 0);
    std::ranges::sort(preference_order, [&images](const auto left, const auto right) {
        return images[left].visual.embedding[0] > images[right].visual.embedding[0];
    });
    std::vector<pms::PreferenceExample> preference_examples;
    preference_examples.reserve(images.size());
    for (std::size_t rank = 0; rank < preference_order.size(); ++rank) {
        auto label = pms::PreferenceLabel::Unlabeled;
        if (rank < 20) {
            label = pms::PreferenceLabel::Favorite;
        } else if (rank + 30 >= preference_order.size()) {
            label = pms::PreferenceLabel::Delete;
        }
        preference_examples.push_back({images[preference_order[rank]].visual.embedding, label});
    }
    pms::PreferenceModel preference;
    preference.train(preference_examples);

    std::vector<float> personal_scores(images.size(), 0.5F);
    for (std::size_t index = 0; index < images.size(); ++index) {
        personal_scores[index] = preference.score(images[index].visual.embedding);
    }
    std::vector<pms::MediaObservation> observations;
    observations.reserve(images.size());
    std::unordered_map<std::size_t, bool> favorite_indices;
    for (const auto index : preference_order | std::views::take(20)) {
        favorite_indices.emplace(index, true);
    }
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto favorite = favorite_indices.contains(index);
        observations.push_back({
            images[index].path.toStdString(),
            images[index].visual.embedding,
            images[index].visual.perceptualHash,
            images[index].digest,
            images[index].visual.blurProblem,
            images[index].visual.exposureProblem,
            images[index].visual.screenshotLikelihood,
            0.2F,
            false,
            favorite,
            false,
        });
    }
    const auto results = pms::analyze_media(observations, personal_scores);
    const auto analysis_ms = timer.elapsed();

    std::unordered_map<int, std::size_t> original_index;
    for (std::size_t index = 0; index < images.size(); ++index) {
        if (images[index].kind == QStringLiteral("original")) {
            original_index.emplace(images[index].sourceId, index);
        }
    }
    std::size_t exact_groups{};
    std::size_t exact_detected{};
    std::size_t near_groups{};
    std::size_t near_detected{};
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto& item = images[index];
        if (item.kind == QStringLiteral("original")) {
            continue;
        }
        const auto source = original_index.at(item.sourceId);
        if (item.kind == QStringLiteral("exact")) {
            ++exact_groups;
            exact_detected += results[index].has_reason(pms::ReasonCode::ExactDuplicate) ||
                                      results[source].has_reason(pms::ReasonCode::ExactDuplicate)
                                  ? 1U
                                  : 0U;
        } else {
            ++near_groups;
            near_detected += has_duplicate_reason(results[index]) || has_duplicate_reason(results[source]) ? 1U : 0U;
        }
    }
    const auto duplicate_marks = static_cast<std::size_t>(std::ranges::count_if(results, has_duplicate_reason));
    const auto valid_groups = exact_detected + near_detected;
    const auto precision_estimate = safe_ratio(valid_groups, std::max(valid_groups, duplicate_marks));
    std::size_t favorite_protection_violations{};
    for (std::size_t index = 0; index < observations.size(); ++index) {
        favorite_protection_violations += observations[index].favorite && results[index].features.cleanup_confidence > 0.0F ? 1U : 0U;
    }

    std::vector<std::size_t> held_out(preference_order.begin() + 20, preference_order.begin() + 25);
    std::vector<std::size_t> comparison(preference_order.begin() + 25, preference_order.end() - 30);
    double favorable_pairs{};
    std::size_t compared_pairs{};
    for (const auto positive : held_out) {
        for (const auto other : comparison) {
            favorable_pairs += personal_scores[positive] > personal_scores[other] ? 1.0 : 0.0;
            favorable_pairs += personal_scores[positive] == personal_scores[other] ? 0.5 : 0.0;
            ++compared_pairs;
        }
    }
    const auto preference_auc = compared_pairs == 0 ? 0.0 : favorable_pairs / static_cast<double>(compared_pairs);

    const QJsonObject report{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("assets"), static_cast<int>(images.size())},
        {QStringLiteral("analysis_ms"), analysis_ms},
        {QStringLiteral("exact_groups"), static_cast<int>(exact_groups)},
        {QStringLiteral("exact_group_recall"), safe_ratio(exact_detected, exact_groups)},
        {QStringLiteral("near_groups"), static_cast<int>(near_groups)},
        {QStringLiteral("near_group_recall"), safe_ratio(near_detected, near_groups)},
        {QStringLiteral("duplicate_precision_estimate"), precision_estimate},
        {QStringLiteral("favorite_protection_violations"), static_cast<int>(favorite_protection_violations)},
        {QStringLiteral("synthetic_preference_auc"), preference_auc},
    };
    const auto output = QJsonDocument{report}.toJson(QJsonDocument::Indented);
    std::cout << output.constData();
    if (argc == 3) {
        QSaveFile file{QString::fromLocal8Bit(argv[2])};
        if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size() || !file.commit()) {
            std::cerr << "could not write report\n";
            return 2;
        }
    }
    return favorite_protection_violations == 0 ? 0 : 1;
}
