#include "vividcam/media_foundation_source.hpp"

#include "vividcam/control_channel_transport.hpp"
#include "vividcam/virtual_camera_media_type.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <devicetopology.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

template <typename Interface>
NativeMediaFoundationHandle own_native(Interface* pointer) {
  if (!pointer) return {};
  return {std::shared_ptr<void>(pointer, [](void* value) {
            static_cast<Interface*>(value)->Release();
          }),
          reinterpret_cast<std::uintptr_t>(pointer)};
}

class VirtualCameraMediaSource;

struct SyntheticMediaFormat {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t frame_rate_numerator{0};
  std::uint32_t frame_rate_denominator{0};
  VirtualCameraPixelFormat pixel_format{VirtualCameraPixelFormat::Nv12};
};

HRESULT current_synthetic_media_format(IMFStreamDescriptor* descriptor,
                                       SyntheticMediaFormat& format) {
  if (!descriptor) return E_POINTER;
  ComPtr<IMFMediaTypeHandler> handler;
  HRESULT status = descriptor->GetMediaTypeHandler(&handler);
  ComPtr<IMFMediaType> media_type;
  if (SUCCEEDED(status)) status = handler->GetCurrentMediaType(&media_type);

  GUID major_type{};
  GUID subtype{};
  if (SUCCEEDED(status)) status = media_type->GetGUID(MF_MT_MAJOR_TYPE, &major_type);
  if (SUCCEEDED(status)) status = media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
  if (SUCCEEDED(status) && major_type != MFMediaType_Video) {
    status = MF_E_INVALIDMEDIATYPE;
  }
  if (SUCCEEDED(status)) {
    status = MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE,
                                &format.width, &format.height);
  }
  if (SUCCEEDED(status)) {
    status = MFGetAttributeRatio(media_type.Get(), MF_MT_FRAME_RATE,
                                 &format.frame_rate_numerator,
                                 &format.frame_rate_denominator);
  }
  if (FAILED(status)) return status;

  if (subtype == MFVideoFormat_NV12) {
    format.pixel_format = VirtualCameraPixelFormat::Nv12;
    if ((format.width & 1U) != 0 || (format.height & 1U) != 0) {
      return MF_E_INVALIDMEDIATYPE;
    }
  } else if (subtype == MFVideoFormat_ARGB32) {
    format.pixel_format = VirtualCameraPixelFormat::Bgra;
  } else {
    return MF_E_INVALIDMEDIATYPE;
  }
  if (format.width == 0 || format.height == 0 ||
      format.frame_rate_numerator == 0 ||
      format.frame_rate_denominator == 0) {
    return MF_E_INVALIDMEDIATYPE;
  }

  const auto pixels = static_cast<std::uint64_t>(format.width) * format.height;
  const auto sample_size = format.pixel_format == VirtualCameraPixelFormat::Nv12
                               ? pixels * 3ULL / 2ULL
                               : pixels * 4ULL;
  if (sample_size == 0 ||
      sample_size > static_cast<std::uint64_t>(
                        std::numeric_limits<DWORD>::max())) {
    return MF_E_INVALIDMEDIATYPE;
  }
  return S_OK;
}

std::wstring copy_string_attribute(IMFAttributes* attributes,
                                   REFGUID key) noexcept {
  if (!attributes) return {};

  PWSTR value = nullptr;
  UINT32 length = 0;
  const HRESULT status = attributes->GetAllocatedString(key, &value, &length);
  std::wstring result;
  if (SUCCEEDED(status) && value) {
    try {
      result.assign(value, length);
    } catch (...) {
      result.clear();
    }
  }
  CoTaskMemFree(value);
  return result;
}

bool buffer_range_contains(const BYTE* buffer_start, DWORD buffer_length,
                           const BYTE* row, std::size_t row_bytes) {
  const auto start = reinterpret_cast<std::uintptr_t>(buffer_start);
  const auto address = reinterpret_cast<std::uintptr_t>(row);
  const auto end = start + buffer_length;
  return end >= start && address >= start && address <= end &&
         row_bytes <= end - address;
}

