#include "vividcam/camera_devices.hpp"
#include "vividcam/camera_capture.hpp"
#include "vividcam/control_channel_transport.hpp"
#include "vividcam/frame_scheduler.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/gpu_pixel_converter.hpp"
#include "vividcam/frame_compositor.hpp"
#include "vividcam/frame_output_hub.hpp"
#include "vividcam/layer_resources.hpp"
#include "vividcam/media_foundation_adapter.hpp"
#include "vividcam/media_foundation_source.hpp"
#include "vividcam/output_profile.hpp"
#include "vividcam/pixel_conversion.hpp"
#include "vividcam/registered_virtual_camera_smoke.hpp"
#include "vividcam/scene_graph.hpp"
#include "vividcam/virtual_camera_media_source.hpp"
#include "vividcam/virtual_camera_media_type.hpp"
#include "vividcam/virtual_camera_registration.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <devicetopology.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
namespace {

enum class VideoSourceLookupStatus { Found, Absent, Error };

struct VideoSourceLookupResult {
  VideoSourceLookupStatus status = VideoSourceLookupStatus::Absent;
  std::wstring symbolic_link;
  std::string error;
};

std::string diagnostic_hresult_error(const char* operation, HRESULT status) {
  std::ostringstream message;
  message << operation << " failed (HRESULT=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}

VideoSourceLookupResult find_video_source_by_name_prefix(
    std::wstring_view target_name) {
  using Microsoft::WRL::ComPtr;

  ComPtr<IMFAttributes> attributes;
  const char* operation = "MFCreateAttributes(video sources)";
  HRESULT status = MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(status)) {
    operation = "IMFAttributes::SetGUID(video source type)";
    status = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  }
  if (FAILED(status)) {
    return {VideoSourceLookupStatus::Error, {},
            diagnostic_hresult_error(operation, status)};
  }

  IMFActivate** devices = nullptr;
  UINT32 device_count = 0;
  status = MFEnumDeviceSources(attributes.Get(), &devices, &device_count);

  const auto release_devices = [&]() noexcept {
    if (devices != nullptr) {
      for (UINT32 index = 0; index < device_count; ++index) {
        if (devices[index] != nullptr) devices[index]->Release();
      }
    }
    CoTaskMemFree(devices);
  };

  VideoSourceLookupResult result;
  if (FAILED(status)) {
    result.status = VideoSourceLookupStatus::Error;
    result.error = diagnostic_hresult_error("MFEnumDeviceSources", status);
    release_devices();
    return result;
  }
  if (device_count != 0 && devices == nullptr) {
    result.status = VideoSourceLookupStatus::Error;
    result.error = "MFEnumDeviceSources returned a null device array";
    release_devices();
    return result;
  }

  for (UINT32 index = 0; index < device_count; ++index) {
    if (devices[index] == nullptr) {
      result.status = VideoSourceLookupStatus::Error;
      result.error = "MFEnumDeviceSources returned a null device";
      break;
    }

    wchar_t* friendly_name = nullptr;
    UINT32 friendly_name_length = 0;
    const HRESULT name_status = devices[index]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly_name,
        &friendly_name_length);
    if (FAILED(name_status) || friendly_name == nullptr) {
      CoTaskMemFree(friendly_name);
      result.status = VideoSourceLookupStatus::Error;
      result.error = FAILED(name_status)
                         ? diagnostic_hresult_error(
                               "IMFActivate::GetAllocatedString(friendly name)",
                               name_status)
                         : "A video source returned a null friendly name";
      break;
    }
    // Windows appends a platform suffix such as "(Windows Virtual Camera)"
    // to the configured friendly name exposed by MFEnumDeviceSources. Match
    // the configured portion exactly (ordinal, case-insensitive), consistent
    // with the registered-source and control-route validators.
    const bool name_matches =
        friendly_name_length >= target_name.size() &&
        CompareStringOrdinal(
            friendly_name, static_cast<int>(target_name.size()),
            target_name.data(), static_cast<int>(target_name.size()), TRUE) ==
            CSTR_EQUAL;
    CoTaskMemFree(friendly_name);
    if (!name_matches) continue;

    wchar_t* symbolic_link = nullptr;
    UINT32 symbolic_link_length = 0;
    const HRESULT link_status = devices[index]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        &symbolic_link, &symbolic_link_length);
    if (FAILED(link_status) || symbolic_link == nullptr ||
        symbolic_link_length == 0) {
      CoTaskMemFree(symbolic_link);
      result.status = VideoSourceLookupStatus::Error;
      result.error =
          FAILED(link_status)
              ? diagnostic_hresult_error(
                    "IMFActivate::GetAllocatedString(symbolic link)",
                    link_status)
              : "The VIVIDCAM video source returned an empty symbolic link";
      break;
    }

