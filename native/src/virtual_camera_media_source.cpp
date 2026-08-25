#include "vividcam/virtual_camera_media_source.hpp"

#include <algorithm>
#include <utility>

namespace vividcam {

VirtualCameraMediaSourceCore::VirtualCameraMediaSourceCore(
    OutputProfile profile, std::size_t maximum_pending_requests)
    : stream_(profile), maximum_pending_requests_(std::max<std::size_t>(1, maximum_pending_requests)) {}

bool VirtualCameraMediaSourceCore::configure(const OutputProfile& profile, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ == MediaSourceState::Shutdown) {
    error = "Media source is shut down";
    return false;
  }
  if (state_ != MediaSourceState::Stopped) {
    error = "Media source must be stopped before changing its profile";
    return false;
  }
  if (!stream_.configure(profile, error)) return false;
  requests_.clear();
  discontinuity_pending_ = true;
  statistics_ = {};
  return true;
}

bool VirtualCameraMediaSourceCore::start(std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ == MediaSourceState::Shutdown) {
    error = "Media source is shut down";
    return false;
  }
  if (state_ == MediaSourceState::Running) return true;
  state_ = MediaSourceState::Running;
  discontinuity_pending_ = true;
  return true;
}

bool VirtualCameraMediaSourceCore::stop(std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ == MediaSourceState::Shutdown) {
    error = "Media source is shut down";
    return false;
  }
  const auto flushed = requests_.size();
  requests_.clear();
  statistics_.flushed_requests += flushed;
  stream_.reset();
  state_ = MediaSourceState::Stopped;
  discontinuity_pending_ = true;
  return true;
}

void VirtualCameraMediaSourceCore::shutdown() {
  std::scoped_lock lock(mutex_);
  statistics_.flushed_requests += requests_.size();
  requests_.clear();
  stream_.reset();
  state_ = MediaSourceState::Shutdown;
  discontinuity_pending_ = true;
}

bool VirtualCameraMediaSourceCore::request_sample(MediaSampleRequest request,
                                                   std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != MediaSourceState::Running) {
    ++statistics_.rejected_requests;
    error = "Media source is not running";
    return false;
  }
  if (requests_.size() >= maximum_pending_requests_) {
    ++statistics_.rejected_requests;
    error = "Media source request queue is full";
    return false;
  }
  requests_.push_back(request);
  ++statistics_.requested_samples;
  return true;
}

bool VirtualCameraMediaSourceCore::submit_frame(CompositedFrame frame, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ == MediaSourceState::Shutdown) {
    error = "Media source is shut down";
    return false;
  }
  return stream_.submit(std::move(frame), error);
}

std::optional<MediaSampleResponse> VirtualCameraMediaSourceCore::pump() {
  std::scoped_lock lock(mutex_);
  if (state_ != MediaSourceState::Running || requests_.empty()) return std::nullopt;
  auto sample = stream_.request_sample();
  if (!sample) {
    ++statistics_.starved_pumps;
    discontinuity_pending_ = true;
    return std::nullopt;
  }
  const auto request = requests_.front();
  requests_.pop_front();
  MediaSampleResponse response{request.token, std::move(*sample), discontinuity_pending_};
  discontinuity_pending_ = false;
  ++statistics_.fulfilled_samples;
  return response;
}

std::size_t VirtualCameraMediaSourceCore::flush() {
  std::scoped_lock lock(mutex_);
  const auto flushed = requests_.size();
  requests_.clear();
  statistics_.flushed_requests += flushed;
  stream_.reset();
  discontinuity_pending_ = true;
  return flushed;
}

MediaSourceState VirtualCameraMediaSourceCore::state() const {
  std::scoped_lock lock(mutex_);
  return state_;
}

std::size_t VirtualCameraMediaSourceCore::pending_requests() const {
  std::scoped_lock lock(mutex_);
  return requests_.size();
}

MediaSourceStatistics VirtualCameraMediaSourceCore::statistics() const {
  std::scoped_lock lock(mutex_);
  return statistics_;
}

VirtualCameraStatistics VirtualCameraMediaSourceCore::stream_statistics() const {
  std::scoped_lock lock(mutex_);
  return stream_.statistics();
}

} // namespace vividcam