BYTE* scanline_at(BYTE* scanline_zero, LONG pitch, std::uint32_t row) {
  const auto address = reinterpret_cast<std::intptr_t>(scanline_zero) +
                       static_cast<std::intptr_t>(pitch) * row;
  return reinterpret_cast<BYTE*>(address);
}

HRESULT fill_nv12_pattern(BYTE* scanline_zero, LONG pitch, BYTE* buffer_start,
                          DWORD buffer_length,
                          const SyntheticMediaFormat& format,
                          std::uint64_t frame_index) {
  constexpr std::array<BYTE, 8> kLuma{235, 210, 170, 145, 106, 81, 41, 16};
  constexpr std::array<BYTE, 8> kChromaU{128, 16, 166, 54, 202, 90, 240, 128};
  constexpr std::array<BYTE, 8> kChromaV{128, 146, 16, 34, 222, 240, 110, 128};
  if (!scanline_zero || !buffer_start || pitch <= 0 ||
      static_cast<std::uint32_t>(pitch) < format.width) {
    return MF_E_INVALIDMEDIATYPE;
  }
  const auto shift = static_cast<std::uint32_t>(
      (frame_index * 8ULL) % format.width);
  for (std::uint32_t y = 0; y < format.height; ++y) {
    auto* row = scanline_at(scanline_zero, pitch, y);
    if (!buffer_range_contains(buffer_start, buffer_length, row, format.width)) {
      return MF_E_BUFFERTOOSMALL;
    }
    for (std::uint32_t x = 0; x < format.width; ++x) {
      const auto shifted = (x + shift) % format.width;
      const auto bar = static_cast<std::size_t>(
          static_cast<std::uint64_t>(shifted) * kLuma.size() / format.width);
      row[x] = kLuma[bar];
    }
  }

  auto* chroma = scanline_at(scanline_zero, pitch, format.height);
  for (std::uint32_t y = 0; y < format.height / 2U; ++y) {
    auto* row = scanline_at(chroma, pitch, y);
    if (!buffer_range_contains(buffer_start, buffer_length, row, format.width)) {
      return MF_E_BUFFERTOOSMALL;
    }
    for (std::uint32_t x = 0; x < format.width; x += 2U) {
      const auto shifted = (x + shift) % format.width;
      const auto bar = static_cast<std::size_t>(
          static_cast<std::uint64_t>(shifted) * kChromaU.size() / format.width);
      row[x] = kChromaU[bar];
      row[x + 1U] = kChromaV[bar];
    }
  }
  return S_OK;
}

HRESULT fill_bgra_pattern(BYTE* scanline_zero, LONG pitch, BYTE* buffer_start,
                          DWORD buffer_length,
                          const SyntheticMediaFormat& format,
                          std::uint64_t frame_index) {
  struct Bgra {
    BYTE blue;
    BYTE green;
    BYTE red;
    BYTE alpha;
  };
  constexpr std::array<Bgra, 8> kBars{{
      {255, 255, 255, 255}, {0, 255, 255, 255}, {255, 255, 0, 255},
      {0, 255, 0, 255},     {255, 0, 255, 255}, {0, 0, 255, 255},
      {255, 0, 0, 255},     {0, 0, 0, 255},
  }};
  const auto row_bytes = static_cast<std::uint64_t>(format.width) * sizeof(Bgra);
  const auto absolute_pitch = pitch < 0
                                  ? -static_cast<std::int64_t>(pitch)
                                  : static_cast<std::int64_t>(pitch);
  if (!scanline_zero || !buffer_start ||
      row_bytes > static_cast<std::uint64_t>(absolute_pitch)) {
    return MF_E_INVALIDMEDIATYPE;
  }
  const auto shift = static_cast<std::uint32_t>(
      (frame_index * 8ULL) % format.width);
  for (std::uint32_t y = 0; y < format.height; ++y) {
    auto* row = scanline_at(scanline_zero, pitch, y);
    if (!buffer_range_contains(buffer_start, buffer_length, row,
                               static_cast<std::size_t>(row_bytes))) {
      return MF_E_BUFFERTOOSMALL;
    }
    for (std::uint32_t x = 0; x < format.width; ++x) {
      const auto shifted = (x + shift) % format.width;
      const auto bar = static_cast<std::size_t>(
          static_cast<std::uint64_t>(shifted) * kBars.size() / format.width);
      const auto& color = kBars[bar];
      auto* pixel = row + static_cast<std::size_t>(x) * sizeof(Bgra);
      pixel[0] = color.blue;
      pixel[1] = color.green;
      pixel[2] = color.red;
      pixel[3] = color.alpha;
    }
  }
  return S_OK;
}

