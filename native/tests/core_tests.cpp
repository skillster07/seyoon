#include "vividcam/frame_scheduler.hpp"
#include "vividcam/camera_devices.hpp"
#include "vividcam/camera_capture.hpp"
#include "vividcam/latest_frame_buffer.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/gpu_pixel_converter.hpp"
#include "vividcam/frame_compositor.hpp"
#include "vividcam/frame_output_hub.hpp"
#include "vividcam/latency_tracker.hpp"
#include "vividcam/media_foundation_adapter.hpp"
#include "vividcam/media_foundation_source.hpp"
#include "vividcam/layer_resources.hpp"
#include "vividcam/output_profile.hpp"
#include "vividcam/pixel_conversion.hpp"
#include "vividcam/scene_graph.hpp"
#include "vividcam/virtual_camera_stream.hpp"
#include "vividcam/virtual_camera_registration.hpp"
#include "vividcam/virtual_camera_activation.hpp"
#include "vividcam/virtual_camera_media_source.hpp"
#include "vividcam/virtual_camera_media_type.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
  assert(vividcam::kVirtualCameraSourceClsid ==
         "{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}");
  assert(vividcam::kVirtualCameraFriendlyName == "VIVIDCAM Virtual Camera");
  using namespace vividcam;
  using namespace std::chrono_literals;

  const auto soop = default_profile(Platform::Soop);
  assert(soop.valid());
  assert(soop.width == 1920 && soop.height == 1080);
  assert(soop.frames_per_second == 60);
  assert(soop.bitrate_kbps == 8000);
  assert(!soop.portrait());

  const auto tiktok = default_profile(Platform::TikTok);
  assert(tiktok.valid());
  assert(tiktok.frames_per_second == 60);
  assert(tiktok.portrait());

  OutputProfile invalid = soop;
  invalid.frames_per_second = 24;
  assert(!invalid.valid());

  FrameScheduler scheduler(60);
  const FrameScheduler::TimePoint epoch{};
  scheduler.reset(epoch);
  assert(scheduler.frame_index() == 0);
  assert(scheduler.advance(epoch + 17ms) == 0);
  assert(scheduler.frame_index() == 1);
  assert(scheduler.advance(epoch + 70ms) == 2);
  assert(scheduler.dropped_frames() == 2);

  bool rejected_zero = false;
  try { FrameScheduler invalid_scheduler(0); }
  catch (const std::invalid_argument&) { rejected_zero = true; }
  assert(rejected_zero);

  const std::vector<CameraFormat> formats = {
      {1280, 720, 60, 1, PixelFormat::Nv12},
      {1920, 1080, 30, 1, PixelFormat::Nv12},
      {1920, 1080, 60, 1, PixelFormat::Mjpeg},
      {1920, 1080, 60, 1, PixelFormat::Nv12},
  };
  const auto preferred = select_preferred_format(formats);
  assert(preferred.has_value());
  assert(preferred->width == 1920 && preferred->height == 1080);
  assert(preferred->frames_per_second() == 60.0);
  assert(preferred->pixel_format == PixelFormat::Nv12);
  assert(preferred->description().find("1920x1080") != std::string::npos);

  const std::vector<CameraFormat> fallback_formats = {
      {1920, 1080, 30, 1, PixelFormat::Nv12},
      {1280, 720, 60, 1, PixelFormat::Yuy2},
  };
  const auto fallback = select_preferred_format(fallback_formats);
  assert(fallback.has_value());
  assert(fallback->width == 1280 && fallback->frames_per_second() == 60.0);

  const std::vector<CameraFormat> invalid_formats = {{0, 0, 0, 0, PixelFormat::Unknown}};
  assert(!select_preferred_format(invalid_formats).has_value());

  LatestFrameBuffer<CapturedFrame> latest_frames;
  latest_frames.push({1, 100, 166667, formats.back(), {1, 2, 3}, std::nullopt});
  latest_frames.push({2, 200, 166667, formats.back(), {4, 5, 6}, std::nullopt});
  assert(latest_frames.published_frames() == 2);
  assert(latest_frames.overwritten_frames() == 1);
  const auto latest = latest_frames.take();
  assert(latest.has_value() && latest->sequence == 2);
  assert(latest->bytes.size() == 3 && latest->bytes.front() == 4);
  assert(latest_frames.consumed_frames() == 1);
  assert(!latest_frames.take().has_value());