    const std::wstring candidate_link(symbolic_link, symbolic_link_length);
    CoTaskMemFree(symbolic_link);
    if (result.status == VideoSourceLookupStatus::Absent) {
      result.status = VideoSourceLookupStatus::Found;
      result.symbolic_link = candidate_link;
    } else if (result.symbolic_link != candidate_link) {
      result.status = VideoSourceLookupStatus::Error;
      result.symbolic_link.clear();
      result.error =
          "Multiple VIVIDCAM video sources have the same friendly name";
      break;
    }
  }

  release_devices();
  return result;
}

} // namespace
#endif

int main(int argc, char** argv) {
  using namespace vividcam;
  const bool render_test = argc > 1 && std::string_view(argv[1]) == "--render-test";
  const bool activation_test = argc > 1 &&
                               std::string_view(argv[1]) == "--activation-test";
  const bool registration_test = argc > 1 &&
                                 std::string_view(argv[1]) == "--register-test";
  const bool registered_source_test = argc > 1 &&
                                      std::string_view(argv[1]) ==
                                          "--registered-source-test";
  const bool control_client_test = argc > 1 &&
                                   std::string_view(argv[1]) ==
                                       "--control-client-test";
  const bool install_camera = argc > 1 &&
                              std::string_view(argv[1]) == "--install-camera";
  const bool stop_camera = argc > 1 &&
                           std::string_view(argv[1]) == "--stop-camera";
  const bool remove_camera = argc > 1 &&
                             std::string_view(argv[1]) == "--remove-camera";
  const bool capture_test = render_test ||
                            (argc > 1 && std::string_view(argv[1]) == "--capture-test");
  std::cout << "VIVIDCAM native diagnostics\n";
  if (activation_test) {
#ifdef _WIN32
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(com_status);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
      std::cout << "[activation] COM initialization failed\n";
      return 4;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
      if (owns_com) CoUninitialize();
      std::cout << "[activation] Media Foundation initialization failed\n";
      return 4;
    }

    using Microsoft::WRL::ComPtr;
    HRESULT status = S_OK;
    const char* operation = "CLSIDFromString";
    CLSID source_clsid{};
    status = CLSIDFromString(L"{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}",
                             &source_clsid);
    ComPtr<IMFActivate> activation;
    if (SUCCEEDED(status)) {
      operation = "CoCreateInstance(IMFActivate)";
      status = CoCreateInstance(source_clsid, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&activation));
    }
    constexpr wchar_t diagnostic_link[] = L"vividcam-diagnostic-symbolic-link";
    if (SUCCEEDED(status)) {
      operation = "IMFActivate attribute store";
      status = activation->SetString(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
          diagnostic_link);
    }

    ComPtr<IMFMediaSource> source;
    if (SUCCEEDED(status)) {
      operation = "IMFActivate::ActivateObject(IMFMediaSource)";
      status = activation->ActivateObject(IID_PPV_ARGS(&source));
    }

    ComPtr<IMFMediaSourceEx> source_ex;
    ComPtr<IMFGetService> get_service;
    ComPtr<IKsControl> ks_control;
    ComPtr<IMFSampleAllocatorControl> allocator_control;
    if (SUCCEEDED(status)) {
      operation = "required media source QueryInterface";
      status = source.As(&source_ex);
    }
    if (SUCCEEDED(status)) status = source.As(&get_service);
    if (SUCCEEDED(status)) status = source.As(&ks_control);
    if (SUCCEEDED(status)) status = source.As(&allocator_control);

    ComPtr<IMFAttributes> source_attributes;
    ComPtr<IMFAttributes> stream_attributes;
    ComPtr<IMFPresentationDescriptor> presentation;
    ComPtr<IMFStreamDescriptor> stream_descriptor;
    DWORD stream_count = 0;
    DWORD stream_id = 0;
    BOOL selected = FALSE;
    GUID stream_category = GUID_NULL;
    UINT32 shared = 0;
    UINT32 frame_source_types = 0;
    wchar_t copied_link[64]{};
    if (SUCCEEDED(status)) {
      operation = "media source/stream attribute contract";
      status = source_ex->GetSourceAttributes(&source_attributes);
    }
    if (SUCCEEDED(status)) {
      status = source_attributes->GetString(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
          copied_link, static_cast<UINT32>(std::size(copied_link)), nullptr);
    }
    if (SUCCEEDED(status) &&
        std::wstring_view(copied_link) != diagnostic_link) {
      status = E_FAIL;
    }
    if (SUCCEEDED(status)) status = source->CreatePresentationDescriptor(&presentation);
    if (SUCCEEDED(status)) status = presentation->GetStreamDescriptorCount(&stream_count);
    if (SUCCEEDED(status) && stream_count != 1) status = E_FAIL;
    if (SUCCEEDED(status)) {
      status = presentation->GetStreamDescriptorByIndex(
          0, &selected, &stream_descriptor);
    }
    if (SUCCEEDED(status)) status = stream_descriptor->GetStreamIdentifier(&stream_id);
    if (SUCCEEDED(status) && stream_id != 0) status = E_FAIL;
    if (SUCCEEDED(status)) {
      status = stream_descriptor->GetGUID(
          MF_DEVICESTREAM_STREAM_CATEGORY, &stream_category);
    }
    if (SUCCEEDED(status) && stream_category != PINNAME_VIDEO_CAPTURE) status = E_FAIL;
    if (SUCCEEDED(status)) {
      status = source_ex->GetStreamAttributes(stream_id, &stream_attributes);
    }
    if (SUCCEEDED(status)) {
      status = stream_attributes->GetUINT32(
          MF_DEVICESTREAM_FRAMESERVER_SHARED, &shared);
    }
    if (SUCCEEDED(status)) {
      status = stream_attributes->GetUINT32(
          MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, &frame_source_types);
    }
    if (SUCCEEDED(status) &&
        (!selected || shared != 1 ||
         (frame_source_types & MFFrameSourceTypes_Color) == 0)) {
      status = E_FAIL;
    }

    DWORD input_stream_id = 0;
    MFSampleAllocatorUsage allocator_usage{};
    if (SUCCEEDED(status)) {
      operation = "IMFSampleAllocatorControl contract";
      status = allocator_control->GetAllocatorUsage(
          stream_id, &input_stream_id, &allocator_usage);
    }
    if (SUCCEEDED(status) &&
        (input_stream_id != stream_id ||
         allocator_usage != MFSampleAllocatorUsage_UsesCustomAllocator)) {
      status = E_FAIL;
    }

    KSPROPERTY property{};
    property.Flags = KSPROPERTY_TYPE_GET;
    ULONG bytes_returned = 0;
    if (SUCCEEDED(status)) {
      operation = "IKsControl unsupported-property contract";
      const HRESULT ks_status = ks_control->KsProperty(
          &property, sizeof(property), nullptr, 0, &bytes_returned);
      if (ks_status != HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND)) status = E_FAIL;
    }
    if (SUCCEEDED(status)) {
      operation = "IMFMediaSourceEx::SetD3DManager contract";
      if (source_ex->SetD3DManager(nullptr) != E_NOTIMPL) status = E_FAIL;
    }

    if (SUCCEEDED(status)) {
      std::cout << "[activation] IMFActivate/media source contract valid; streams="
                << stream_count << " id=" << stream_id << '\n';
    } else {
      std::cout << "[activation] " << operation << " failed (HRESULT=0x"
                << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(status) << ")\n";
    }
    source.Reset();
    activation.Reset();
    MFShutdown();
    if (owns_com) CoUninitialize();
    return SUCCEEDED(status) ? 0 : 4;