HRESULT create_synthetic_sample(IMFStreamDescriptor* descriptor, IUnknown* token,
                                bool discontinuity, std::uint64_t frame_index,
                                LONGLONG previous_timestamp_100ns,
                                LONGLONG* timestamp_100ns,
                                IMFSample** output) {
  if (!timestamp_100ns || !output) return E_POINTER;
  *timestamp_100ns = 0;
  *output = nullptr;

  SyntheticMediaFormat format;
  HRESULT status = current_synthetic_media_format(descriptor, format);
  LONGLONG duration = 0;
  if (SUCCEEDED(status)) {
    duration = static_cast<LONGLONG>(
        (10'000'000ULL * format.frame_rate_denominator +
         format.frame_rate_numerator / 2ULL) /
        format.frame_rate_numerator);
    if (duration <= 0) status = MF_E_INVALIDMEDIATYPE;
  }
  const auto sample_timestamp =
      discontinuity || previous_timestamp_100ns < 0
          ? MFGetSystemTime()
          : previous_timestamp_100ns + duration;
  ComPtr<IMFMediaBuffer> buffer;
  if (SUCCEEDED(status)) {
    const auto fourcc = format.pixel_format == VirtualCameraPixelFormat::Nv12
                            ? MFVideoFormat_NV12.Data1
                            : MFVideoFormat_ARGB32.Data1;
    status = MFCreate2DMediaBuffer(
        format.width, format.height, fourcc, FALSE, &buffer);
  }
  ComPtr<IMF2DBuffer2> buffer_2d;
  if (SUCCEEDED(status)) status = buffer.As(&buffer_2d);
  BYTE* scanline_zero = nullptr;
  LONG pitch = 0;
  BYTE* buffer_start = nullptr;
  DWORD buffer_length = 0;
  bool locked = false;
  if (SUCCEEDED(status)) {
    status = buffer_2d->Lock2DSize(
        MF2DBuffer_LockFlags_Write, &scanline_zero, &pitch,
        &buffer_start, &buffer_length);
    locked = SUCCEEDED(status);
  }
  if (SUCCEEDED(status)) {
    if (format.pixel_format == VirtualCameraPixelFormat::Nv12) {
      status = fill_nv12_pattern(
          scanline_zero, pitch, buffer_start, buffer_length, format, frame_index);
    } else {
      status = fill_bgra_pattern(
          scanline_zero, pitch, buffer_start, buffer_length, format, frame_index);
    }
  }
  if (locked) {
    const HRESULT unlock_status = buffer_2d->Unlock2D();
    if (SUCCEEDED(status)) status = unlock_status;
  }
  ComPtr<IMFSample> sample;
  if (SUCCEEDED(status)) status = MFCreateSample(&sample);
  if (SUCCEEDED(status)) status = sample->AddBuffer(buffer.Get());
  if (SUCCEEDED(status)) status = sample->SetSampleTime(sample_timestamp);
  if (SUCCEEDED(status)) status = sample->SetSampleDuration(duration);
  if (SUCCEEDED(status)) {
    status = sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
  }
  if (SUCCEEDED(status) && discontinuity) {
    status = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
  }
  if (SUCCEEDED(status) && token) {
    status = sample->SetUnknown(MFSampleExtension_Token, token);
  }
  if (FAILED(status)) return status;
  *timestamp_100ns = sample_timestamp;
  *output = sample.Detach();
  return S_OK;
}

