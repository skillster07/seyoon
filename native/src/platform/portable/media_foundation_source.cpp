#include "vividcam/media_foundation_source.hpp"

namespace vividcam {

NativeMediaFoundationHandle create_media_foundation_virtual_camera_source(
    const OutputProfile&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return {};
}

bool submit_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle&, const NativeMediaFoundationHandle&,
    std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return false;
}

bool start_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return false;
}

bool request_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return false;
}

NativeMediaFoundationHandle take_media_foundation_virtual_camera_stream_event(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return {};
}

bool stop_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return false;
}

bool shutdown_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = "Media Foundation virtual camera sources are available on Windows only";
  return false;
}

} // namespace vividcam