#else
    std::cout << "[activation] Windows Media Foundation is unavailable\n";
    return 4;
#endif
  }
  if (registration_test) {
    std::string registration_error;
#ifdef _WIN32
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(com_status);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
      std::cout << "[registration] COM initialization failed\n";
      return 4;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
      if (owns_com) CoUninitialize();
      std::cout << "[registration] Media Foundation initialization failed\n";
      return 4;
    }
#endif
    int registration_result = 4;
    const VirtualCameraRegistrationConfig config{
        L"VIVIDCAM Registration Lifecycle Test",
        L"{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}",
        VirtualCameraLifetime::Session, VirtualCameraAccess::CurrentUser};
    {
      const auto camera = register_and_start_virtual_camera(config, registration_error);
      if (!camera.valid()) {
        std::cout << "[registration] create/start failed: " << registration_error << '\n';
      } else {
        const bool stopped = stop_registered_virtual_camera(camera, registration_error);
        const bool removed = remove_registered_virtual_camera(camera, registration_error);
        std::cout << "[registration] stopped=" << (stopped ? "yes" : "no")
                  << " removed=" << (removed ? "yes" : "no") << '\n';
        registration_result = stopped && removed ? 0 : 4;
      }
    }
#ifdef _WIN32
    MFShutdown();
    if (owns_com) CoUninitialize();
