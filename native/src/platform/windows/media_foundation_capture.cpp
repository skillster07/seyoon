#include "vividcam/camera_capture.hpp"
#include "vividcam/latest_frame_buffer.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

GUID subtype_guid(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::Nv12: return MFVideoFormat_NV12;
    case PixelFormat::Yuy2: return MFVideoFormat_YUY2;
    case PixelFormat::Bgra: return MFVideoFormat_ARGB32;
    case PixelFormat::Mjpeg: return MFVideoFormat_MJPG;
    case PixelFormat::H264: return MFVideoFormat_H264;
    case PixelFormat::Unknown: return GUID_NULL;
  }
  return GUID_NULL;
}

bool subtype_matches(PixelFormat format, const GUID& subtype) noexcept {
  if (format == PixelFormat::Bgra) {
    return subtype == MFVideoFormat_ARGB32 || subtype == MFVideoFormat_RGB32;
  }
  return subtype == subtype_guid(format);
}

constexpr auto kCaptureFlushTimeout = std::chrono::seconds{2};

std::atomic<std::uint64_t> next_capture_thread_token{1};
std::atomic<bool> capture_restart_blocked{false};

std::uint64_t current_capture_thread_token() noexcept {
  thread_local const std::uint64_t token =
      next_capture_thread_token.fetch_add(1, std::memory_order_relaxed);
  return token;
}

struct CaptureState {
  LatestFrameBuffer<CapturedFrame> frames;
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> gpu_frames{0};
  std::atomic<std::uint64_t> cpu_frames{0};
  std::atomic<bool> running{false};
  CameraFormat format;
  std::mutex reader_mutex;
  ComPtr<IMFSourceReader> reader;
  std::mutex callback_mutex;
  std::condition_variable callback_changed;
  std::uint32_t active_callbacks{0};
  bool flush_completed{false};
};

struct GpuSampleOwner {
  ComPtr<IMFSample> sample;
  ComPtr<ID3D11Texture2D> texture;
};

class CallbackActivity {
 public:
  explicit CallbackActivity(std::shared_ptr<CaptureState> state) noexcept
      : state_(std::move(state)) {
    std::scoped_lock lock(state_->callback_mutex);
    ++state_->active_callbacks;
  }

  ~CallbackActivity() {
    {
      std::scoped_lock lock(state_->callback_mutex);
      if (state_->active_callbacks != 0) --state_->active_callbacks;
    }
    state_->callback_changed.notify_all();
  }

  CallbackActivity(const CallbackActivity&) = delete;
  CallbackActivity& operator=(const CallbackActivity&) = delete;

 private:
  std::shared_ptr<CaptureState> state_;
};

class SourceReaderCallback final : public IMFSourceReaderCallback {
 public:
  explicit SourceReaderCallback(std::shared_ptr<CaptureState> state) : state_(std::move(state)) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == __uuidof(IMFSourceReaderCallback)) {
      *object = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG references = --references_;
    if (references <= 1) {
      {
        // Synchronize with wait_for_callback_detach so the final reader
        // Release notification cannot be lost between its predicate and wait.
        std::scoped_lock lock(state_->callback_mutex);
      }
      state_->callback_changed.notify_all();
    }
    if (references == 0) delete this;
    return references;
  }

  [[nodiscard]] bool detached_from_reader() const noexcept {
    return references_.load() == 1;
  }

  STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp,
                            IMFSample* sample) override {
    CallbackActivity activity(state_);
    if (!state_->running.load()) return S_OK;
    if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR)) {
      ++state_->errors;
      state_->running.store(false);
      return status;
    }

    if (sample) {
      ComPtr<IMFMediaBuffer> buffer;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
        LONGLONG duration = 0;
        sample->GetSampleDuration(&duration);
        CapturedFrame frame;
        frame.sequence = ++state_->sequence;
        frame.timestamp_100ns = timestamp;
        frame.duration_100ns = duration;
        frame.format = state_->format;

        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        if (SUCCEEDED(buffer.As(&dxgi_buffer))) {
          auto owner = std::make_shared<GpuSampleOwner>();
          owner->sample = sample;
          UINT subresource = 0;
          if (SUCCEEDED(dxgi_buffer->GetResource(IID_PPV_ARGS(&owner->texture))) &&
              SUCCEEDED(dxgi_buffer->GetSubresourceIndex(&subresource))) {
            frame.gpu = GpuFrameHandle{owner,
                                       reinterpret_cast<std::uintptr_t>(owner->texture.Get()),
                                       subresource};
            ++state_->gpu_frames;
          }
        }

        if (!frame.gpu) {
          BYTE* bytes = nullptr;
          DWORD length = 0;
          if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
            frame.bytes.assign(bytes, bytes + length);
            buffer->Unlock();
            ++state_->cpu_frames;
          }
        }
        if (frame.gpu || !frame.bytes.empty()) state_->frames.push(std::move(frame));
      }
    }

    request_next_sample();
    return S_OK;
  }

  STDMETHODIMP OnFlush(DWORD) override {
    CallbackActivity activity(state_);
    {
      std::scoped_lock lock(state_->callback_mutex);
      state_->flush_completed = true;
    }
    state_->callback_changed.notify_all();
    return S_OK;
  }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override {
    CallbackActivity activity(state_);
    return S_OK;
  }

 private:
  void request_next_sample() {
    std::scoped_lock lock(state_->reader_mutex);
    if (state_->running.load() && state_->reader) {
      const HRESULT result = state_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                        0, nullptr, nullptr, nullptr, nullptr);
      if (FAILED(result)) {
        ++state_->errors;
        state_->running.store(false);
      }
    }
  }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<CaptureState> state_;
};

