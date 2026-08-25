#include "vividcam/frame_scheduler.hpp"

#include <stdexcept>

namespace vividcam {

FrameScheduler::FrameScheduler(std::uint32_t frames_per_second) {
  if (frames_per_second == 0 || frames_per_second > 240) {
    throw std::invalid_argument("frames_per_second must be between 1 and 240");
  }
  frame_period_ = std::chrono::nanoseconds{1'000'000'000LL / frames_per_second};
  reset(Clock::now());
}

void FrameScheduler::reset(TimePoint now) noexcept {
  epoch_ = now;
  frame_index_ = 0;
  dropped_frames_ = 0;
}

FrameScheduler::TimePoint FrameScheduler::next_deadline() const noexcept {
  return epoch_ + frame_period_ * static_cast<std::int64_t>(frame_index_ + 1);
}

std::uint64_t FrameScheduler::advance(TimePoint completed_at) noexcept {
  ++frame_index_;
  const auto elapsed = completed_at - epoch_;
  if (elapsed <= std::chrono::nanoseconds::zero()) return 0;

  const auto expected_index = static_cast<std::uint64_t>(elapsed / frame_period_);
  if (expected_index <= frame_index_) return 0;

  const auto dropped_now = expected_index - frame_index_;
  dropped_frames_ += dropped_now;
  frame_index_ = expected_index;
  return dropped_now;
}

} // namespace vividcam
