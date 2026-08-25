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
#include <memory>
#include <mutex>

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
};

struct GpuSampleOwner {
  ComPtr<IMFSample> sample;
  ComPtr<ID3D11Texture2D> texture;
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
    if (references == 0) delete this;
    return references;
  }

  STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp,
                            IMFSample* sample) override {
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

  STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

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

class MediaFoundationCaptureSession final : public CameraCaptureSession {
 public:
  MediaFoundationCaptureSession() : state_(std::make_shared<CaptureState>()) {}
  ~MediaFoundationCaptureSession() override { stop(); }

  bool start(const std::wstring& symbolic_link, const CameraFormat& format,
             const CaptureOptions& options, std::string& error) override {
    stop();
    state_ = std::make_shared<CaptureState>();
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
            subtype == subtype_guid(format.pixel_format)) {
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
      cleanup();
      return false;
    }

    state_->format = format;
    state_->running.store(true);
    status = state_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                        0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(status)) {
      error = "Initial ReadSample failed";
      cleanup();
      return false;
    }
    return true;
  }

  void stop() noexcept override { cleanup(); }
  [[nodiscard]] bool running() const noexcept override { return state_->running.load(); }
  [[nodiscard]] std::optional<CapturedFrame> take_latest_frame() override {
    return state_->frames.take();
  }
  [[nodiscard]] CaptureStatistics statistics() const noexcept override {
    return {state_->frames.published_frames(), state_->frames.consumed_frames(),
            state_->frames.overwritten_frames(), state_->errors.load(),
            state_->gpu_frames.load(), state_->cpu_frames.load()};
  }

 private:
  void cleanup() noexcept {
    state_->running.store(false);
    ComPtr<IMFSourceReader> reader;
    {
      std::scoped_lock lock(state_->reader_mutex);
      reader = std::move(state_->reader);
    }
    if (reader) reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader.Reset();
    callback_.Reset();
    if (media_foundation_started_) MFShutdown();
    media_foundation_started_ = false;
    if (owns_com_) CoUninitialize();
    owns_com_ = false;
  }

  std::shared_ptr<CaptureState> state_;
  ComPtr<SourceReaderCallback> callback_;
  bool media_foundation_started_{false};
  bool owns_com_{false};
};
} // namespace

std::unique_ptr<CameraCaptureSession> create_camera_capture_session() {
  return std::make_unique<MediaFoundationCaptureSession>();
}
} // namespace vividcam
