#pragma once

#include "vividcam/virtual_camera_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace vividcam {

enum class MediaSourceState { Stopped, Running, Shutdown };

struct MediaSampleRequest {
  std::uint64_t token{0};
};

struct MediaSampleResponse {
  std::uint64_t token{0};
  VirtualCameraSample sample;
  bool discontinuity{false};
};

struct MediaSourceStatistics {
  std::uint64_t requested_samples{0};
  std::uint64_t fulfilled_samples{0};
  std::uint64_t rejected_requests{0};
  std::uint64_t starved_pumps{0};
  std::uint64_t flushed_requests{0};
};

class VirtualCameraMediaSourceCore {
 public:
  explicit VirtualCameraMediaSourceCore(
      OutputProfile profile = default_profile(Platform::Soop),
      std::size_t maximum_pending_requests = 8);

  [[nodiscard]] bool configure(const OutputProfile& profile, std::string& error);
  [[nodiscard]] bool start(std::string& error);
  [[nodiscard]] bool stop(std::string& error);
  void shutdown();
  [[nodiscard]] bool request_sample(MediaSampleRequest request, std::string& error);
  [[nodiscard]] bool submit_frame(CompositedFrame frame, std::string& error);
  [[nodiscard]] std::optional<MediaSampleResponse> pump();
  [[nodiscard]] std::size_t flush();

  [[nodiscard]] MediaSourceState state() const;
  [[nodiscard]] std::size_t pending_requests() const;
  [[nodiscard]] MediaSourceStatistics statistics() const;
  [[nodiscard]] VirtualCameraStatistics stream_statistics() const;

 private:
  mutable std::mutex mutex_;
  VirtualCameraStream stream_;
  std::deque<MediaSampleRequest> requests_;
  std::size_t maximum_pending_requests_{8};
  MediaSourceState state_{MediaSourceState::Stopped};
  bool discontinuity_pending_{true};
  MediaSourceStatistics statistics_;
};

} // namespace vividcam
