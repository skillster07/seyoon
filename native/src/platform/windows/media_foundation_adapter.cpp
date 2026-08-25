#include "vividcam/media_foundation_adapter.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>

#include <memory>

namespace vividcam {
namespace {

template <typename Interface>
NativeMediaFoundationHandle make_handle(Interface* pointer) {
  if (!pointer) return {};
  return {std::shared_ptr<void>(pointer, [](void* value) {
            static_cast<Interface*>(value)->Release();
          }),
          reinterpret_cast<std::uintptr_t>(pointer)};
}

const GUID& subtype_for(VirtualCameraPixelFormat format) {
  return format == VirtualCameraPixelFormat::Nv12 ? MFVideoFormat_NV12
                                                   : MFVideoFormat_ARGB32;
}

MediaEventType native_event_type(MediaFoundationEventKind kind) {
  switch (kind) {
    case MediaFoundationEventKind::StreamStarted: return MEStreamStarted;
    case MediaFoundationEventKind::StreamStopped: return MEStreamStopped;
    case MediaFoundationEventKind::MediaSample: return MEMediaSample;
    case MediaFoundationEventKind::Error: return MEError;
  }
  return MEUnknown;
}
} // namespace

NativeMediaFoundationHandle create_media_foundation_media_type(
    const VirtualCameraMediaType& media_type, std::string& error) {
  if (!media_type.valid()) {
    error = "Cannot create IMFMediaType from an invalid virtual camera media type";
    return {};
  }
  IMFMediaType* type = nullptr;
  HRESULT status = MFCreateMediaType(&type);
  if (SUCCEEDED(status)) status = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(status)) status = type->SetGUID(MF_MT_SUBTYPE, subtype_for(media_type.pixel_format));
  if (SUCCEEDED(status)) status = MFSetAttributeSize(
      type, MF_MT_FRAME_SIZE, media_type.width, media_type.height);
  if (SUCCEEDED(status)) status = MFSetAttributeRatio(
      type, MF_MT_FRAME_RATE, media_type.frame_rate_numerator,
      media_type.frame_rate_denominator);
  if (SUCCEEDED(status)) status = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(status)) status = type->SetUINT32(
      MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(status)) status = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (SUCCEEDED(status)) status = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  if (SUCCEEDED(status)) status = type->SetUINT32(
      MF_MT_DEFAULT_STRIDE, media_type.stride_bytes);
  if (SUCCEEDED(status)) status = type->SetUINT32(
      MF_MT_SAMPLE_SIZE, static_cast<UINT32>(media_type.sample_size_bytes));
  if (FAILED(status)) {
    if (type) type->Release();
    error = "Unable to create the negotiated Media Foundation video type";
    return {};
  }
  return make_handle(type);
}

NativeMediaFoundationHandle create_media_foundation_gpu_sample(
    const ConvertedGpuFrame& frame, std::int64_t timestamp_100ns,
    std::int64_t duration_100ns, bool discontinuity, std::string& error) {
  if (frame.pixel_format != VirtualCameraPixelFormat::Nv12 ||
      frame.frame.native_texture == 0 || !frame.frame.texture_owner ||
      timestamp_100ns < 0 || duration_100ns <= 0) {
    error = "Cannot create IMFSample from an invalid NV12 GPU frame or timestamp";
    return {};
  }
  auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame.frame.native_texture);
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  if (description.Format != DXGI_FORMAT_NV12 || description.Width != frame.frame.width ||
      description.Height != frame.frame.height) {
    error = "GPU sample texture must be matching DXGI_FORMAT_NV12";
    return {};
  }

  IMFMediaBuffer* buffer = nullptr;
  HRESULT status = MFCreateDXGISurfaceBuffer(
      __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
  IMFSample* sample = nullptr;
  if (SUCCEEDED(status)) status = MFCreateSample(&sample);
  if (SUCCEEDED(status)) status = sample->AddBuffer(buffer);
  if (SUCCEEDED(status)) status = sample->SetSampleTime(timestamp_100ns);
  if (SUCCEEDED(status)) status = sample->SetSampleDuration(duration_100ns);
  if (SUCCEEDED(status) && discontinuity) {
    status = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
  }
  if (buffer) buffer->Release();
  if (FAILED(status)) {
    if (sample) sample->Release();
    error = "Unable to wrap the D3D11 NV12 texture in an IMFMediaBuffer/IMFSample";
    return {};
  }
  return make_handle(sample);
}

NativeMediaFoundationHandle create_media_foundation_event_queue(std::string& error) {
  IMFMediaEventQueue* queue = nullptr;
  if (FAILED(MFCreateEventQueue(&queue))) {
    error = "Unable to create IMFMediaEventQueue";
    return {};
  }
  return make_handle(queue);
}