class VirtualCameraMediaStream final : public IMFMediaStream2 {
 public:
  VirtualCameraMediaStream(VirtualCameraMediaSource* source,
                           IMFStreamDescriptor* descriptor,
                           MediaFoundationVirtualCameraSourceMode mode)
      : source_(source), descriptor_(descriptor), mode_(mode),
        initialization_status_(MFCreateEventQueue(&events_)) {}

  [[nodiscard]] HRESULT initialization_status() const noexcept {
    return initialization_status_;
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override;
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto references = --references_;
    if (references == 0) delete this;
    return references;
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->GetEvent(flags, event);
  }
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->BeginGetEvent(callback, state);
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->EndGetEvent(result, event);
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                          HRESULT status, const PROPVARIANT* value) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->QueueEventParamVar(type, extended_type, status, value);
  }
  STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override {
    if (!descriptor) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *descriptor = descriptor_.Get();
    (*descriptor)->AddRef();
    return S_OK;
  }
  STDMETHODIMP RequestSample(IUnknown* token) override {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!running_) return MF_E_INVALIDREQUEST;
    if (mode_ == MediaFoundationVirtualCameraSourceMode::SyntheticPattern) {
      LONGLONG sample_time = 0;
      ComPtr<IMFSample> sample;
      HRESULT status = create_synthetic_sample(
          descriptor_.Get(), token, first_sample_, frame_index_,
          last_sample_time_100ns_, &sample_time, &sample);
      if (SUCCEEDED(status)) {
        status = events_->QueueEventParamUnk(
            MEMediaSample, GUID_NULL, S_OK, sample.Get());
      }
      if (SUCCEEDED(status)) {
        first_sample_ = false;
        last_sample_time_100ns_ = sample_time;
        ++frame_index_;
      }
      return status;
    }
    if (requests_.size() >= 8) return MF_E_NOTACCEPTING;
    requests_.emplace_back(token);
    return S_OK;
  }
  STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state != MF_STREAM_STATE_STOPPED && state != MF_STREAM_STATE_RUNNING &&
        state != MF_STREAM_STATE_PAUSED) {
      return MF_E_INVALID_STATE_TRANSITION;
    }
    const bool was_running = running_;
    state_ = state;
    running_ = state == MF_STREAM_STATE_RUNNING;
    if (!running_) requests_.clear();
    if (running_ && !was_running) first_sample_ = true;
    return S_OK;
  }
  STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
    if (!state) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
  }

  HRESULT Start(const PROPVARIANT* start_time) {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    running_ = true;
    state_ = MF_STREAM_STATE_RUNNING;
    first_sample_ = true;
    return events_->QueueEventParamVar(
        MEStreamStarted, GUID_NULL, S_OK, start_time);
  }
  HRESULT Stop() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    running_ = false;
    state_ = MF_STREAM_STATE_STOPPED;
    requests_.clear();
    return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
  }
  HRESULT Submit(IMFSample* sample) {
    if (!sample) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (mode_ != MediaFoundationVirtualCameraSourceMode::ExternalSubmit) {
      return MF_E_NOTACCEPTING;
    }
    if (!running_ || requests_.empty()) return MF_E_NOTACCEPTING;
    auto token = std::move(requests_.front());
    requests_.pop_front();
    if (token) {
      const auto status = sample->SetUnknown(MFSampleExtension_Token, token.Get());
      if (FAILED(status)) return status;
    }
    return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
  }
  void Shutdown() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    running_ = false;
    state_ = MF_STREAM_STATE_STOPPED;
    requests_.clear();
    if (events_) events_->Shutdown();
    events_.Reset();
    descriptor_.Reset();
    source_ = nullptr;
  }

 private:
  ~VirtualCameraMediaStream() { Shutdown(); }
  std::atomic<ULONG> references_{1};
  mutable std::mutex mutex_;
  VirtualCameraMediaSource* source_{nullptr};
  ComPtr<IMFStreamDescriptor> descriptor_;
  ComPtr<IMFMediaEventQueue> events_;
  std::deque<ComPtr<IUnknown>> requests_;
  MediaFoundationVirtualCameraSourceMode mode_{
      MediaFoundationVirtualCameraSourceMode::ExternalSubmit};
  HRESULT initialization_status_{E_UNEXPECTED};
  std::uint64_t frame_index_{0};
  LONGLONG last_sample_time_100ns_{-1};
  MF_STREAM_STATE state_{MF_STREAM_STATE_STOPPED};
  bool first_sample_{true};
  bool running_{false};
  bool shutdown_{false};
};