#ifndef _WIN32
  auto capture = create_camera_capture_session();
  std::string capture_error;
  assert(!capture->start(L"unsupported", formats.back(), {}, capture_error));
  assert(!capture_error.empty());
  assert(!capture->running());
  const auto gpu = create_gpu_context();
  assert(!gpu.succeeded());
  assert(gpu.context == nullptr && !gpu.error.empty());
  auto compositor = create_frame_compositor(nullptr);
  std::string compositor_error;
  assert(!compositor->configure({}, compositor_error));
  assert(!compositor_error.empty() && !compositor->valid());
#endif

  assert(valid_compositor_config({1920, 1080, 60, CanvasOrientation::Landscape}));
  assert(valid_compositor_config({1080, 1920, 60, CanvasOrientation::Portrait}));
  assert(!valid_compositor_config({1920, 1080, 24, CanvasOrientation::Landscape}));
  assert(valid_render_plan({}));

  LatencyTracker latency(5);
  for (const double value : {1.0, 2.0, 3.0, 4.0, 100.0, 5.0}) latency.record(value);
  latency.record(-1.0);
  const auto latency_snapshot = latency.snapshot();
  assert(latency_snapshot.samples == 5);
  assert(latency_snapshot.p50_ms == 4.0);
  assert(latency_snapshot.p95_ms == 100.0);
  assert(latency_snapshot.max_ms == 100.0);
  assert(latency_snapshot.average_ms == 22.8);
  latency.reset();
  assert(latency.snapshot().samples == 0);

  const auto templates = built_in_scene_templates();
  assert(templates.size() == 3);
  SceneGraph scene;
  std::string scene_error;
  const bool applied_soop = scene.apply_template(templates.front(), scene_error);
  assert(applied_soop);
  assert(scene.canvas_width() == 1920 && scene.canvas_height() == 1080);
  assert(scene.layers().size() == 3);
  auto plan = scene.render_plan();
  assert(plan.size() == 3);
  assert(plan.front().layer_id == "background");
  assert(plan.back().layer_id == "title");
  assert(valid_render_plan(plan));
  auto duplicate_camera_plan = plan;
  duplicate_camera_plan.push_back(plan[1]);
  assert(!valid_render_plan(duplicate_camera_plan));

  assert(!scene.remove_layer("background", scene_error));
  assert(scene_error == "Locked layer cannot be removed");
  assert(!scene.update_transform("background", {}, scene_error));
  assert(!scene.move_layer("background", 99, scene_error));
  const bool title_hidden = scene.set_visibility("title", false, scene_error);
  assert(title_hidden);
  assert(scene.render_plan().size() == 2);
  const bool title_shown = scene.set_visibility("title", true, scene_error);
  assert(title_shown);
  const bool camera_moved = scene.move_layer("camera", 99, scene_error);
  assert(camera_moved);
  plan = scene.render_plan();
  assert(plan.back().layer_id == "camera");

  SceneLayer duplicate = scene.layers().front();
  assert(!scene.add_layer(duplicate, scene_error));
  LayerTransform invalid_transform;
  invalid_transform.opacity = std::numeric_limits<double>::quiet_NaN();
  assert(!scene.update_transform("title", invalid_transform, scene_error));
  invalid_transform = {};
  invalid_transform.x = 0.5;
  invalid_transform.width = 0.6;
  assert(!invalid_transform.valid());

  const bool applied_tiktok = scene.apply_template(templates[1], scene_error);
  assert(applied_tiktok);
  assert(scene.canvas_width() == 1080 && scene.canvas_height() == 1920);
  assert(scene.find_layer("safe-title") != nullptr);
  assert(std::string(layer_kind_name(LayerKind::Camera)) == "Camera");

  FrameOutputHub output_hub;
  std::string output_error;
  assert(output_hub.register_consumer("preview", OutputConsumerKind::Preview, output_error));
  assert(output_hub.register_consumer("virtual-camera", OutputConsumerKind::VirtualCamera,
                                      output_error));
  assert(!output_hub.register_consumer("preview", OutputConsumerKind::Preview, output_error));
  assert(output_hub.consumer_count() == 2);
  assert(!output_hub.publish({}, output_error));
  const auto texture_owner = std::make_shared<int>(7);
  const CompositedFrame output_frame{10, 1000, 1920, 1080, texture_owner, 7};
  assert(output_hub.publish(output_frame, output_error));
  const CompositedFrame newer_frame{11, 1100, 1920, 1080, texture_owner, 7};
  assert(output_hub.publish(newer_frame, output_error));
  const auto preview_frame = output_hub.take_latest("preview");
  assert(preview_frame && preview_frame->source_sequence == 11);
  const auto preview_stats = output_hub.statistics("preview");
  assert(preview_stats && preview_stats->published_frames == 2);
  assert(preview_stats->consumed_frames == 1 && preview_stats->overwritten_frames == 1);
  assert(output_hub.take_latest("missing") == std::nullopt);
  assert(output_hub.unregister_consumer("virtual-camera"));
  assert(output_hub.consumer_count() == 1);
  assert(std::string(output_consumer_kind_name(OutputConsumerKind::Ndi)) == "NDI");

  VirtualCameraStream virtual_camera(soop);
  assert(!virtual_camera.request_sample());
  assert(virtual_camera.statistics().underruns == 1);
  CompositedFrame wrong_size{20, 2000, 1080, 1920, texture_owner, 7};
  assert(!virtual_camera.submit(wrong_size, output_error));
  assert(virtual_camera.statistics().rejected_frames == 1);
  assert(virtual_camera.submit(output_frame, output_error));
  assert(virtual_camera.submit(newer_frame, output_error));
  assert(virtual_camera.statistics().overwritten_frames == 1);
  const auto first_virtual_sample = virtual_camera.request_sample();
  assert(first_virtual_sample && first_virtual_sample->frame.source_sequence == 11);
  assert(first_virtual_sample->timestamp_100ns == 0);
  assert(first_virtual_sample->duration_100ns == 166666);
  assert(!first_virtual_sample->repeated);
  const auto repeated_virtual_sample = virtual_camera.request_sample();
  assert(repeated_virtual_sample && repeated_virtual_sample->repeated);
  assert(repeated_virtual_sample->timestamp_100ns == 166666);
  auto virtual_stats = virtual_camera.statistics();
  assert(virtual_stats.submitted_frames == 2 && virtual_stats.delivered_samples == 2);
  assert(virtual_stats.repeated_samples == 1);
  assert(virtual_camera.configure(tiktok, output_error));
  assert(virtual_camera.profile().portrait());
  virtual_stats = virtual_camera.statistics();
  assert(virtual_stats.delivered_samples == 0 && virtual_stats.underruns == 0);

  VirtualCameraStream timeline(soop);
  assert(timeline.submit(output_frame, output_error));
  std::optional<VirtualCameraSample> timeline_sample;
  for (int index = 0; index < 60; ++index) timeline_sample = timeline.request_sample();
  assert(timeline_sample);
  assert(timeline_sample->timestamp_100ns == 9'833'333);
  assert(timeline_sample->timestamp_100ns + timeline_sample->duration_100ns == 10'000'000);
  assert(timeline.statistics().repeated_samples == 59);

  LayerResourceStore layer_resources;
  std::string resource_error;
  assert(!layer_resources.put_image({"broken", 2, 2, {0, 0, 0, 0}}, resource_error));
  assert(layer_resources.put_image({"blue", 1, 1, {0, 0, 255, 255}}, resource_error));
  assert(layer_resources.put_text({"caption", "LIVE", "sans-serif", 48.0, 0xFFFFFFFF},
                                  resource_error));
  assert(layer_resources.size() == 2);
  const std::vector<RenderCommand> reference_plan = {
      {"background", {LayerKind::Color, "", 0xFF0000FF}, {}, BlendMode::Normal, 0},
      {"overlay", {LayerKind::Image, "blue", 0},
       {0.5, 0.0, 0.5, 1.0, 0.0, 0.5}, BlendMode::Normal, 1},
      {"caption", {LayerKind::Text, "caption", 0},
       {0.0, 0.0, 1.0, 0.5, 0.0, 1.0}, BlendMode::Normal, 2},
  };
  const auto reference = render_reference_scene(2, 2, reference_plan,
                                                layer_resources, resource_error);
  assert(reference && reference->rendered_layers == 2 && reference->skipped_layers == 1);
  assert(reference->canvas.rgba.size() == 16);
  assert(reference->canvas.rgba[0] == 255 && reference->canvas.rgba[2] == 0);
  assert(reference->canvas.rgba[4] == 127 && reference->canvas.rgba[6] == 128);
  auto missing_plan = reference_plan;
  missing_plan[1].source.resource = "missing";
  assert(!render_reference_scene(2, 2, missing_plan, layer_resources, resource_error));
  assert(!render_reference_scene(0, 2, reference_plan, layer_resources, resource_error));
  assert(layer_resources.remove("caption"));
  assert(!layer_resources.text("caption"));

  VirtualCameraMediaSourceCore media_source(soop, 2);
  std::string media_error;
  assert(!media_source.request_sample({1}, media_error));
  assert(media_source.start(media_error));
  assert(media_source.start(media_error));
  assert(!media_source.configure(tiktok, media_error));
  assert(media_source.request_sample({101}, media_error));
  assert(!media_source.pump());
  assert(media_source.pending_requests() == 1);
  assert(media_source.submit_frame(output_frame, media_error));
  const auto media_first = media_source.pump();
  assert(media_first && media_first->token == 101 && media_first->discontinuity);
  assert(!media_first->sample.repeated && media_first->sample.timestamp_100ns == 0);
  assert(media_source.request_sample({102}, media_error));
  const auto media_repeat = media_source.pump();
  assert(media_repeat && media_repeat->sample.repeated && !media_repeat->discontinuity);
  assert(media_source.request_sample({103}, media_error));
  assert(media_source.request_sample({104}, media_error));
  assert(!media_source.request_sample({105}, media_error));
  assert(media_source.flush() == 2);
  assert(media_source.submit_frame(newer_frame, media_error));
  assert(media_source.request_sample({106}, media_error));
  const auto after_flush = media_source.pump();
  assert(after_flush && after_flush->discontinuity);
  assert(after_flush->sample.timestamp_100ns == 0);
  const auto media_stats = media_source.statistics();
  assert(media_stats.requested_samples == 5 && media_stats.fulfilled_samples == 3);
  assert(media_stats.rejected_requests == 2 && media_stats.starved_pumps == 1);
  assert(media_stats.flushed_requests == 2);
  assert(media_source.stop(media_error));
  assert(media_source.configure(tiktok, media_error));
  media_source.shutdown();
  assert(media_source.state() == MediaSourceState::Shutdown);
  assert(!media_source.start(media_error));
  assert(!media_source.submit_frame(wrong_size, media_error));

  const auto soop_media_types = supported_virtual_camera_media_types(soop);
  assert(soop_media_types.size() == 2);
  assert(soop_media_types[0].pixel_format == VirtualCameraPixelFormat::Nv12);
  assert(soop_media_types[0].valid());
  assert(soop_media_types[0].stride_bytes == 1920);
  assert(soop_media_types[0].sample_size_bytes == 3'110'400);
  assert(soop_media_types[1].pixel_format == VirtualCameraPixelFormat::Bgra);
  assert(soop_media_types[1].stride_bytes == 7680);
  assert(soop_media_types[1].sample_size_bytes == 8'294'400);
  assert(soop_media_types[1].frames_per_second() == 60.0);
  const auto preferred_media_type = negotiate_virtual_camera_media_type(soop, {}, media_error);
  assert(preferred_media_type &&
         preferred_media_type->pixel_format == VirtualCameraPixelFormat::Nv12);
  const auto bgra_media_type = negotiate_virtual_camera_media_type(
      soop, {{}, {}, 60, VirtualCameraPixelFormat::Bgra}, media_error);
  assert(bgra_media_type && bgra_media_type->pixel_format == VirtualCameraPixelFormat::Bgra);
  const auto unsupported_fps = negotiate_virtual_camera_media_type(
      soop, {{}, {}, 30, {}}, media_error);
  assert(!unsupported_fps && !media_error.empty());
  const auto portrait_media_types = supported_virtual_camera_media_types(tiktok);
  assert(portrait_media_types.size() == 2);
  assert(portrait_media_types[0].width == 1080 && portrait_media_types[0].height == 1920);
  OutputProfile invalid_media_profile = soop;
  invalid_media_profile.frames_per_second = 24;
  assert(supported_virtual_camera_media_types(invalid_media_profile).empty());

  const VirtualCameraRegistrationConfig session_registration{
      L"VIVIDCAM Virtual Camera", L"{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}",
      VirtualCameraLifetime::Session, VirtualCameraAccess::CurrentUser};
  assert(session_registration.valid());
  auto invalid_registration = session_registration;
  invalid_registration.source_clsid = L"not-a-clsid";
  assert(!invalid_registration.valid());
  invalid_registration = session_registration;
  invalid_registration.access = VirtualCameraAccess::AllUsers;
  assert(!invalid_registration.valid());
  auto persistent_registration = session_registration;
  persistent_registration.lifetime = VirtualCameraLifetime::System;
  assert(persistent_registration.valid());
  persistent_registration.access = VirtualCameraAccess::AllUsers;
  assert(persistent_registration.valid());

  std::string conversion_error;
  const std::vector<std::uint8_t> black_bgra(16, 0);
  const auto black_nv12 = convert_bgra_to_nv12_bt709(2, 2, black_bgra, 8,
                                                      conversion_error);
  assert(black_nv12 && black_nv12->valid());
  assert(black_nv12->bytes == std::vector<std::uint8_t>({16, 16, 16, 16, 128, 128}));
  assert(black_nv12->uv_plane_offset() == 4);

  std::vector<std::uint8_t> white_bgra(16, 255);
  const auto white_nv12 = convert_bgra_to_nv12_bt709(2, 2, white_bgra, 8,
                                                      conversion_error);
  assert(white_nv12);
  assert(white_nv12->bytes == std::vector<std::uint8_t>({235, 235, 235, 235, 128, 128}));

  std::vector<std::uint8_t> red_bgra(16, 255);
  for (std::size_t pixel = 0; pixel < 4; ++pixel) {
    red_bgra[pixel * 4] = 0;
    red_bgra[pixel * 4 + 1] = 0;
  }
  const auto red_nv12 = convert_bgra_to_nv12_bt709(2, 2, red_bgra, 8,
                                                    conversion_error);
  assert(red_nv12);
  assert(red_nv12->bytes == std::vector<std::uint8_t>({63, 63, 63, 63, 102, 240}));

  const std::vector<std::uint8_t> padded_black_bgra(24, 0);
  const auto padded_nv12 = convert_bgra_to_nv12_bt709(2, 2, padded_black_bgra, 12,
                                                       conversion_error);
  assert(padded_nv12 && padded_nv12->valid());
  assert(!convert_bgra_to_nv12_bt709(3, 2, padded_black_bgra, 12, conversion_error));
  assert(!convert_bgra_to_nv12_bt709(2, 2, {0, 0, 0}, 8, conversion_error));

  assert(valid_gpu_conversion_output(soop_media_types[0]));
  assert(!valid_gpu_conversion_output(soop_media_types[1]));
