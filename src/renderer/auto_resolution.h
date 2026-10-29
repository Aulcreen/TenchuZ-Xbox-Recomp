// tenchuz - automatic internal resolution (--tenchuz_native_resolution_scale=0).
//
// Picks the resolution scale (1-4 times the game's 720p) from how long the GPU
// takes per frame: the highest the GPU can draw with headroom for the frame
// rate, up to the display's own resolution. Changing scale makes the render
// targets again (a short hitch), so it changes rarely: it steps down after the
// GPU has been near its budget for a while, up only when the bigger scale's
// estimated cost would leave plenty spare, and not back up to a scale it just
// left for a minute. Only frames of gameplay (3D scenes) count.

#pragma once

#include <chrono>
#include <cstdint>

namespace tenchuz::native {

class AutoResolution {
 public:
  using Clock = std::chrono::steady_clock;

  // One frame's GPU time. `budget_ms`: one frame at the target frame rate;
  // `largest`: the highest scale worth drawing (the display's).
  void AddFrame(double gpu_ms, double budget_ms, uint32_t largest, Clock::time_point now);

  // The scale to draw at (starts at the largest seen).
  uint32_t scale() const { return scale_; }

 private:
  void Change(uint32_t to, Clock::time_point now);

  uint32_t scale_ = 0;
  double average_ms_ = 0;          // smoothed GPU time of the current scale
  uint32_t frames_ = 0;            // since the last change
  Clock::time_point since_;        // the last change
  Clock::time_point over_since_;   // first frame of the current run over budget
  Clock::time_point under_since_;  // first frame of the current run with room to grow
  bool over_ = false, under_ = false;
  uint32_t failed_ = 0;            // the scale last stepped down from
  Clock::time_point failed_at_;
  // up_cost_[s]: how much more scale s + 1 costs than s, learnt when a step
  // down measures both (0: not yet, estimated from the pixel counts).
  double up_cost_[5] = {};
  double left_ms_ = 0;  // the GPU time at the scale stepped down from, until the new one is measured
};

}  // namespace tenchuz::native