bool queue_media_foundation_event(
    const NativeMediaFoundationHandle& queue_handle, MediaFoundationEventKind kind,
    const NativeMediaFoundationHandle& payload, std::int32_t status_code,
    std::string& error) {
  if (!queue_handle.valid()) {
    error = "Cannot queue an event without IMFMediaEventQueue";
    return false;
  }
  auto* queue = reinterpret_cast<IMFMediaEventQueue*>(queue_handle.native_pointer);
  const auto event_type = native_event_type(kind);
  const auto status = static_cast<HRESULT>(status_code);
  HRESULT result = S_OK;
  if (kind == MediaFoundationEventKind::MediaSample) {
    if (!payload.valid()) {
      error = "MEMediaSample requires an IMFSample payload";
      return false;
    }
    result = queue->QueueEventParamUnk(
        event_type, GUID_NULL, status,
        reinterpret_cast<IUnknown*>(payload.native_pointer));
  } else {
    result = queue->QueueEventParamVar(event_type, GUID_NULL, status, nullptr);
  }
  if (FAILED(result)) {
    error = "Unable to queue the Media Foundation event";
    return false;
  }
  return true;
}

NativeMediaFoundationHandle take_media_foundation_event(
    const NativeMediaFoundationHandle& queue_handle, std::string& error) {
  if (!queue_handle.valid()) {
    error = "Cannot read an event without IMFMediaEventQueue";
    return {};
  }
  IMFMediaEvent* event = nullptr;
  const auto status = reinterpret_cast<IMFMediaEventQueue*>(queue_handle.native_pointer)->GetEvent(
      MF_EVENT_FLAG_NO_WAIT, &event);
  if (status == MF_E_NO_EVENTS_AVAILABLE) {
    error.clear();
    return {};
  }
  if (FAILED(status)) {
    error = "Unable to read the Media Foundation event queue";
    return {};
  }
  return make_handle(event);
}

bool shutdown_media_foundation_event_queue(
    const NativeMediaFoundationHandle& queue_handle, std::string& error) {
  if (!queue_handle.valid()) {
    error = "Cannot shut down an invalid IMFMediaEventQueue";
    return false;
  }
  if (FAILED(reinterpret_cast<IMFMediaEventQueue*>(queue_handle.native_pointer)->Shutdown())) {
    error = "Unable to shut down IMFMediaEventQueue";
    return false;
  }
  return true;
}

NativeMediaFoundationHandle create_media_foundation_stream_descriptor(
    std::uint32_t stream_id,
    const std::vector<NativeMediaFoundationHandle>& media_type_handles,
    std::string& error) {
  if (media_type_handles.empty()) {
    error = "IMFStreamDescriptor requires at least one media type";
    return {};
  }
  std::vector<IMFMediaType*> media_types;
  media_types.reserve(media_type_handles.size());
  for (const auto& handle : media_type_handles) {
    if (!handle.valid()) {
      error = "IMFStreamDescriptor received an invalid IMFMediaType handle";
      return {};
    }
    media_types.push_back(reinterpret_cast<IMFMediaType*>(handle.native_pointer));
  }

  IMFStreamDescriptor* descriptor = nullptr;
  HRESULT status = MFCreateStreamDescriptor(
      stream_id, static_cast<DWORD>(media_types.size()), media_types.data(), &descriptor);
  IMFMediaTypeHandler* handler = nullptr;
  if (SUCCEEDED(status)) status = descriptor->GetMediaTypeHandler(&handler);
  if (SUCCEEDED(status)) status = handler->SetCurrentMediaType(media_types.front());
  if (handler) handler->Release();
  if (FAILED(status)) {
    if (descriptor) descriptor->Release();
    error = "Unable to create IMFStreamDescriptor or select its default media type";
    return {};
  }
  return make_handle(descriptor);
}

NativeMediaFoundationHandle create_media_foundation_presentation_descriptor(
    const NativeMediaFoundationHandle& stream_handle, std::string& error) {
  if (!stream_handle.valid()) {
    error = "IMFPresentationDescriptor requires a valid IMFStreamDescriptor";
    return {};
  }
  auto* stream = reinterpret_cast<IMFStreamDescriptor*>(stream_handle.native_pointer);
  IMFPresentationDescriptor* presentation = nullptr;
  HRESULT status = MFCreatePresentationDescriptor(1, &stream, &presentation);
  if (SUCCEEDED(status)) status = presentation->SelectStream(0);
  if (FAILED(status)) {
    if (presentation) presentation->Release();
    error = "Unable to create or select the virtual camera presentation stream";
    return {};
  }
  return make_handle(presentation);
}

} // namespace vividcam
