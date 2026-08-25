#include "vividcam/virtual_camera_stream.hpp"

#include <utility>

namespace vividcam {
namespace {
constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;
}

VirtualCameraStream::VirtualCameraStream(OutputProfile profile) : profile_(profile) {
  if (!profile_.valid()) profile_ = default_profile(Platform::Soop);
}

bool VirtualCameraStream::configure(const OutputProfile& profile, std::string& error) {
  if (!profile.valid()) {
    error = "Virtual camera output profile is invalid";
    return false;
  }
  std::scoped_lock lock(mutex_);
  profile_ = profile;
  pending_.reset();
  last_delivered_.reset();
  sample_index_ = 0;
  statistics_ = {};
  return true;
}

bool VirtualCameraStream::submit(CompositedFrame frame, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (!matches_profile(frame)) {
    ++statistics_.rejected_frames;
    error = "Composited frame does not match the virtual camera profile";
    return false;
  }
  if (pending_) ++statistics_.overwritten_frames;
  pending_ = std::move(frame);
  ++statistics_.submitted_frames;
  return true;
}

std::optional<VirtualCameraSample> VirtualCameraStream::request_sample() {
  std::scoped_lock lock(mutex_);
  bool repeated = false;
  if (pending_) {
    last_delivered_ = std::move(pending_);
    pending_.reset();
  } else if (last_delivered_) {
    repeated = true;
    ++statistics_.repeated_samples;
  } else {
    ++statistics_.underruns;
    return std::nullopt;
  }

  const auto frame_rate = static_cast<std::uint64_t>(profile_.frames_per_second);
  const auto timestamp = static_cast<std::int64_t>(
      sample_index_ * static_cast<std::uint64_t>(kHundredNanosecondsPerSecond) / frame_rate);
  const auto next_timestamp = static_cast<std::int64_t>(
      (sample_index_ + 1) * static_cast<std::uint64_t>(kHundredNanosecondsPerSecond) /
      frame_rate);
  VirtualCameraSample sample{*last_delivered_,
                             timestamp, next_timestamp - timestamp, repeated};
  ++sample_index_;
  ++statistics_.delivered_samples;
  return sample;
}

void VirtualCameraStream::reset() {
  std::scoped_lock lock(mutex_);
  pending_.reset();
  last_delivered_.reset();
  sample_index_ = 0;
  statistics_ = {};
}

OutputProfile VirtualCameraStream::profile() const {
  std::scoped_lock lock(mutex_);
  return profile_;
}

VirtualCameraStatistics VirtualCameraStream::statistics() const {
  std::scoped_lock lock(mutex_);
  return statistics_;
}

bool VirtualCameraStream::matches_profile(const CompositedFrame& frame) const noexcept {
  return frame.width == profile_.width && frame.height == profile_.height &&
         frame.native_texture != 0 && frame.texture_owner;
}

} // namespace vividcam
