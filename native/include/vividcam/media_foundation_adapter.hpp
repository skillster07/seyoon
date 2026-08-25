#pragma once

#include "vividcam/gpu_pixel_converter.hpp"
#include "vividcam/virtual_camera_media_source.hpp"
#include "vividcam/virtual_camera_media_type.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vividcam {

struct NativeMediaFoundationHandle {
  std::shared_ptr<void> owner;
  std::uintptr_t native_pointer{0};

  [[nodiscard]] bool valid() const noexcept { return owner && native_pointer != 0; }
};

enum class MediaFoundationEventKind { StreamStarted, StreamStopped, MediaSample, Error };

[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_media_type(
    const VirtualCameraMediaType& media_type, std::string& error);
[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_gpu_sample(
    const ConvertedGpuFrame& frame, std::int64_t timestamp_100ns,
    std::int64_t duration_100ns, bool discontinuity, std::string& error);
[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_event_queue(
    std::string& error);
[[nodiscard]] bool queue_media_foundation_event(
    const NativeMediaFoundationHandle& queue, MediaFoundationEventKind kind,
    const NativeMediaFoundationHandle& payload, std::int32_t status_code,
    std::string& error);
[[nodiscard]] NativeMediaFoundationHandle take_media_foundation_event(
    const NativeMediaFoundationHandle& queue, std::string& error);
[[nodiscard]] bool shutdown_media_foundation_event_queue(
    const NativeMediaFoundationHandle& queue, std::string& error);
[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_stream_descriptor(
    std::uint32_t stream_id,
    const std::vector<NativeMediaFoundationHandle>& media_types,
    std::string& error);
[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_presentation_descriptor(
    const NativeMediaFoundationHandle& stream_descriptor, std::string& error);

} // namespace vividcam
