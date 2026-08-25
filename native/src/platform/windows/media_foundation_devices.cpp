#include "vividcam/camera_devices.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <string>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

std::wstring allocated_string(IMFActivate* activation, const GUID& key) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  if (FAILED(activation->GetAllocatedString(key, &value, &length))) return {};
  std::wstring result(value, length);
  CoTaskMemFree(value);
  return result;
}

PixelFormat pixel_format_from_guid(const GUID& subtype) noexcept {
  if (subtype == MFVideoFormat_NV12) return PixelFormat::Nv12;
  if (subtype == MFVideoFormat_YUY2) return PixelFormat::Yuy2;
  if (subtype == MFVideoFormat_ARGB32 || subtype == MFVideoFormat_RGB32) return PixelFormat::Bgra;
  if (subtype == MFVideoFormat_MJPG) return PixelFormat::Mjpeg;
  if (subtype == MFVideoFormat_H264) return PixelFormat::H264;
  return PixelFormat::Unknown;
}

std::vector<CameraFormat> enumerate_formats(IMFActivate* activation) {
  std::vector<CameraFormat> formats;
  ComPtr<IMFMediaSource> source;
  if (FAILED(activation->ActivateObject(IID_PPV_ARGS(&source)))) return formats;

  ComPtr<IMFSourceReader> reader;
  if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
    for (DWORD index = 0;; ++index) {
      ComPtr<IMFMediaType> media_type;
      const HRESULT status = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                        index, &media_type);
      if (status == MF_E_NO_MORE_TYPES) break;
      if (FAILED(status)) continue;

      UINT32 width = 0;
      UINT32 height = 0;
      UINT32 fps_numerator = 0;
      UINT32 fps_denominator = 1;
      GUID subtype = GUID_NULL;
      if (FAILED(MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
          FAILED(MFGetAttributeRatio(media_type.Get(), MF_MT_FRAME_RATE,
                                     &fps_numerator, &fps_denominator)) ||
          FAILED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        continue;
      }
      formats.push_back({width, height, fps_numerator, fps_denominator,
                         pixel_format_from_guid(subtype)});
    }
  }
  source->Shutdown();
  activation->ShutdownObject();
  return formats;
}
} // namespace

CameraEnumerationResult enumerate_camera_devices() {
  CameraEnumerationResult result;
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool owns_com = SUCCEEDED(com_result);
  if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
    result.error = "CoInitializeEx failed";
    return result;
  }

  const HRESULT mf_result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(mf_result)) {
    if (owns_com) CoUninitialize();
    result.error = "MFStartup failed";
    return result;
  }

  ComPtr<IMFAttributes> attributes;
  HRESULT status = MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(status)) {
    status = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  }

  IMFActivate** activations = nullptr;
  UINT32 count = 0;
  if (SUCCEEDED(status)) status = MFEnumDeviceSources(attributes.Get(), &activations, &count);

  if (SUCCEEDED(status)) {
    result.devices.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
      result.devices.push_back({
          allocated_string(activations[index], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME),
          allocated_string(activations[index], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK),
          enumerate_formats(activations[index]),
      });
      activations[index]->Release();
    }
    CoTaskMemFree(activations);
  } else {
    result.error = "MFEnumDeviceSources failed";
  }

  MFShutdown();
  if (owns_com) CoUninitialize();
  return result;
}

} // namespace vividcam
