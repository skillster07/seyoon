#include "vividcam/media_foundation_source.hpp"

#include "vividcam/virtual_camera_media_type.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <devicetopology.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
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

class VirtualCameraMediaStream final : public IMFMediaStream2 {
 public:
  VirtualCameraMediaStream(VirtualCameraMediaSource* source,
                           IMFStreamDescriptor* descriptor)
      : source_(source), descriptor_(descriptor) {
    MFCreateEventQueue(&events_);
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
    return events_ ? events_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    return events_ ? events_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                          HRESULT status, const PROPVARIANT* value) override {
    return events_ ? events_->QueueEventParamVar(type, extended_type, status, value)
                   : MF_E_SHUTDOWN;
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
    state_ = state;
    running_ = state == MF_STREAM_STATE_RUNNING;
    if (!running_) requests_.clear();
    return S_OK;
  }
  STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
    if (!state) return E_POINTER;
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
  }

  HRESULT Start() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    running_ = true;
    state_ = MF_STREAM_STATE_RUNNING;
    return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
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
  MF_STREAM_STATE state_{MF_STREAM_STATE_STOPPED};
  bool running_{false};
  bool shutdown_{false};
};

class VirtualCameraMediaSource final : public IMFMediaSourceEx,
                                       public IMFGetService,
                                       public IKsControl,
                                       public IMFSampleAllocatorControl {
 public:
  HRESULT Initialize(const OutputProfile& profile) {
    if (!profile.valid()) return E_INVALIDARG;
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
    auto* stream = new (std::nothrow) VirtualCameraMediaStream(this, descriptor_.Get());
    if (!stream) return E_OUTOFMEMORY;
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
    return events_ ? events_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    return events_ ? events_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                          HRESULT status, const PROPVARIANT* value) override {
    return events_ ? events_->QueueEventParamVar(type, extended_type, status, value)
                   : MF_E_SHUTDOWN;
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
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    HRESULT status = stream_->Start();
    if (SUCCEEDED(status) && !announced_) {
      status = events_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, stream_.Get());
      announced_ = SUCCEEDED(status);
    }
    if (SUCCEEDED(status)) {
      status = events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, nullptr);
    }
    return status;
  }
  STDMETHODIMP Stop() override {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    HRESULT status = stream_->Stop();
    if (SUCCEEDED(status)) {
      status = events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    return status;
  }
  STDMETHODIMP Pause() override {
    std::scoped_lock lock(mutex_);
    return shutdown_ ? MF_E_SHUTDOWN : MF_E_INVALID_STATE_TRANSITION;
  }
  STDMETHODIMP Shutdown() override {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    shutdown_ = true;
    if (stream_) stream_->Shutdown();
    if (events_) events_->Shutdown();
    stream_.Reset();
    descriptor_.Reset();
    presentation_.Reset();
    source_attributes_.Reset();
    events_.Reset();
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
    *usage = MFSampleAllocatorUsage_DoesNotAllocate;
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
  std::mutex mutex_;
  ComPtr<IMFMediaEventQueue> events_;
  ComPtr<IMFStreamDescriptor> descriptor_;
  ComPtr<IMFPresentationDescriptor> presentation_;
  ComPtr<VirtualCameraMediaStream> stream_;
  ComPtr<IMFAttributes> source_attributes_;
  bool announced_{false};
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
    const OutputProfile& profile, std::string& error) {
  auto* source = new (std::nothrow) VirtualCameraMediaSource();
  if (!source) {
    error = "Unable to allocate the virtual camera IMFMediaSource";
    return {};
  }
  const auto status = source->Initialize(profile);
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
