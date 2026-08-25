#include "vividcam/registered_virtual_camera_smoke.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

constexpr wchar_t kFriendlyName[] = L"VIVIDCAM Virtual Camera";
constexpr std::uint32_t kExpectedWidth = 1920;
constexpr std::uint32_t kExpectedHeight = 1080;
constexpr std::uint32_t kExpectedFps = 60;
constexpr DWORD kMediaTypeOrStreamChangedFlags =
    MF_SOURCE_READERF_NEWSTREAM |
    MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
    MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
constexpr DWORD kRetryableNullReaderFlags =
    MF_SOURCE_READERF_STREAMTICK |
    MF_SOURCE_READERF_ALLEFFECTSREMOVED;

std::string hresult_error(const char* operation, HRESULT status) {
  std::ostringstream message;
  message << operation << " failed (HRESULT=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}

class ComRuntime {
 public:
  ComRuntime() : status_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
                 owns_(SUCCEEDED(status_)) {}
  ~ComRuntime() {
    if (owns_) CoUninitialize();
  }

  [[nodiscard]] bool available() const noexcept {
    return SUCCEEDED(status_) || status_ == RPC_E_CHANGED_MODE;
  }
  [[nodiscard]] HRESULT status() const noexcept { return status_; }

 private:
  HRESULT status_;
  bool owns_;
};

class MediaFoundationRuntime {
 public:
  MediaFoundationRuntime() : status_(MFStartup(MF_VERSION, MFSTARTUP_FULL)),
                             started_(SUCCEEDED(status_)) {}
  ~MediaFoundationRuntime() {
    if (started_) MFShutdown();
  }

  [[nodiscard]] bool available() const noexcept { return started_; }
  [[nodiscard]] HRESULT status() const noexcept { return status_; }

 private:
  HRESULT status_;
  bool started_;
};

std::wstring activation_string(IMFActivate* activation, const GUID& key) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  if (FAILED(activation->GetAllocatedString(key, &value, &length))) return {};
  std::wstring result(value, length);
  CoTaskMemFree(value);
  return result;
}

