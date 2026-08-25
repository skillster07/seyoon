#pragma once

#include <chrono>
#include <cstdint>

namespace vividcam {

class FrameScheduler {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit FrameScheduler(std::uint32_t frames_per_second = 60);
  void reset(TimePoint now) noexcept;
  [[nodiscard]] TimePoint next_deadline() const noexcept;
  [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }
  [[nodiscard]] std::uint64_t dropped_frames() const noexcept { return dropped_frames_; }
  std::uint64_t advance(TimePoint completed_at) noexcept;

 private:
  std::chrono::nanoseconds frame_period_;
  TimePoint epoch_{};
  std::uint64_t frame_index_{0};
  std::uint64_t dropped_frames_{0};
};

} // namespace vividcam