bool wait_for_callbacks(const std::shared_ptr<CaptureState>& state) noexcept {
  try {
    std::unique_lock lock(state->callback_mutex);
    return state->callback_changed.wait_for(lock, kCaptureFlushTimeout, [&] {
      return state->flush_completed && state->active_callbacks == 0;
    });
  } catch (...) {
    return false;
  }
}

bool wait_for_callback_detach(
    const std::shared_ptr<CaptureState>& state,
    const ComPtr<SourceReaderCallback>& callback) noexcept {
  try {
    std::unique_lock lock(state->callback_mutex);
    return state->callback_changed.wait_for(lock, kCaptureFlushTimeout, [&] {
      return state->active_callbacks == 0 &&
             (!callback || callback->detached_from_reader());
    });
  } catch (...) {
    return false;
  }
}

struct RetainedCaptureLifetime {
  std::shared_ptr<CaptureState> state;
  ComPtr<IMFSourceReader> reader;
  ComPtr<SourceReaderCallback> callback;
};

class MediaFoundationCaptureSession final : public CameraCaptureSession {
 public:
  MediaFoundationCaptureSession() : state_(std::make_shared<CaptureState>()) {}
  ~MediaFoundationCaptureSession() override { stop(); }

  bool start(const std::wstring& symbolic_link, const CameraFormat& format,
             const CaptureOptions& options, std::string& error) override {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    cleanup_locked();
    if (capture_restart_blocked.load(std::memory_order_acquire)) {
      error = "Camera capture restart is blocked after an unsafe Media "
              "Foundation shutdown; restart the VIVIDCAM engine process";
      return false;
    }
    state_ = std::make_shared<CaptureState>();
    lifetime_retained_ = false;
    if (!format.valid()) {
      error = "Invalid camera format";
      return false;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owns_com_ = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
      error = "CoInitializeEx failed";
      return false;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
      if (owns_com_) CoUninitialize();
      owns_com_ = false;
      error = "MFStartup failed";
      return false;
    }
    media_foundation_started_ = true;
    owner_thread_token_ = current_capture_thread_token();
    deferred_lifetime_ = new (std::nothrow) RetainedCaptureLifetime();
    if (!deferred_lifetime_) {
      error = "Unable to allocate capture shutdown safety state";
      cleanup_locked();
      return false;
    }

    ComPtr<IMFAttributes> source_attributes;
    ComPtr<IMFMediaSource> source;
    HRESULT status = MFCreateAttributes(&source_attributes, 2);
    if (SUCCEEDED(status)) status = source_attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(status)) status = source_attributes->SetString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolic_link.c_str());
    if (SUCCEEDED(status)) status = MFCreateDeviceSource(source_attributes.Get(), &source);

    ComPtr<IMFAttributes> reader_attributes;
    callback_.Attach(new SourceReaderCallback(state_));
    if (SUCCEEDED(status)) status = MFCreateAttributes(&reader_attributes, 4);
    if (SUCCEEDED(status)) status = reader_attributes->SetUnknown(
        MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    if (SUCCEEDED(status)) status = reader_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (SUCCEEDED(status) && options.prefer_gpu_surfaces && options.gpu_context &&
        options.gpu_context->valid()) {
      status = reader_attributes->SetUnknown(
          MF_SOURCE_READER_D3D_MANAGER,
          reinterpret_cast<IUnknown*>(options.gpu_context->native_device_manager()));
    }
    if (SUCCEEDED(status)) status = MFCreateSourceReaderFromMediaSource(
        source.Get(), reader_attributes.Get(), &state_->reader);

    ComPtr<IMFMediaType> selected_type;
    if (SUCCEEDED(status)) {
      for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        const HRESULT type_status = state_->reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &candidate);
        if (type_status == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_status)) continue;
        UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
        GUID subtype = GUID_NULL;
        if (SUCCEEDED(MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height)) &&
            SUCCEEDED(MFGetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
            SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            width == format.width && height == format.height &&
            numerator == format.frames_per_second_numerator &&
            denominator == format.frames_per_second_denominator &&
            subtype_matches(format.pixel_format, subtype)) {
          selected_type = candidate;
          break;
        }
      }
      if (!selected_type) status = MF_E_INVALIDMEDIATYPE;
    }
    if (SUCCEEDED(status)) status = state_->reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, selected_type.Get());

    if (FAILED(status)) {
      error = "Unable to open camera with selected media type";
      selected_type.Reset();
      reader_attributes.Reset();
      source.Reset();
      source_attributes.Reset();
      cleanup_locked();
      return false;
    }

    state_->format = format;
    state_->running.store(true);
    status = state_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                        0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(status)) {
      error = "Initial ReadSample failed";
      selected_type.Reset();
      reader_attributes.Reset();
      source.Reset();
      source_attributes.Reset();
      cleanup_locked();
      return false;
    }
    return true;
  }

  void stop() noexcept override {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    cleanup_locked();
  }
  [[nodiscard]] bool running() const noexcept override {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    return state_->running.load();
  }
  [[nodiscard]] std::optional<CapturedFrame> take_latest_frame() override {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    return state_->frames.take();
  }
  [[nodiscard]] CaptureStatistics statistics() const noexcept override {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    return {state_->frames.published_frames(), state_->frames.consumed_frames(),
            state_->frames.overwritten_frames(), state_->errors.load(),
            state_->gpu_frames.load(), state_->cpu_frames.load()};
  }

 private:
  void cleanup_locked() noexcept {
    if (lifetime_retained_) return;
    state_->running.store(false);
    ComPtr<IMFSourceReader> reader;
    {
      std::scoped_lock lock(state_->reader_mutex);
      reader = std::move(state_->reader);
    }

    // CoUninitialize must run on the thread that successfully initialized the
    // apartment. EngineFrameWorker owns start/stop on one worker thread; keep a
    // defensive check here so an accidental cross-thread destruction cannot
    // release COM/MF objects from the wrong apartment.
    if (owner_thread_token_ != 0 &&
        owner_thread_token_ != current_capture_thread_token()) {
      retain_lifetime(std::move(reader));
      return;
    }

    bool callbacks_completed = !reader;
    if (reader) {
      {
        std::scoped_lock lock(state_->callback_mutex);
        state_->flush_completed = false;
      }
      const HRESULT flush_status =
          reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
      callbacks_completed = SUCCEEDED(flush_status) && wait_for_callbacks(state_);
    }

    if (callbacks_completed) {
      reader.Reset();
      callbacks_completed = wait_for_callback_detach(state_, callback_);
    }

    if (!callbacks_completed) {
      // The node was reserved before the async reader was created, so this
      // timeout path performs no allocation. There is no safe bounded way to
      // reclaim the COM apartment reference after its owning worker exits, so
      // conservatively retain the whole callback graph for process lifetime.
      // A faulty driver can leak one capture lifetime, but cannot trigger a
      // use-after-free or MFShutdown while a late callback is still possible.
      retain_lifetime(std::move(reader));
      return;
    }

    // OnFlush, an empty active-callback count, and release of the reader's
    // callback reference form the final publication barrier. Only now can the
    // last IMF sample/D3D11 texture be drained ahead of MFShutdown.
    (void)state_->frames.take();
    callback_.Reset();
    if (media_foundation_started_) (void)MFShutdown();
    media_foundation_started_ = false;
    if (owns_com_) CoUninitialize();
    owns_com_ = false;
    owner_thread_token_ = 0;
    delete deferred_lifetime_;
    deferred_lifetime_ = nullptr;
  }

  void retain_lifetime(ComPtr<IMFSourceReader> reader) noexcept {
    RetainedCaptureLifetime* lifetime = deferred_lifetime_;
    deferred_lifetime_ = nullptr;
    // deferred_lifetime_ is reserved immediately after MFStartup, before the
    // asynchronous reader can exist. Retaining this raw allocation deliberately
    // suppresses its destructor and therefore keeps every COM reference alive.
    lifetime->state = state_;
    lifetime->reader = std::move(reader);
    lifetime->callback = std::move(callback_);
    (void)lifetime;
    capture_restart_blocked.store(true, std::memory_order_release);
    media_foundation_started_ = false;
    owns_com_ = false;
    owner_thread_token_ = 0;
    lifetime_retained_ = true;
  }

  mutable std::mutex lifecycle_mutex_;
  std::shared_ptr<CaptureState> state_;
  ComPtr<SourceReaderCallback> callback_;
  RetainedCaptureLifetime* deferred_lifetime_{nullptr};
  bool media_foundation_started_{false};
  bool owns_com_{false};
  std::uint64_t owner_thread_token_{0};
  bool lifetime_retained_{false};
};
} // namespace

std::unique_ptr<CameraCaptureSession> create_camera_capture_session() {
  return std::make_unique<MediaFoundationCaptureSession>();
}
} // namespace vividcam
