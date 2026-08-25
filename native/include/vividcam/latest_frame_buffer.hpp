#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace vividcam {

template <typename Frame>
class LatestFrameBuffer {
 public:
  void push(Frame frame) {
    std::scoped_lock lock(mutex_);
    if (frame_) ++overwritten_frames_;
    frame_ = std::move(frame);
    ++published_frames_;
  }

  [[nodiscard]] std::optional<Frame> take() {
    std::scoped_lock lock(mutex_);
    auto result = std::move(frame_);
    frame_.reset();
    if (result) ++consumed_frames_;
    return result;
  }

  [[nodiscard]] bool has_frame() const {
    std::scoped_lock lock(mutex_);
    return frame_.has_value();
  }

  [[nodiscard]] std::uint64_t published_frames() const {
    std::scoped_lock lock(mutex_);
    return published_frames_;
  }

  [[nodiscard]] std::uint64_t consumed_frames() const {
    std::scoped_lock lock(mutex_);
    return consumed_frames_;
  }

  [[nodiscard]] std::uint64_t overwritten_frames() const {
    std::scoped_lock lock(mutex_);
    return overwritten_frames_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<Frame> frame_;
  std::uint64_t published_frames_{0};
  std::uint64_t consumed_frames_{0};
  std::uint64_t overwritten_frames_{0};
};

} // namespace vividcam
