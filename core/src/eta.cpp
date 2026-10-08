#include "pms/eta.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pms {
namespace {

constexpr std::size_t min_samples = 20;
constexpr auto min_history = std::chrono::seconds(3);
constexpr auto hold_time = std::chrono::seconds(15);  // hysteresis: refresh the shown value at least this often
constexpr double min_change = 0.10;                   // ... or when it moves by more than this fraction

double seconds(const EtaClock::duration d) { return std::chrono::duration<double>(d).count(); }

}  // namespace

EtaEstimator::EtaEstimator(const std::size_t stages, const std::chrono::seconds window)
    : stages_(stages), window_(window) {}

void EtaEstimator::update(const std::size_t stage, const StageProgress progress, const EtaClock::time_point now) {
    Stage& s = stages_.at(stage);
    if (!std::isfinite(progress.done) || (progress.total_known && !std::isfinite(progress.total))) return;
    const double total = progress.total_known ? std::max(progress.total, 0.0) : 0.0;
    const double done = progress.total_known ? std::clamp(progress.done, 0.0, total) : std::max(progress.done, 0.0);
    s.total = total;
    s.total_known = progress.total_known;
    if (!s.samples.empty() && done < s.samples.back().done) {  // went backwards: the old evidence no longer applies
        s.samples.clear();
        s.count = 0;
    }
    if (s.samples.empty()) s.first = now;
    if (!s.samples.empty() && now <= s.samples.back().time)
        s.samples.back().done = done;
    else
        s.samples.push_back({now, done});
    ++s.count;
    trim(s, now);
}

// Drop samples older than the window, keeping the last one at or before its start as the baseline.
void EtaEstimator::trim(Stage& stage, const EtaClock::time_point now) const {
    while (stage.samples.size() >= 2 && stage.samples[1].time <= now - window_) stage.samples.pop_front();
}

double EtaEstimator::rate(const Stage& stage, const EtaClock::time_point now) {
    const auto& samples = stage.samples;
    std::size_t first = 0, last = 0, advances = 0;  // samples that moved done forward
    for (std::size_t k = 1; k < samples.size(); ++k)
        if (samples[k].done > samples[k - 1].done) {
            if (advances++ == 0) first = k;
            last = k;
        }
    if (advances < 2) return 0.0;  // no progress in the window, or too little to measure a rate
    const double between = seconds(samples[last].time - samples[first].time);
    const double gap = between / static_cast<double>(advances - 1);  // average time between advances
    const double span = std::max(between, seconds(now - samples[first].time) - gap);
    return (samples[last].done - samples[first].done) / span;
}

EtaDisplay EtaEstimator::display(const EtaClock::time_point now) {
    const auto finished = [](const Stage& s) { return s.total_known && s.samples.back().done >= s.total; };
    const auto estimating = [this] {
        showing_ = false;
        return EtaDisplay{};
    };
    if (std::ranges::all_of(stages_, [&](const Stage& s) { return !s.samples.empty() && finished(s); })) {
        showing_ = false;
        return {EtaPhase::Done, 0.0, 0.0};
    }
    for (const Stage& s : stages_)
        if (!s.samples.empty() && !s.total_known) {
            showing_ = false;
            return {EtaPhase::Counting, 0.0, s.samples.back().done};
        }

    double remaining = 0.0;  // bottleneck: the slowest unfinished stage
    for (Stage& s : stages_) {
        if (s.samples.empty()) return estimating();
        if (finished(s)) continue;
        trim(s, now);
        if (s.count < min_samples || now - s.first < min_history) return estimating();
        const double r = rate(s, now);
        if (!(r > 0.0)) return estimating();  // a stall: no progress for the whole window
        remaining = std::max(remaining, (s.total - s.samples.back().done) / r);
    }

    if (!showing_ || std::abs(remaining - shown_) > min_change * shown_ || now - shown_at_ >= hold_time) {
        shown_ = remaining;
        shown_at_ = now;
        showing_ = true;
    }
    return {EtaPhase::Remaining, shown_, 0.0};
}

std::string eta_text(const double seconds) {
    if (!(seconds >= 60.0)) return "less than a minute";  // also negative and NaN
    const auto count = [](const long long n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
    };
    const long long minutes = std::llround(std::min(seconds / 60.0, 1e9));  // the cap keeps infinity defined
    if (minutes < 60) return "about " + count(minutes, "minute");
    if (minutes < 180) {
        const long long rounded = 5 * std::llround(static_cast<double>(minutes) / 5.0);
        const long long hours = rounded / 60, rest = rounded % 60;
        return "about " + count(hours, "hour") + (rest == 0 ? "" : " " + count(rest, "minute"));
    }
    return "about " + count(std::llround(static_cast<double>(minutes) / 60.0), "hour");
}

}  // namespace pms