class VirtualCameraMediaSource final : public IMFMediaSourceEx,
                                       public IMFGetService,
                                       public IKsControl,
                                       public IMFSampleAllocatorControl {
 public:
  HRESULT Initialize(const OutputProfile& profile,
                     MediaFoundationVirtualCameraSourceMode mode) {
    if (!profile.valid()) return E_INVALIDARG;
    mode_ = mode;
    HRESULT status = MFCreateEventQueue(&events_);
    if (SUCCEEDED(status)) status = MFCreateAttributes(&source_attributes_, 1);
    if (FAILED(status)) return status;
    std::vector<NativeMediaFoundationHandle> types;
    std::string error;
    for (const auto& media_type : supported_virtual_camera_media_types(profile)) {
      auto type = create_media_foundation_media_type(media_type, error);
      if (!type.valid()) return E_FAIL;
      types.push_back(std::move(type));
    }
    auto stream_descriptor = create_media_foundation_stream_descriptor(0, types, error);
    auto presentation = create_media_foundation_presentation_descriptor(stream_descriptor, error);
    if (!stream_descriptor.valid() || !presentation.valid()) return E_FAIL;
    descriptor_ = reinterpret_cast<IMFStreamDescriptor*>(stream_descriptor.native_pointer);
    presentation_ = reinterpret_cast<IMFPresentationDescriptor*>(presentation.native_pointer);
    status = descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(status)) status = descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(status)) {
      status = descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, TRUE);
    }
    if (SUCCEEDED(status)) {
      status = descriptor_->SetUINT32(
          MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    }
    if (FAILED(status)) return status;
    auto* stream = new (std::nothrow)
        VirtualCameraMediaStream(this, descriptor_.Get(), mode_);
    if (!stream) return E_OUTOFMEMORY;
    if (FAILED(stream->initialization_status())) {
      const auto stream_status = stream->initialization_status();
      stream->Release();
      return stream_status;
    }
    stream_.Attach(stream);
    return status;
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator) ||
        iid == __uuidof(IMFMediaSource) || iid == __uuidof(IMFMediaSourceEx)) {
      *object = static_cast<IMFMediaSourceEx*>(this);
    } else if (iid == __uuidof(IMFGetService)) {
      *object = static_cast<IMFGetService*>(this);
    } else if (iid == __uuidof(IKsControl)) {
      *object = static_cast<IKsControl*>(this);
    } else if (iid == __uuidof(IMFSampleAllocatorControl)) {
      *object = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto references = --references_;
    if (references == 0) delete this;
    return references;
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->GetEvent(flags, event);
  }
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->BeginGetEvent(callback, state);
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->EndGetEvent(result, event);
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                          HRESULT status, const PROPVARIANT* value) override {
    ComPtr<IMFMediaEventQueue> events;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_ || !events_) return MF_E_SHUTDOWN;
      events = events_;
    }
    return events->QueueEventParamVar(type, extended_type, status, value);
  }
  STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
    if (!characteristics) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override {
    if (!descriptor) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return presentation_->Clone(descriptor);
  }
  STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                      const PROPVARIANT* position) override {
    if (!descriptor || !position) return E_INVALIDARG;
    if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::scoped_lock lifecycle_lock(control_lifecycle_mutex_);
    std::wstring control_route;
    HRESULT status = S_OK;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_) return MF_E_SHUTDOWN;
      DWORD stream_count = 0;
      status = descriptor->GetStreamDescriptorCount(&stream_count);
      if (FAILED(status)) return status;
      if (stream_count != 1) return E_INVALIDARG;

      BOOL selected = FALSE;
      ComPtr<IMFStreamDescriptor> requested_descriptor;
      status = descriptor->GetStreamDescriptorByIndex(
          0, &selected, &requested_descriptor);
      if (SUCCEEDED(status) && !selected) return E_INVALIDARG;
      DWORD requested_stream_id = 0;
      if (SUCCEEDED(status)) {
        status = requested_descriptor->GetStreamIdentifier(&requested_stream_id);
      }
      if (SUCCEEDED(status) && requested_stream_id != 0) status = MF_E_NOT_FOUND;

      if (SUCCEEDED(status)) {
        ComPtr<IMFMediaTypeHandler> requested_handler;
        ComPtr<IMFMediaTypeHandler> stream_handler;
        ComPtr<IMFMediaType> requested_type;
        status = requested_descriptor->GetMediaTypeHandler(&requested_handler);
        if (SUCCEEDED(status)) {
          status = requested_handler->GetCurrentMediaType(&requested_type);
        }
        if (SUCCEEDED(status)) {
          status = descriptor_->GetMediaTypeHandler(&stream_handler);
        }
        if (SUCCEEDED(status)) {
          status = stream_handler->SetCurrentMediaType(requested_type.Get());
        }
      }

      if (SUCCEEDED(status)) {
        const auto stream_event = announced_ ? MEUpdatedStream : MENewStream;
        status = events_->QueueEventParamUnk(
            stream_event, GUID_NULL, S_OK, stream_.Get());
        if (SUCCEEDED(status)) announced_ = true;
      }
      PROPVARIANT start_time;
      PropVariantInit(&start_time);
      start_time.vt = VT_I8;
      start_time.hVal.QuadPart = MFGetSystemTime();
      if (SUCCEEDED(status)) status = stream_->Start(&start_time);
      if (SUCCEEDED(status)) {
        status = events_->QueueEventParamVar(
            MESourceStarted, GUID_NULL, S_OK, &start_time);
      }
      if (SUCCEEDED(status)) {
        started_ = true;
        if (mode_ == MediaFoundationVirtualCameraSourceMode::SyntheticPattern) {
          control_route = copy_string_attribute(
              source_attributes_.Get(),
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        }
      }
      PropVariantClear(&start_time);
    }

    if (!control_route.empty()) {
      try {
        std::string ignored_error;
        (void)control_client_.start(std::move(control_route), ignored_error);
      } catch (...) {
        control_client_.stop();
      }
    }
    return status;
  }
  STDMETHODIMP Stop() override {
    std::scoped_lock lifecycle_lock(control_lifecycle_mutex_);
    HRESULT status = S_OK;
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_) return MF_E_SHUTDOWN;
      if (!started_) return MF_E_INVALID_STATE_TRANSITION;
      status = stream_->Stop();
      if (SUCCEEDED(status)) {
        started_ = false;
        announced_ = false;
        status = events_->QueueEventParamVar(
            MESourceStopped, GUID_NULL, S_OK, nullptr);
      }
    }
    control_client_.stop();
    return status;
  }
  STDMETHODIMP Pause() override {
    std::scoped_lock lock(mutex_);
    return shutdown_ ? MF_E_SHUTDOWN : MF_E_INVALID_STATE_TRANSITION;
  }
  STDMETHODIMP Shutdown() override {
    std::scoped_lock lifecycle_lock(control_lifecycle_mutex_);
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_) return MF_E_SHUTDOWN;
      shutdown_ = true;
      started_ = false;
      announced_ = false;
      if (stream_) stream_->Shutdown();
      if (events_) events_->Shutdown();
      stream_.Reset();
      descriptor_.Reset();
      presentation_.Reset();
      source_attributes_.Reset();
      events_.Reset();
    }
    control_client_.stop();
    return S_OK;
  }
  STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *attributes = source_attributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
  }
  STDMETHODIMP GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) override {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (stream_id != 0) return MF_E_NOT_FOUND;
    return descriptor_->QueryInterface(IID_PPV_ARGS(attributes));
  }
  STDMETHODIMP SetD3DManager(IUnknown*) override {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::scoped_lock lock(mutex_);
    return shutdown_ ? MF_E_SHUTDOWN : MF_E_UNSUPPORTED_SERVICE;
  }
  STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, PVOID, ULONG,
                          ULONG* bytes_returned) override {
    if (bytes_returned) *bytes_returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }
  STDMETHODIMP KsMethod(PKSMETHOD, ULONG, PVOID, ULONG,
                        ULONG* bytes_returned) override {
    if (bytes_returned) *bytes_returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }
  STDMETHODIMP KsEvent(PKSEVENT, ULONG, PVOID, ULONG,
                       ULONG* bytes_returned) override {
    if (bytes_returned) *bytes_returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }
  STDMETHODIMP SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) override {
    if (!allocator || stream_id != 0) return E_INVALIDARG;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id,
                                 MFSampleAllocatorUsage* usage) override {
    if (!input_stream_id || !usage) return E_POINTER;
    if (stream_id != 0) return MF_E_NOT_FOUND;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *input_stream_id = stream_id;
    *usage = mode_ == MediaFoundationVirtualCameraSourceMode::SyntheticPattern
                 ? MFSampleAllocatorUsage_UsesCustomAllocator
                 : MFSampleAllocatorUsage_DoesNotAllocate;
    return S_OK;
  }

  HRESULT Submit(IMFSample* sample) {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->Submit(sample);
  }
  HRESULT StartDefault() {
    ComPtr<IMFPresentationDescriptor> presentation;
    HRESULT status = CreatePresentationDescriptor(&presentation);
    PROPVARIANT position;
    PropVariantInit(&position);
    if (SUCCEEDED(status)) status = Start(presentation.Get(), &GUID_NULL, &position);
    PropVariantClear(&position);
    return status;
  }
  HRESULT Request() {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->RequestSample(nullptr);
  }
  HRESULT TakeStreamEvent(IMFMediaEvent** event) {
    if (!event) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->GetEvent(MF_EVENT_FLAG_NO_WAIT, event);
  }

 private:
  ~VirtualCameraMediaSource() {
    if (!shutdown_) Shutdown();
  }
  std::atomic<ULONG> references_{1};
  std::mutex control_lifecycle_mutex_;
  std::mutex mutex_;
  ComPtr<IMFMediaEventQueue> events_;
  ComPtr<IMFStreamDescriptor> descriptor_;
  ComPtr<IMFPresentationDescriptor> presentation_;
  ComPtr<VirtualCameraMediaStream> stream_;
  ComPtr<IMFAttributes> source_attributes_;
  MediaFoundationVirtualCameraSourceMode mode_{
      MediaFoundationVirtualCameraSourceMode::ExternalSubmit};
  SourceControlClient control_client_;
  bool announced_{false};
  bool started_{false};
  bool shutdown_{false};
};

