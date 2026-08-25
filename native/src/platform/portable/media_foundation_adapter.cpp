#include "vividcam/media_foundation_adapter.hpp"

namespace vividcam {

NativeMediaFoundationHandle create_media_foundation_media_type(
    const VirtualCameraMediaType&, std::string& error) {
  error = "Media Foundation media types are available on Windows only";
  return {};
}

NativeMediaFoundationHandle create_media_foundation_gpu_sample(
    const ConvertedGpuFrame&, std::int64_t, std::int64_t, bool, std::string& error) {
  error = "Media Foundation GPU samples are available on Windows only";
  return {};
}

NativeMediaFoundationHandle create_media_foundation_event_queue(std::string& error) {
  error = "Media Foundation event queues are available on Windows only";
  return {};
}

bool queue_media_foundation_event(const NativeMediaFoundationHandle&,
                                  MediaFoundationEventKind,
                                  const NativeMediaFoundationHandle&, std::int32_t,
                                  std::string& error) {
  error = "Media Foundation event queues are available on Windows only";
  return false;
}

NativeMediaFoundationHandle take_media_foundation_event(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation event queues are available on Windows only";
  return {};
}

bool shutdown_media_foundation_event_queue(const NativeMediaFoundationHandle&,
                                            std::string& error) {
  error = "Media Foundation event queues are available on Windows only";
  return false;
}

NativeMediaFoundationHandle create_media_foundation_stream_descriptor(
    std::uint32_t, const std::vector<NativeMediaFoundationHandle>&,
    std::string& error) {
  error = "Media Foundation stream descriptors are available on Windows only";
  return {};
}

NativeMediaFoundationHandle create_media_foundation_presentation_descriptor(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation presentation descriptors are available on Windows only";
  return {};
}

} // namespace vividcam