std::wstring wait_for_persistent_camera(
    std::chrono::steady_clock::time_point deadline, std::string& error) {
  HRESULT last_status = MF_E_NOT_FOUND;
  do {
    ComPtr<IMFAttributes> attributes;
    HRESULT status = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(status)) {
      status = attributes->SetGUID(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }

    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(status)) {
      status = MFEnumDeviceSources(attributes.Get(), &activations, &count);
    }
    last_status = status;

    std::wstring symbolic_link;
    if (SUCCEEDED(status)) {
      for (UINT32 index = 0; index < count; ++index) {
        const auto candidate_name = activation_string(
            activations[index], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        constexpr int friendly_name_length =
            static_cast<int>(std::size(kFriendlyName) - 1U);
        if (symbolic_link.empty() &&
            candidate_name.size() >=
                static_cast<std::size_t>(friendly_name_length) &&
            CompareStringOrdinal(candidate_name.data(), friendly_name_length,
                                 kFriendlyName, friendly_name_length,
                                 TRUE) == CSTR_EQUAL) {
          symbolic_link = activation_string(
              activations[index],
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        }
        activations[index]->Release();
      }
      CoTaskMemFree(activations);
    }
    if (!symbolic_link.empty()) return symbolic_link;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (std::chrono::steady_clock::now() < deadline);

  error = FAILED(last_status)
              ? hresult_error("MFEnumDeviceSources", last_status)
              : "The persistent VIVIDCAM camera did not remain enumerable after installation";
  return {};
}

std::uint64_t frame_checksum(const std::uint8_t* bytes, std::size_t size) {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t checksum = offset_basis;
  for (std::size_t index = 0; index < size; ++index) {
    checksum ^= bytes[index];
    checksum *= prime;
  }
  return checksum;
}

class ReaderCallback final : public IMFSourceReaderCallback {
 public:
  ReaderCallback(std::uint32_t required_samples, std::uint32_t expected_bytes,
                 std::int64_t expected_duration)
      : required_samples_(required_samples), expected_bytes_(expected_bytes),
        expected_duration_(expected_duration) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != __uuidof(IMFSourceReaderCallback)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<IMFSourceReaderCallback*>(this);
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto references = --references_;
    if (references == 0) delete this;
    return references;
  }

  STDMETHODIMP OnReadSample(HRESULT read_status, DWORD, DWORD flags,
                            LONGLONG reader_timestamp,
                            IMFSample* sample) override {
    ComPtr<IMFSourceReader> reader;
    {
      std::lock_guard lock(mutex_);
      if (done_) return S_OK;
      result_.source_reader_flags |= flags;
      if (FAILED(read_status)) {
        fail_locked(hresult_error("IMFSourceReaderCallback::OnReadSample",
                                  read_status));
        return S_OK;
      }
      if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
        fail_locked("Source reader reported MF_SOURCE_READERF_ERROR");
        return S_OK;
      }
      if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
        fail_locked("Registered virtual camera ended its stream unexpectedly");
        return S_OK;
      }
      if ((flags & kMediaTypeOrStreamChangedFlags) != 0) {
        fail_locked(
            "Registered virtual camera changed its fixed stream or media type");
        return S_OK;
      }
      if (!sample) {
        if ((flags & kRetryableNullReaderFlags) == 0) {
          std::ostringstream message;
          message << "Source reader returned a null video sample without a "
                     "non-terminal state flag (flags=0x"
                  << std::hex << std::uppercase << flags << ')';
          fail_locked(message.str());
          return S_OK;
        }
        ++empty_callbacks_;
        result_.empty_callbacks = empty_callbacks_;
        if (empty_callbacks_ > 16) {
          std::ostringstream message;
          message << "Source reader repeatedly returned a null video sample"
                  << " (flags=0x" << std::hex << std::uppercase << flags
                  << ')';
          fail_locked(message.str());
          return S_OK;
        }
      } else {
        if (!inspect_sample_locked(sample, reader_timestamp)) return S_OK;
        if (result_.samples >= required_samples_) {
          result_.distinct_checksums =
              static_cast<std::uint32_t>(checksums_.size());
          result_.average_timestamp_delta_100ns =
              (result_.last_timestamp_100ns - result_.first_timestamp_100ns) /
              static_cast<std::int64_t>(result_.samples - 1U);
          if (checksums_.size() < 2) {
            fail_locked("All received frames had the same content checksum");
            return S_OK;
          }
          result_.passed = true;
          done_ = true;
          condition_.notify_all();
          return S_OK;
        }
      }
      reader = reader_;
    }

    if (!reader) {
      fail("Source reader detached before the requested samples were received");
      return S_OK;
    }
    const HRESULT status = reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(status)) fail(hresult_error("IMFSourceReader::ReadSample", status));
    return S_OK;
  }

  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
  STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

  void attach_reader(IMFSourceReader* reader) {
    std::lock_guard lock(mutex_);
    reader_ = reader;
  }

  void detach_reader() {
    std::lock_guard lock(mutex_);
    reader_ = nullptr;
  }

  void fail(std::string error) {
    std::lock_guard lock(mutex_);
    fail_locked(std::move(error));
  }

  void wait_for_completion(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, timeout, [this] { return done_; })) {
      fail_locked("Timed out waiting for registered virtual camera samples");
    }
  }

  [[nodiscard]] RegisteredVirtualCameraSmokeResult result() const {
    std::lock_guard lock(mutex_);
    return result_;
  }

 private:
  ~ReaderCallback() = default;

  bool inspect_sample_locked(IMFSample* sample, LONGLONG reader_timestamp) {
    LONGLONG sample_timestamp = 0;
    LONGLONG duration = 0;
    if (FAILED(sample->GetSampleTime(&sample_timestamp))) {
      fail_locked("Received sample has no Media Foundation timestamp");
      return false;
    }
    if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
      fail_locked("Received sample has no positive Media Foundation duration");
      return false;
    }
    if (sample_timestamp != reader_timestamp) {
      fail_locked("Source reader and sample timestamps do not match");
      return false;
    }
    if (result_.samples > 0) {
      if (sample_timestamp <= result_.last_timestamp_100ns) {
        fail_locked("Registered virtual camera timestamps are not strictly monotonic");
        return false;
      }
      const auto delta = sample_timestamp - result_.last_timestamp_100ns;
      const auto delta_error = delta > expected_duration_
                                   ? delta - expected_duration_
                                   : expected_duration_ - delta;
      if (delta_error > expected_duration_ / 4) {
        fail_locked("Registered virtual camera timestamps do not match 60p cadence");
        return false;
      }
      if (result_.samples == 1) {
        result_.minimum_timestamp_delta_100ns = delta;
        result_.maximum_timestamp_delta_100ns = delta;
      } else {
        result_.minimum_timestamp_delta_100ns =
            std::min(result_.minimum_timestamp_delta_100ns, delta);
        result_.maximum_timestamp_delta_100ns =
            std::max(result_.maximum_timestamp_delta_100ns, delta);
      }
    }
    const auto duration_error = duration > expected_duration_
                                    ? duration - expected_duration_
                                    : expected_duration_ - duration;
    if (duration_error > 1000) {
      fail_locked("Registered virtual camera sample duration does not match 60p");
      return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
      fail_locked("Unable to make the registered camera sample contiguous");
      return false;
    }

    std::uint64_t checksum = 0;
    ComPtr<IMF2DBuffer> buffer_2d;
    if (SUCCEEDED(buffer.As(&buffer_2d))) {
      DWORD contiguous_bytes = 0;
      if (FAILED(buffer_2d->GetContiguousLength(&contiguous_bytes)) ||
          contiguous_bytes < expected_bytes_) {
        fail_locked("Registered 2D camera buffer is smaller than one NV12 frame");
        return false;
      }
      std::vector<BYTE> contiguous(contiguous_bytes);
      if (FAILED(buffer_2d->ContiguousCopyTo(contiguous.data(),
                                             contiguous_bytes))) {
        fail_locked("Unable to copy the registered 2D camera buffer");
        return false;
      }
      checksum = frame_checksum(contiguous.data(), expected_bytes_);
    } else {
      BYTE* bytes = nullptr;
      DWORD maximum_bytes = 0;
      DWORD current_bytes = 0;
      if (FAILED(buffer->Lock(&bytes, &maximum_bytes, &current_bytes))) {
        fail_locked("Unable to read registered camera sample bytes");
        return false;
      }
      if (!bytes || current_bytes < expected_bytes_) {
        (void)buffer->Unlock();
        fail_locked(
            "Contiguous registered camera buffer is smaller than one NV12 frame");
        return false;
      }
      checksum = frame_checksum(bytes, expected_bytes_);
      if (FAILED(buffer->Unlock())) {
        fail_locked("Unable to unlock the registered camera sample buffer");
        return false;
      }
    }

    if (std::find(checksums_.begin(), checksums_.end(), checksum) ==
        checksums_.end()) {
      checksums_.push_back(checksum);
    }
    if (result_.samples == 0) {
      result_.first_timestamp_100ns = sample_timestamp;
      result_.minimum_duration_100ns = duration;
      result_.maximum_duration_100ns = duration;
    } else {
      result_.minimum_duration_100ns =
          std::min(result_.minimum_duration_100ns, duration);
      result_.maximum_duration_100ns =
          std::max(result_.maximum_duration_100ns, duration);
    }
    result_.last_timestamp_100ns = sample_timestamp;
    ++result_.samples;
    return true;
  }

  void fail_locked(std::string error) {
    if (done_) return;
    result_.passed = false;
    result_.distinct_checksums =
        static_cast<std::uint32_t>(checksums_.size());
    result_.error = std::move(error);
    done_ = true;
    condition_.notify_all();
  }

  std::atomic<ULONG> references_{1};
  const std::uint32_t required_samples_;
  const std::uint32_t expected_bytes_;
  const std::int64_t expected_duration_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  IMFSourceReader* reader_{nullptr};
  RegisteredVirtualCameraSmokeResult result_;
  std::vector<std::uint64_t> checksums_;
  std::uint32_t empty_callbacks_{0};
  bool done_{false};
};