#endif
    return registration_result;
  }
  if (registered_source_test) {
    const auto result = run_registered_virtual_camera_smoke();
    if (!result.supported) {
      std::cout << "[registered-source] unavailable: " << result.error << '\n';
      return 5;
    }
    if (!result.passed) {
      std::cout << "[registered-source] failed after " << result.samples
                << " sample(s), empty callbacks=" << result.empty_callbacks
                << ", flags=0x" << std::hex << std::uppercase
                << result.source_reader_flags << std::dec << ": "
                << result.error << '\n';
      return 5;
    }
    std::cout << "[registered-source] samples=" << result.samples
              << " type=" << result.width << 'x' << result.height << " NV12 "
              << result.fps_numerator << '/' << result.fps_denominator
              << "p checksums=" << result.distinct_checksums
              << " timestamps=" << result.first_timestamp_100ns << ".."
              << result.last_timestamp_100ns << " delta_avg/min/max="
              << result.average_timestamp_delta_100ns << '/'
              << result.minimum_timestamp_delta_100ns << '/'
              << result.maximum_timestamp_delta_100ns << " durations="
              << result.minimum_duration_100ns << ".."
              << result.maximum_duration_100ns << " empty_callbacks="
              << result.empty_callbacks << " flags=0x" << std::hex
              << std::uppercase << result.source_reader_flags << std::dec
              << " [valid]\n";
    return 0;
  }
  if (control_client_test) {
#ifdef _WIN32
    std::wstring route;
    std::string error;
    if (!find_registered_vividcam_control_route(route, error)) {
      std::cout << "[control-client] route unavailable: " << error << '\n';
      return 5;
    }
    std::wstring pipe_name;
    if (!make_vividcam_control_pipe_name(route, pipe_name, error)) {
      std::cout << "[control-client-denial] pipe unavailable: " << error
                << '\n';
      return 5;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{5};
    DWORD status = ERROR_FILE_NOT_FOUND;
    bool denied = false;
    do {
      const DWORD flags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
      const HANDLE pipe = CreateFileW(
          pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
          OPEN_EXISTING, flags, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        status = ERROR_SUCCESS;
        break;
      }
      status = GetLastError();
      if (status == ERROR_ACCESS_DENIED) {
        denied = true;
        break;
      }
      if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PIPE_BUSY) break;
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    } while (std::chrono::steady_clock::now() < deadline);

    std::cout << "[control-client-denial] win32=" << status
              << (denied ? " [valid]\n" : " [invalid]\n");
    return denied ? 0 : 5;
#else
    std::cout << "[control-client] Windows named pipes are unavailable\n";
    return 5;
#endif
  }
  if (install_camera || stop_camera || remove_camera) {
    std::string registration_error;
#ifdef _WIN32
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(com_status);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
      std::cout << "[persistent-camera] COM initialization failed\n";
      return 4;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
      if (owns_com) CoUninitialize();
      std::cout << "[persistent-camera] Media Foundation initialization failed\n";
      return 4;
    }