STDMETHODIMP VirtualCameraMediaStream::QueryInterface(REFIID iid, void** object) {
  if (!object) return E_POINTER;
  if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator) ||
      iid == __uuidof(IMFMediaStream) || iid == __uuidof(IMFMediaStream2)) {
    *object = static_cast<IMFMediaStream2*>(this);
    AddRef();
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP VirtualCameraMediaStream::GetMediaSource(IMFMediaSource** source) {
  if (!source) return E_POINTER;
  std::scoped_lock lock(mutex_);
  if (shutdown_ || !source_) return MF_E_SHUTDOWN;
  *source = static_cast<IMFMediaSource*>(source_);
  (*source)->AddRef();
  return S_OK;
}
} // namespace

NativeMediaFoundationHandle create_media_foundation_virtual_camera_source(
    const OutputProfile& profile, std::string& error,
    MediaFoundationVirtualCameraSourceMode mode) {
  auto* source = new (std::nothrow) VirtualCameraMediaSource();
  if (!source) {
    error = "Unable to allocate the virtual camera IMFMediaSource";
    return {};
  }
  const auto status = source->Initialize(profile, mode);
  if (FAILED(status)) {
    source->Release();
    error = "Unable to initialize the virtual camera IMFMediaSource/IMFMediaStream";
    return {};
  }
  return {std::shared_ptr<void>(source, [](void* value) {
            static_cast<VirtualCameraMediaSource*>(value)->Release();
          }),
          reinterpret_cast<std::uintptr_t>(
              static_cast<IMFMediaSource*>(static_cast<IMFMediaSourceEx*>(source)))};
}

