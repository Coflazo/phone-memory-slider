#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>
#include "pms/eta.hpp"
#include "test_harness.hpp"

namespace {

struct Trace {
    double median_error{};               // median |shown - true| / true from 25% progress until done (1 if not shown)
    std::vector<pms::EtaDisplay> shown;  // one display per second
    int end{};                           // the first second with done >= total
};

// One stage reporting once a second; the true remaining time at second s is end - s.
Trace run_trace(const std::function<double(int)>& done_at, const double total) {
    pms::EtaEstimator eta(1);
    Trace trace;
    while (done_at(trace.end) < total) ++trace.end;
    std::vector<double> errors;
    for (int s = 0; s <= trace.end; ++s) {
        const double done = std::min(done_at(s), total);
        const auto now = pms::EtaClock::time_point{} + std::chrono::seconds(s);
        eta.update(0, {done, total}, now);
        const auto d = eta.display(now);
        trace.shown.push_back(d);
        if (done >= 0.25 * total && s < trace.end) {
            const double truth = trace.end - s;
            errors.push_back(d.phase == pms::EtaPhase::Remaining ? std::abs(d.seconds - truth) / truth : 1.0);
        }
    }
    const auto middle = errors.begin() + static_cast<std::ptrdiff_t>(errors.size() / 2);
    std::ranges::nth_element(errors, middle);
    trace.median_error = *middle;
    return trace;
}

}  // namespace