#endif
    int registration_result = 4;
    {
      NativeMediaFoundationHandle camera;
      if (install_camera) {
        camera = register_and_start_persistent_virtual_camera(
            L"VIVIDCAM Virtual Camera",
            L"{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}", registration_error);
        if (camera.valid()) {
          const auto symbolic_link =
              registered_virtual_camera_symbolic_link(camera, registration_error);
          if (!symbolic_link.empty()) {
            std::wcout << L"[persistent-camera] installed/started link="
                       << symbolic_link << L'\n';
            registration_result = 0;
          }
        }
      } else {
        const VirtualCameraRegistrationConfig config{
            L"VIVIDCAM Virtual Camera",
            L"{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}",
            VirtualCameraLifetime::System, VirtualCameraAccess::CurrentUser};
        if (stop_camera) {
#ifdef _WIN32
          const auto existing_camera =
              find_video_source_by_name_prefix(config.friendly_name);
          if (existing_camera.status == VideoSourceLookupStatus::Absent) {
            std::cout << "[persistent-camera] not-installed\n";
            registration_result = 3;
          } else if (existing_camera.status == VideoSourceLookupStatus::Error) {
            registration_error = existing_camera.error;
          } else {
            camera =
                create_virtual_camera_registration(config, registration_error);
            if (camera.valid() &&
                stop_registered_virtual_camera(camera, registration_error)) {
              std::wcout << L"[persistent-camera] stopped link="
                         << existing_camera.symbolic_link << L'\n';
              registration_result = 0;
            }
          }
#else
          registration_error =
              "Windows video source enumeration is unavailable";
#endif
        } else {
          camera = create_virtual_camera_registration(config, registration_error);
        }
        if (remove_camera && camera.valid() &&
            remove_registered_virtual_camera(camera, registration_error)) {
          std::cout << "[persistent-camera] removed\n";
          registration_result = 0;
        }
      }
      if (registration_result == 4) {
        const char* operation = install_camera   ? "install/start"
                                : stop_camera    ? "stop"
                                                 : "remove";
        std::cout << "[persistent-camera] "
                  << operation << " failed: " << registration_error << '\n';
      }
    }
#ifdef _WIN32
    MFShutdown();
    if (owns_com) CoUninitialize();
