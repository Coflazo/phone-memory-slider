#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pms {

using EtaClock = std::chrono::steady_clock;

// done is clamped to [0, total] (to >= 0 when the total is unknown); done >= total means the stage finished.
struct StageProgress {
    double done{};
    double total{};
    bool total_known{true};
};

enum class EtaPhase : std::uint8_t {
    Estimating,  // not enough evidence yet, or a stall: show "Estimating..."
    Counting,    // some stage's total is unknown: show the live count `counted` instead of a time
    Remaining,   // show `seconds` (eta_text)
    Done,        // every stage finished
};

struct EtaDisplay {
    EtaPhase phase{EtaPhase::Estimating};
    double seconds{};
    double counted{};
};

// Time left for stages that run concurrently (copy, decode, embed...), which finish when the slowest one does:
// remaining = max over unfinished stages of (total - done) / rate. Finished stages are ignored.
//
// Rate: per stage, samples within the window (plus the last one before it). Progress arrives in bursts (a batch of
// files, one big video), so the rate is measured between the first and the last sample in the window that
// advanced: (done_last - done_first) / (t_last - t_first), exact for a steady stream and for evenly spaced bursts
// alike. Once the time since the first advance exceeds that span plus one average gap between advances, the extra
// wait is counted too, so a slowdown raises the estimate. Fewer than two advances in the window give no rate.
//
// Phases, in order: Done when every stage finished; Counting when a stage that reported has an unknown total
// (the first such stage's done is the count); Estimating while any unfinished stage has not reported, has fewer
// than 20 samples or less than 3 s of them, or advanced fewer than twice in the window (a stall, including a
// producer that stopped calling update); otherwise Remaining.
//
// Hysteresis: the shown seconds change only when the new value differs by more than 10% or 15 s passed since the
// last change; leaving Remaining resets it. A stage whose done goes backwards restarts its evidence.
// ponytail: one sample per update kept for a window; decimate if a stage reports thousands of times a second.
class EtaEstimator {
public:
    explicit EtaEstimator(std::size_t stages, std::chrono::seconds window = std::chrono::seconds(30));
    // Throws std::out_of_range for a stage >= the count given to the constructor. Ignores non-finite values.
    void update(std::size_t stage, StageProgress progress, EtaClock::time_point now);
    [[nodiscard]] EtaDisplay display(EtaClock::time_point now);

private:
    struct Sample {
        EtaClock::time_point time;
        double done;
    };
    struct Stage {
        std::deque<Sample> samples;  // strictly increasing times
        std::size_t count{};         // updates since the stage (re)started
        EtaClock::time_point first{};
        double total{};
        bool total_known{true};
    };
    void trim(Stage& stage, EtaClock::time_point now) const;
    [[nodiscard]] static double rate(const Stage& stage, EtaClock::time_point now);

    std::vector<Stage> stages_;
    EtaClock::duration window_;
    double shown_{};
    EtaClock::time_point shown_at_{};
    bool showing_{};
};

// "less than a minute" (also for negative or NaN), "about 1 minute", "about 7 minutes", "about 1 hour 10 minutes"
// (from 1 hour, minutes rounded to 5), "about 3 hours" (from 3 hours, whole hours).
[[nodiscard]] std::string eta_text(double seconds);

}  // namespace pms