void run_eta_tests() {
    using namespace std::chrono;
    using pms::EtaPhase;
    pms::EtaEstimator eta(2);
    auto t = pms::EtaClock::time_point{};
    eta.update(0, {0, 1000}, t);
    pms_check(eta.display(t).phase == EtaPhase::Estimating, "starts estimating");
    for (int s = 1; s <= 40; ++s) {  // stage 0: 10 items/s of 1000; stage 1: 50/s of 1000 (not the bottleneck)
        t += seconds(1);
        eta.update(0, {10.0 * s, 1000}, t);
        eta.update(1, {50.0 * s, 1000}, t);
    }
    auto d = eta.display(t);
    pms_check(d.phase == EtaPhase::Remaining && d.seconds > 54 && d.seconds < 66, "bottleneck stage sets the ETA (600 s left of 1000/10)");
    // Hysteresis: a small speed wobble does not change the shown value.
    const double shown = d.seconds;
    t += seconds(1); eta.update(0, {410.5, 1000}, t);
    pms_check(eta.display(t).seconds == shown, "small changes do not jitter the display");
    // Stall: no progress for the whole window.
    for (int s = 0; s < 31; ++s) { t += seconds(1); eta.update(0, {410.5, 1000}, t); }
    pms_check(eta.display(t).phase == EtaPhase::Estimating, "a stall falls back to Estimating");
    // Unknown total counts instead of guessing.
    pms::EtaEstimator scan(1);
    scan.update(0, {12431, 0, false}, t);
    const auto c = scan.display(t);
    pms_check(c.phase == EtaPhase::Counting && c.counted == 12431, "unknown totals show a live count");
    pms_check(pms::eta_text(30) == "less than a minute" && pms::eta_text(65) == "about 1 minute", "short wording");
    pms_check(pms::eta_text(4200) == "about 1 hour 10 minutes", "hour wording");

    // Synthetic traces. Constant rate and ramps: median error of the shown seconds <= 20% from 25% progress on.
    const auto constant = run_trace([](int s) { return 4.0 * s; }, 2000);
    pms_check(constant.median_error <= 0.2, "constant rate: median error <= 20%");
    const auto warm_up = run_trace([](int s) { return s < 100 ? s + 2.0 * s * s / 100 : 300 + 5.0 * (s - 100); }, 2000);
    pms_check(warm_up.median_error <= 0.2, "rate ramping 1 -> 5 items/s over 100 s, then steady: median error <= 20%");
    const auto ramp = run_trace([](int s) { return 2.0 * s + s * s / 500.0; }, 1500);
    pms_check(ramp.median_error <= 0.2, "rate doubling linearly over the run: median error <= 20%");

    // Bursts: 100 items every 7 s (a period that does not divide the 30 s window). The shown value never goes up.
    const auto burst = run_trace([](int s) { return 100.0 * (s / 7); }, 6000);
    pms_check(burst.median_error <= 0.2, "bursts: median error <= 20%");
    double previous = std::numeric_limits<double>::infinity();
    int changes = 0;
    for (const auto& shown_now : burst.shown) {
        if (shown_now.phase != EtaPhase::Remaining) continue;
        pms_check(shown_now.seconds <= previous, "bursts at a steady average rate never raise the shown ETA");
        if (shown_now.seconds != previous && std::isfinite(previous)) ++changes;
        previous = shown_now.seconds;
    }
    pms_check(changes <= burst.end / 10, "bursts change the shown ETA rarely");

    // Stall in the middle of a run: 5 items/s, nothing from 100 s to 180 s (updates keep coming), then 5 items/s.
    pms::EtaEstimator stall(1);
    for (int s = 0; s < 300; ++s) {
        const double done = s < 100 ? 5.0 * s : (s < 180 ? 500.0 : 500.0 + 5.0 * (s - 180));
        const auto now = pms::EtaClock::time_point{} + seconds(s);
        stall.update(0, {done, 2000}, now);
        const auto shown_now = stall.display(now);
        pms_check(shown_now.seconds >= 0, "the ETA is never negative");
        if (s == 99) pms_check(shown_now.phase == EtaPhase::Remaining, "steady progress shows a time");
        if (s >= 130 && s <= 180) pms_check(shown_now.phase == EtaPhase::Estimating, "a window without progress shows Estimating");
        if (s > 100 && s < 129 && shown_now.phase == EtaPhase::Remaining)
            pms_check(shown_now.seconds >= 300, "while stalling, the ETA rises instead of freezing or falling");
        if (s >= 185) pms_check(shown_now.phase == EtaPhase::Remaining && std::abs(shown_now.seconds - (2000 - done) / 5) < 0.15 * ((2000 - done) / 5), "progress resuming shows a time again");
    }
    // A producer that stops calling update: display alone falls back to Estimating after one window.
    pms::EtaEstimator silent(1);
    for (int s = 0; s < 60; ++s) silent.update(0, {5.0 * s, 2000}, pms::EtaClock::time_point{} + seconds(s));
    pms_check(silent.display(pms::EtaClock::time_point{} + seconds(60)).phase == EtaPhase::Remaining, "a live producer shows a time");
    pms_check(silent.display(pms::EtaClock::time_point{} + seconds(90)).phase == EtaPhase::Estimating, "a silent producer shows Estimating");

    // Ruling R3: done clamps to [0, total]; finished stages are ignored; every stage finished is Done.
    pms::EtaEstimator clamp(2);
    const auto t0 = pms::EtaClock::time_point{};
    clamp.update(0, {-5, 100}, t0);
    clamp.update(1, {250, 100}, t0);
    pms_check(clamp.display(t0).phase == EtaPhase::Estimating, "an unfinished stage without evidence keeps Estimating");
    clamp.update(0, {100, 100}, t0 + seconds(1));
    pms_check(clamp.display(t0 + seconds(1)).phase == EtaPhase::Done, "every stage finished is Done");
    // A stage that never reported keeps Estimating; an unknown total wins over it.
    pms::EtaEstimator partial(2);
    partial.update(0, {100, 100}, t0);
    pms_check(partial.display(t0).phase == EtaPhase::Estimating, "a stage that never reported keeps Estimating");
    partial.update(1, {42, 0, false}, t0);
    const auto counting = partial.display(t0);
    pms_check(counting.phase == EtaPhase::Counting && counting.counted == 42, "an unknown total shows the count");
    // Progress going backwards restarts the evidence.
    pms::EtaEstimator backwards(1);
    for (int s = 0; s <= 40; ++s) backwards.update(0, {10.0 * s, 1000}, t0 + seconds(s));
    pms_check(backwards.display(t0 + seconds(40)).phase == EtaPhase::Remaining, "steady progress shows a time");
    backwards.update(0, {100, 1000}, t0 + seconds(41));
    pms_check(backwards.display(t0 + seconds(41)).phase == EtaPhase::Estimating, "progress going backwards re-estimates");
    bool threw = false;
    try {
        backwards.update(1, {0, 1}, t0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    pms_check(threw, "an unknown stage throws");

    // Wording.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    pms_check(pms::eta_text(0) == "less than a minute" && pms::eta_text(59.9) == "less than a minute" &&
                  pms::eta_text(-3) == "less than a minute" && pms::eta_text(nan) == "less than a minute", "under a minute");
    pms_check(pms::eta_text(60) == "about 1 minute" && pms::eta_text(89) == "about 1 minute" && pms::eta_text(90) == "about 2 minutes", "minutes round");
    pms_check(pms::eta_text(420) == "about 7 minutes" && pms::eta_text(3540) == "about 59 minutes", "minutes");
    pms_check(pms::eta_text(3570) == "about 1 hour" && pms::eta_text(7200) == "about 2 hours", "whole hours");
    pms_check(pms::eta_text(4320) == "about 1 hour 10 minutes" && pms::eta_text(9000) == "about 2 hours 30 minutes", "hours and 5-minute steps");
    pms_check(pms::eta_text(10800) == "about 3 hours" && pms::eta_text(11000) == "about 3 hours" && pms::eta_text(12000) == "about 3 hours", "from 3 hours, whole hours");
    pms_check(pms::eta_text(std::numeric_limits<double>::infinity()).starts_with("about "), "infinity stays defined");
}