bool submit_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle& source_handle,
    const NativeMediaFoundationHandle& sample_handle, std::string& error) {
  if (!source_handle.valid() || !sample_handle.valid()) {
    error = "Virtual camera source and sample handles must be valid";
    return false;
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  const auto status = source->Submit(
      reinterpret_cast<IMFSample*>(sample_handle.native_pointer));
  if (FAILED(status)) {
    error = "Virtual camera IMFMediaStream rejected the sample or has no pending request";
    return false;
  }
  return true;
}

bool start_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source_handle, std::string& error) {
  if (!source_handle.valid()) {
    error = "Virtual camera IMFMediaSource handle is invalid";
    return false;
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  if (FAILED(source->StartDefault())) {
    error = "Unable to start the virtual camera IMFMediaSource";
    return false;
  }
  return true;
}

bool request_media_foundation_virtual_camera_sample(
    const NativeMediaFoundationHandle& source_handle, std::string& error) {
  if (!source_handle.valid()) {
    error = "Virtual camera IMFMediaSource handle is invalid";
    return false;
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  if (FAILED(source->Request())) {
    error = "Unable to request an IMFMediaStream sample";
    return false;
  }
  return true;
}

NativeMediaFoundationHandle take_media_foundation_virtual_camera_stream_event(
    const NativeMediaFoundationHandle& source_handle, std::string& error) {
  if (!source_handle.valid()) {
    error = "Virtual camera IMFMediaSource handle is invalid";
    return {};
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  IMFMediaEvent* event = nullptr;
  const auto status = source->TakeStreamEvent(&event);
  if (status == MF_E_NO_EVENTS_AVAILABLE) {
    error.clear();
    return {};
  }
  if (FAILED(status)) {
    error = "Unable to read the virtual camera IMFMediaStream event";
    return {};
  }
  return own_native(event);
}

bool stop_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source_handle, std::string& error) {
  if (!source_handle.valid()) {
    error = "Virtual camera IMFMediaSource handle is invalid";
    return false;
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  if (FAILED(source->Stop())) {
    error = "Unable to stop the virtual camera IMFMediaSource";
    return false;
  }
  return true;
}

bool shutdown_media_foundation_virtual_camera_source(
    const NativeMediaFoundationHandle& source_handle, std::string& error) {
  if (!source_handle.valid()) {
    error = "Virtual camera IMFMediaSource handle is invalid";
    return false;
  }
  auto* source = static_cast<VirtualCameraMediaSource*>(
      reinterpret_cast<IMFMediaSource*>(source_handle.native_pointer));
  const auto status = source->Shutdown();
  if (FAILED(status) && status != MF_E_SHUTDOWN) {
    error = "Unable to shut down the virtual camera IMFMediaSource";
    return false;
  }
  return true;
}

} // namespace vividcam