RegisteredVirtualCameraSmokeResult consume_registered_camera(
    std::chrono::steady_clock::time_point deadline,
    std::uint32_t required_samples,
    const std::wstring& symbolic_link) {
  RegisteredVirtualCameraSmokeResult result;
  result.supported = true;

  ComPtr<IMFAttributes> source_attributes;
  HRESULT status = MFCreateAttributes(&source_attributes, 2);
  if (SUCCEEDED(status)) {
    status = source_attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  }
  if (SUCCEEDED(status)) {
    status = source_attributes->SetString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        symbolic_link.c_str());
  }
  ComPtr<IMFMediaSource> source;
  if (SUCCEEDED(status)) {
    status = MFCreateDeviceSource(source_attributes.Get(), &source);
  }
  if (FAILED(status)) {
    result.error = hresult_error("MFCreateDeviceSource(registered link)", status);
    return result;
  }

  constexpr std::uint32_t expected_bytes =
      kExpectedWidth * kExpectedHeight * 3U / 2U;
  constexpr std::int64_t expected_duration = 10000000LL / kExpectedFps;
  ComPtr<ReaderCallback> callback;
  callback.Attach(new ReaderCallback(required_samples, expected_bytes,
                                     expected_duration));

  ComPtr<IMFAttributes> reader_attributes;
  status = MFCreateAttributes(&reader_attributes, 3);
  if (SUCCEEDED(status)) {
    status = reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
                                           callback.Get());
  }
  if (SUCCEEDED(status)) {
    status = reader_attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
  }
  if (SUCCEEDED(status)) {
    status = reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                          FALSE);
  }

  ComPtr<IMFSourceReader> reader;
  if (SUCCEEDED(status)) {
    status = MFCreateSourceReaderFromMediaSource(source.Get(),
                                                 reader_attributes.Get(), &reader);
  }
  if (FAILED(status)) {
    result.error = hresult_error("MFCreateSourceReaderFromMediaSource", status);
    source->Shutdown();
    return result;
  }

  ComPtr<IMFMediaType> requested_type;
  status = MFCreateMediaType(&requested_type);
  if (SUCCEEDED(status)) status = requested_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(status)) status = requested_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (SUCCEEDED(status)) {
    status = MFSetAttributeSize(requested_type.Get(), MF_MT_FRAME_SIZE,
                                kExpectedWidth, kExpectedHeight);
  }
  if (SUCCEEDED(status)) {
    status = MFSetAttributeRatio(requested_type.Get(), MF_MT_FRAME_RATE,
                                 kExpectedFps, 1);
  }
  if (SUCCEEDED(status)) {
    status = requested_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                       MFVideoInterlace_Progressive);
  }
  if (SUCCEEDED(status)) {
    status = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  }
  if (SUCCEEDED(status)) {
    status = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  }
  if (SUCCEEDED(status)) {
    status = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                         nullptr, requested_type.Get());
  }

  ComPtr<IMFMediaType> selected_type;
  if (SUCCEEDED(status)) {
    status = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                         &selected_type);
  }
  GUID subtype = GUID_NULL;
  UINT32 width = 0;
  UINT32 height = 0;
  UINT32 fps_numerator = 0;
  UINT32 fps_denominator = 0;
  if (SUCCEEDED(status)) status = selected_type->GetGUID(MF_MT_SUBTYPE, &subtype);
  if (SUCCEEDED(status)) {
    status = MFGetAttributeSize(selected_type.Get(), MF_MT_FRAME_SIZE,
                                &width, &height);
  }
  if (SUCCEEDED(status)) {
    status = MFGetAttributeRatio(selected_type.Get(), MF_MT_FRAME_RATE,
                                 &fps_numerator, &fps_denominator);
  }
  if (FAILED(status)) {
    result.error = hresult_error("NV12 media type negotiation", status);
  } else if (subtype != MFVideoFormat_NV12 || width != kExpectedWidth ||
             height != kExpectedHeight || fps_numerator != kExpectedFps ||
             fps_denominator != 1) {
    result.error = "Registered camera did not negotiate 1920x1080 NV12 at 60p";
  } else {
    callback->attach_reader(reader.Get());
    status = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                nullptr, nullptr, nullptr, nullptr);
    if (FAILED(status)) {
      callback->fail(hresult_error("IMFSourceReader::ReadSample", status));
    }
    const auto now = std::chrono::steady_clock::now();
    callback->wait_for_completion(
        now < deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            : std::chrono::milliseconds(0));
    result = callback->result();
    result.supported = true;
    result.width = width;
    result.height = height;
    result.fps_numerator = fps_numerator;
    result.fps_denominator = fps_denominator;
  }

  reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  callback->detach_reader();
  reader.Reset();
  source->Shutdown();
  return result;
}

} // namespace

RegisteredVirtualCameraSmokeResult run_registered_virtual_camera_smoke(
    std::uint32_t required_samples, std::uint32_t timeout_ms) {
  RegisteredVirtualCameraSmokeResult result;
  result.supported = true;
  if (required_samples < 2) {
    result.error = "Registered camera smoke test requires at least two samples";
    return result;
  }
  if (timeout_ms == 0) {
    result.error = "Registered camera smoke test timeout must be positive";
    return result;
  }

  ComRuntime com;
  if (!com.available()) {
    result.error = hresult_error("CoInitializeEx", com.status());
    return result;
  }
  MediaFoundationRuntime media_foundation;
  if (!media_foundation.available()) {
    result.error = hresult_error("MFStartup", media_foundation.status());
    return result;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  const auto enumeration_deadline =
      std::min(deadline, std::chrono::steady_clock::now() +
                             std::chrono::seconds(5));
  const auto symbolic_link =
      wait_for_persistent_camera(enumeration_deadline, result.error);
  if (symbolic_link.empty()) return result;
  result = consume_registered_camera(deadline, required_samples, symbolic_link);
  return result;
}

} // namespace vividcam