#ifndef _WIN32
  auto gpu_converter = create_gpu_pixel_converter(nullptr);
  std::string gpu_conversion_error;
  assert(!gpu_converter->valid());
  assert(!gpu_converter->configure(soop_media_types[0], gpu_conversion_error));
  assert(!gpu_converter->convert(output_frame, gpu_conversion_error));
  assert(!gpu_conversion_error.empty());
  const auto native_media_type = create_media_foundation_media_type(
      soop_media_types[0], gpu_conversion_error);
  assert(!native_media_type.valid());
  const ConvertedGpuFrame fake_converted{output_frame, VirtualCameraPixelFormat::Nv12};
  const auto native_sample = create_media_foundation_gpu_sample(
      fake_converted, 0, 166666, true, gpu_conversion_error);
  assert(!native_sample.valid());
  const auto native_event_queue = create_media_foundation_event_queue(gpu_conversion_error);
  assert(!native_event_queue.valid());
  assert(!queue_media_foundation_event(native_event_queue,
                                       MediaFoundationEventKind::StreamStarted, {}, 0,
                                       gpu_conversion_error));
  assert(!take_media_foundation_event(native_event_queue, gpu_conversion_error).valid());
  assert(!shutdown_media_foundation_event_queue(native_event_queue, gpu_conversion_error));
  const auto native_stream_descriptor = create_media_foundation_stream_descriptor(
      1, {native_media_type}, gpu_conversion_error);
  assert(!native_stream_descriptor.valid());
  const auto native_presentation = create_media_foundation_presentation_descriptor(
      native_stream_descriptor, gpu_conversion_error);
  assert(!native_presentation.valid());
  const auto native_source = create_media_foundation_virtual_camera_source(
      soop, gpu_conversion_error);
  assert(!native_source.valid());
  assert(!submit_media_foundation_virtual_camera_sample(
      native_source, native_sample, gpu_conversion_error));
  assert(!start_media_foundation_virtual_camera_source(native_source, gpu_conversion_error));
  assert(!request_media_foundation_virtual_camera_sample(native_source, gpu_conversion_error));
  assert(!take_media_foundation_virtual_camera_stream_event(
      native_source, gpu_conversion_error).valid());
  assert(!stop_media_foundation_virtual_camera_source(native_source, gpu_conversion_error));
  assert(!shutdown_media_foundation_virtual_camera_source(
      native_source, gpu_conversion_error));
  const auto registered_camera = register_and_start_virtual_camera(
      session_registration, gpu_conversion_error);
  assert(!registered_camera.valid());
  assert(!stop_registered_virtual_camera(registered_camera, gpu_conversion_error));
  assert(!remove_registered_virtual_camera(registered_camera, gpu_conversion_error));
#endif

  std::cout << "VIVIDCAM native core tests passed\n";
  return 0;
}