#endif
    return registration_result;
  }
  for (const auto platform : {Platform::Soop, Platform::TikTok, Platform::Obs}) {
    const auto profile = default_profile(platform);
    std::cout << "[profile] " << profile.description() << (profile.valid() ? " [valid]\n" : " [invalid]\n");
    for (const auto& media_type : supported_virtual_camera_media_types(profile)) {
      std::cout << "  [media-type] " << media_type.description()
                << (media_type.valid() ? " [valid]\n" : " [invalid]\n");
    }
  }
  for (const auto& scene_template : built_in_scene_templates()) {
    SceneGraph scene;
    std::string error;
    const bool applied = scene.apply_template(scene_template, error);
    std::cout << "[scene] " << scene_template.id << ' ' << scene_template.canvas_width
              << 'x' << scene_template.canvas_height << " layers="
              << scene_template.layers.size() << " commands="
              << (applied ? scene.render_plan().size() : 0)
              << (applied ? " [valid]\n" : " [invalid]\n");
  }
  LayerResourceStore reference_resources;
  std::string reference_error;
  if (!reference_resources.put_image({"diagnostic-pixel", 1, 1, {30, 90, 220, 255}},
                                     reference_error)) {
    std::cout << "[reference-render] resource failed: " << reference_error << '\n';
    return 2;
  }
  const std::vector<RenderCommand> reference_plan = {
      {"background", {LayerKind::Color, "", 0x171522FF}, {}, BlendMode::Normal, 0},
      {"image", {LayerKind::Image, "diagnostic-pixel", 0},
       {0.25, 0.25, 0.5, 0.5, 0.0, 0.75}, BlendMode::Normal, 1}};
  const auto reference = render_reference_scene(4, 4, reference_plan,
                                                reference_resources, reference_error);
  std::cout << "[reference-render] layers=" << (reference ? reference->rendered_layers : 0)
            << " skipped=" << (reference ? reference->skipped_layers : 0)
            << (reference ? " [valid]\n" : " [invalid]\n");
  const std::vector<std::uint8_t> diagnostic_bgra = {
      0, 0, 255, 255, 0, 0, 255, 255,
      0, 0, 255, 255, 0, 0, 255, 255};
  const auto diagnostic_nv12 = convert_bgra_to_nv12_bt709(
      2, 2, diagnostic_bgra, 8, reference_error);
  std::cout << "[pixel-conversion] BGRA->NV12 bytes="
            << (diagnostic_nv12 ? diagnostic_nv12->bytes.size() : 0)
            << (diagnostic_nv12 && diagnostic_nv12->valid() ? " [valid]\n" : " [invalid]\n");

  FrameScheduler scheduler(60);
  std::cout << "[clock] 60p frame scheduler ready; first deadline scheduled\n";

  const auto gpu = create_gpu_context(true);
  if (gpu.succeeded()) {
    const auto info = gpu.context->info();
    std::cout << "[gpu] " << gpu_backend_name(info.backend) << " / "
              << info.adapter_name << " / video=" << (info.video_support ? "yes" : "no") << '\n';
  } else {
    std::cout << "[gpu] " << gpu.error << '\n';
  }

  const auto cameras = enumerate_camera_devices();
  if (!cameras.supported()) {
    std::cout << "[camera] " << cameras.error << '\n';
    return capture_test ? 2 : 0;
  }
  std::cout << "[camera] " << cameras.devices.size() << " device(s) found\n";
  for (const auto& camera : cameras.devices) {
    std::wcout << L"  - " << camera.friendly_name << L" (" << camera.formats.size()
               << L" native format(s))\n";
    if (const auto preferred = select_preferred_format(camera.formats)) {
      std::cout << "    preferred: " << preferred->description() << '\n';
    } else {
      std::cout << "    preferred: no valid capture format\n";
    }
  }

  const auto capture_camera = std::find_if(
      cameras.devices.begin(), cameras.devices.end(), [](const auto& camera) {
        return camera.friendly_name.find(L"VIVIDCAM") == std::wstring::npos;
      });

  if (capture_test && capture_camera == cameras.devices.end()) {
    std::cout << "[capture] no non-VIVIDCAM input camera is available\n";
    return 2;
  }

  if (capture_test) {
    const auto preferred = select_preferred_format(capture_camera->formats);
    if (!preferred) {
      std::cout << "[capture] first camera has no valid format\n";
      return 2;
    }
    auto capture = create_camera_capture_session();
    auto compositor = create_frame_compositor(gpu.context);
    auto gpu_converter = create_gpu_pixel_converter(gpu.context);
    FrameOutputHub output_hub;
    std::string output_error;
    if (!output_hub.register_consumer("diagnostic-preview", OutputConsumerKind::Preview,
                                      output_error)) {
      std::cout << "[output] registration failed: " << output_error << '\n';
      return 2;
    }
    std::string compositor_error;
    const OutputProfile virtual_profile = default_profile(Platform::Soop);
    const CompositorConfig compositor_config{
        virtual_profile.width, virtual_profile.height,
        virtual_profile.frames_per_second,
        virtual_profile.portrait() ? CanvasOrientation::Portrait
                                   : CanvasOrientation::Landscape};
    VirtualCameraMediaSourceCore virtual_camera(virtual_profile);
    std::string virtual_error;
    if (!virtual_camera.start(virtual_error)) {
      std::cout << "[virtual-camera] start failed: " << virtual_error << '\n';
      return 2;
    }
    if (gpu.succeeded() && !compositor->configure(compositor_config, compositor_error)) {
      std::cout << "[compositor] configure failed: " << compositor_error << '\n';
      return 2;
    }
    const auto conversion_types = supported_virtual_camera_media_types(virtual_profile);
    if (gpu.succeeded() && (conversion_types.empty() ||
        !gpu_converter->configure(conversion_types.front(), compositor_error))) {
      std::cout << "[gpu-conversion] configure failed: " << compositor_error << '\n';
      return 2;
    }
    NativeMediaFoundationHandle native_event_queue;
    NativeMediaFoundationHandle native_source;
    std::uint64_t media_foundation_descriptors = 0;
    if (gpu.succeeded()) {
      std::vector<NativeMediaFoundationHandle> native_types;
      for (const auto& conversion_type : conversion_types) {
        auto native_type = create_media_foundation_media_type(
            conversion_type, compositor_error);
        if (!native_type.valid()) {
          std::cout << "[media-foundation] media type failed: " << compositor_error << '\n';
          return 2;
        }
        native_types.push_back(std::move(native_type));
      }
      const auto stream_descriptor = create_media_foundation_stream_descriptor(
          1, native_types, compositor_error);
      const auto presentation_descriptor = create_media_foundation_presentation_descriptor(
          stream_descriptor, compositor_error);
      if (!stream_descriptor.valid() || !presentation_descriptor.valid()) {
        std::cout << "[media-foundation] descriptor failed: " << compositor_error << '\n';
        return 2;
      }
      media_foundation_descriptors = 2;
      native_source = create_media_foundation_virtual_camera_source(
          virtual_profile, compositor_error);
      if (!native_source.valid() ||
          !start_media_foundation_virtual_camera_source(native_source, compositor_error) ||
          !take_media_foundation_virtual_camera_stream_event(
              native_source, compositor_error).valid()) {
        std::cout << "[media-foundation] source start failed: " << compositor_error << '\n';
        return 2;
      }
      native_event_queue = create_media_foundation_event_queue(compositor_error);
      if (!native_event_queue.valid() ||
          !queue_media_foundation_event(native_event_queue,
                                        MediaFoundationEventKind::StreamStarted, {}, 0,
                                        compositor_error) ||
          !take_media_foundation_event(native_event_queue, compositor_error).valid()) {
        std::cout << "[media-foundation] event queue failed: " << compositor_error << '\n';
        return 2;
      }
    }
    if (gpu.succeeded()) {
      const auto templates = built_in_scene_templates();
      const auto& selected_template = compositor_config.orientation == CanvasOrientation::Portrait
                                          ? templates[1]
                                          : templates[0];
      SceneGraph scene;
      std::string scene_error;
      if (!scene.apply_template(selected_template, scene_error) ||
          !compositor->set_render_plan(scene.render_plan(), scene_error)) {
        std::cout << "[scene] compositor plan failed: " << scene_error << '\n';
        return 2;
      }
    }
    std::string error;
    const CaptureOptions options{gpu.succeeded(), gpu.context};
    if (!capture->start(capture_camera->symbolic_link, *preferred, options, error)) {
      std::cout << "[capture] start failed: " << error << '\n';
      return 2;
    }
    std::cout << "[capture] sampling first camera for 3 seconds with "
              << preferred->description() << '\n';
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::uint64_t latest_sequence = 0;
    std::uint64_t media_foundation_samples = 0;
    std::uint64_t media_foundation_events = 1;
    std::uint64_t media_source_samples = 0;
    while (std::chrono::steady_clock::now() < deadline && capture->running()) {
      if (const auto frame = capture->take_latest_frame()) {
        latest_sequence = frame->sequence;
        if (frame->gpu && compositor->valid()) {
          std::string render_error;
          const auto composited = compositor->render(*frame, render_error);
          if (!composited) {
            std::cout << "[compositor] render failed: " << render_error << '\n';
          } else if (!output_hub.publish(*composited, render_error)) {
            std::cout << "[output] publish failed: " << render_error << '\n';
          } else {
            (void)output_hub.take_latest("diagnostic-preview");
            const auto converted = gpu_converter->convert(*composited, render_error);
            if (converted && virtual_camera.submit_frame(converted->frame, render_error) &&
                virtual_camera.request_sample({converted->frame.source_sequence}, render_error)) {
              const auto response = virtual_camera.pump();
              if (response) {
                const auto native_sample = create_media_foundation_gpu_sample(
                    *converted, response->sample.timestamp_100ns,
                    response->sample.duration_100ns, response->discontinuity, render_error);
                if (native_sample.valid()) {
                  if (queue_media_foundation_event(
                          native_event_queue, MediaFoundationEventKind::MediaSample,
                          native_sample, 0, render_error) &&
                      take_media_foundation_event(native_event_queue, render_error).valid()) {
                    ++media_foundation_samples;
                    ++media_foundation_events;
                  } else {
                    std::cout << "[media-foundation] sample event failed: "
                              << render_error << '\n';
                  }
                  if (request_media_foundation_virtual_camera_sample(
                          native_source, render_error) &&
                      submit_media_foundation_virtual_camera_sample(
                          native_source, native_sample, render_error) &&
                      take_media_foundation_virtual_camera_stream_event(
                          native_source, render_error).valid()) {
                    ++media_source_samples;
                  } else {
                    std::cout << "[media-foundation] source sample failed: "
                              << render_error << '\n';
                  }
                } else {
                  std::cout << "[media-foundation] sample failed: " << render_error << '\n';
                }
              }
            } else {
              std::cout << "[gpu-conversion] convert/submit failed: " << render_error << '\n';
            }
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    capture->stop();
    if (gpu.succeeded()) {
      if (!stop_media_foundation_virtual_camera_source(native_source, error) ||
          !take_media_foundation_virtual_camera_stream_event(
              native_source, error).valid() ||
          !shutdown_media_foundation_virtual_camera_source(native_source, error)) {
        std::cout << "[media-foundation] source stop failed: " << error << '\n';
      }
      if (queue_media_foundation_event(native_event_queue,
                                       MediaFoundationEventKind::StreamStopped, {}, 0,
                                       error) &&
          take_media_foundation_event(native_event_queue, error).valid()) {
        ++media_foundation_events;
      }
      if (!shutdown_media_foundation_event_queue(native_event_queue, error)) {
        std::cout << "[media-foundation] event shutdown failed: " << error << '\n';
      }
    }
    const auto stats = capture->statistics();
    std::cout << "[capture] received=" << stats.received_frames
              << " consumed=" << stats.consumed_frames
              << " overwritten=" << stats.overwritten_frames
              << " errors=" << stats.source_errors
              << " gpu=" << stats.gpu_frames
              << " cpu=" << stats.cpu_frames
              << " latest_sequence=" << latest_sequence << '\n';
    const auto render = compositor->statistics();
    if (compositor->valid()) {
      std::cout << "[render] frames=" << render.rendered_frames
                << " rejected=" << render.rejected_frames
                << " pool=" << render.pool_allocations
                << " skipped_layers=" << render.skipped_layers
                << " p50_ms=" << render.render_latency.p50_ms
                << " p95_ms=" << render.render_latency.p95_ms
                << " max_ms=" << render.render_latency.max_ms << '\n';
    }
    if (const auto output = output_hub.statistics("diagnostic-preview")) {
      std::cout << "[output] published=" << output->published_frames
                << " consumed=" << output->consumed_frames
                << " overwritten=" << output->overwritten_frames << '\n';
    }
    const auto conversion_stats = gpu_converter->statistics();
    std::cout << "[gpu-conversion] frames=" << conversion_stats.converted_frames
              << " rejected=" << conversion_stats.rejected_frames
              << " pool=" << conversion_stats.pool_allocations
              << " p95_ms=" << conversion_stats.conversion_latency.p95_ms << '\n';
    std::cout << "[media-foundation] gpu_samples=" << media_foundation_samples
              << " events=" << media_foundation_events
              << " descriptors=" << media_foundation_descriptors
              << " source_samples=" << media_source_samples << '\n';
    const auto virtual_stats = virtual_camera.stream_statistics();
    const auto media_stats = virtual_camera.statistics();
    std::cout << "[virtual-camera] submitted=" << virtual_stats.submitted_frames
              << " delivered=" << virtual_stats.delivered_samples
              << " repeated=" << virtual_stats.repeated_samples
              << " overwritten=" << virtual_stats.overwritten_frames
              << " underruns=" << virtual_stats.underruns
              << " rejected=" << virtual_stats.rejected_frames
              << " requests=" << media_stats.requested_samples
              << " fulfilled=" << media_stats.fulfilled_samples
              << " starved=" << media_stats.starved_pumps << '\n';
    const bool capture_passed = stats.received_frames > 0 && stats.source_errors == 0;
    const bool render_passed = !render_test ||
        (gpu.succeeded() && render.rendered_frames > 0 && render.rejected_frames == 0 &&
         render.render_latency.p95_ms < 16.67 && conversion_stats.converted_frames > 0 &&
         conversion_stats.rejected_frames == 0 &&
         conversion_stats.conversion_latency.p95_ms < 16.67 &&
         media_foundation_samples > 0 && media_source_samples > 0);
    return capture_passed && render_passed ? 0 : 3;
  }
  return 0;
}
