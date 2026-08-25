#pragma once

#include "vividcam/media_foundation_adapter.hpp"
#include "vividcam/output_profile.hpp"

#include <string>

namespace vividcam {

enum class MediaFoundationVirtualCameraSourceMode {
  ExternalSubmit,
  SyntheticPattern,
};

[[nodiscard]] NativeMediaFoundationHandle create_media_foundation_virtual_camera_source(
    const OutputProfile& profile, std::string& error,
    MediaFoundationVirtualCameraSourceMode mode =
        MediaFoundationVirtualCameraSourceMode::ExternalSubmit);
[[nodiscard]] bool submit_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle& source,
    const NativeMediaFoundationHandle& sample, std::string& error);
[[nodiscard]] bool start_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source, std::string& error);
[[nodiscard]] bool request_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle& source, std::string& error);
[[nodiscard]] NativeMediaFoundationHandle take_media_foundation_virtual_camera_stream_event(
    const NativeMediaFoundationHandle& source, std::string& error);
[[nodiscard]] bool stop_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source, std::string& error);
[[nodiscard]] bool shutdown_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source, std::string& error);

} // namespace vividcam
