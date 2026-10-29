// tenchuz - automatic internal resolution. See auto_resolution.h.

#include "auto_resolution.h"

#include <algorithm>

#include <rex/logging.h>

namespace tenchuz::native {
namespace {

constexpr double kHigh = 0.85;  // of the budget: step down past this
constexpr double kLow = 0.60;   // of the budget: the bigger scale's estimate must stay under this
constexpr auto kDownAfter = std::chrono::seconds(2);
constexpr auto kUpAfter = std::chrono::seconds(5);
constexpr auto kSettle = std::chrono::seconds(10);  // after a change, before another
constexpr auto kRetryAfter = std::chrono::seconds(60);  // before going back up to a scale left for being slow
constexpr uint32_t kWarmFrames = 30;  // frames after a change before measuring (pipelines, new targets)

}  // namespace

void AutoResolution::Change(uint32_t to, Clock::time_point now) {
  REXLOG_INFO("tenchuz: auto resolution {}x -> {}x (GPU {:.1f} ms a frame)", scale_, to, average_ms_);
  if (to < scale_) {
    failed_ = scale_;
    failed_at_ = now;
    left_ms_ = average_ms_;
  } else {
    left_ms_ = 0;
  }
  scale_ = to;
  average_ms_ = 0;
  frames_ = 0;
  over_ = under_ = false;
  since_ = now;
}

void AutoResolution::AddFrame(double gpu_ms, double budget_ms, uint32_t largest, Clock::time_point now) {
  largest = std::clamp(largest, 1u, 4u);
  if (scale_ == 0) {
    scale_ = largest;
    since_ = now;
    REXLOG_INFO("tenchuz: auto resolution starts at {}x", scale_);
    return;
  }
  if (scale_ > largest) {  // moved to a smaller display
    Change(largest, now);
    return;
  }
  if (++frames_ <= kWarmFrames) return;
  average_ms_ = average_ms_ == 0 ? gpu_ms : average_ms_ * 0.95 + gpu_ms * 0.05;
  if (now - since_ < kSettle) return;
  if (left_ms_ > 0) {  // the step down measured: what going back up costs
    up_cost_[scale_] = std::max(1.0, left_ms_ / average_ms_);
    left_ms_ = 0;
  }

  // Too slow: one step down once it has lasted.
  if (average_ms_ > kHigh * budget_ms && scale_ > 1) {
    if (!over_) {
      over_ = true;
      over_since_ = now;
    } else if (now - over_since_ >= kDownAfter) {
      Change(scale_ - 1, now);
    }
    return;
  }
  over_ = false;

  // Room to spare: the next scale's cost, as learnt, or estimated by its
  // pixel count (an overestimate: part of a frame's work doesn't grow).
  const uint32_t next = scale_ + 1;
  const double up_cost = up_cost_[scale_] > 0 ? up_cost_[scale_] : double(next * next) / double(scale_ * scale_);
  const double estimate = average_ms_ * up_cost;
  const bool retry_ok = next != failed_ || now - failed_at_ >= kRetryAfter;
  if (next <= largest && retry_ok && estimate < kLow * budget_ms) {
    if (!under_) {
      under_ = true;
      under_since_ = now;
    } else if (now - under_since_ >= kUpAfter) {
      Change(next, now);
    }
    return;
  }
  under_ = false;
}

}  // namespace tenchuz::native
