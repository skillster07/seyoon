cmake_minimum_required(VERSION 3.22)
project(VividCamNative LANGUAGES CXX)

option(VIVIDCAM_BUILD_TESTS "Build native core tests" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()

add_library(vividcam_core
  src/control_channel_state.cpp
  src/engine_host.cpp
  src/engine_options.cpp
  src/output_profile.cpp
  src/pixel_conversion.cpp
  src/producer_ipc_protocol.cpp
  src/frame_scheduler.cpp
  src/camera_formats.cpp
  src/gpu_context.cpp
  src/gpu_pixel_converter.cpp
  src/latency_tracker.cpp
  src/layer_resources.cpp
  src/frame_compositor.cpp
  src/frame_output_hub.cpp
  src/scene_graph.cpp
  src/virtual_camera_stream.cpp
  src/virtual_camera_media_source.cpp
  src/virtual_camera_media_type.cpp
  src/virtual_camera_registration.cpp
)
target_include_directories(vividcam_core PUBLIC include)
target_compile_features(vividcam_core PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(vividcam_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(vividcam_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WIN32)
  add_library(vividcam_platform
    src/platform/windows/control_channel_transport.cpp
    src/platform/windows/producer_identity.cpp
    src/platform/windows/media_foundation_devices.cpp
    src/platform/windows/media_foundation_capture.cpp
    src/platform/windows/d3d11_gpu_context.cpp
    src/platform/windows/d3d11_gpu_pixel_converter.cpp
    src/platform/windows/media_foundation_adapter.cpp
    src/platform/windows/media_foundation_source.cpp
    src/platform/windows/virtual_camera_registration.cpp
    src/platform/windows/registered_virtual_camera_smoke.cpp
    src/platform/windows/d3d11_frame_compositor.cpp
  )
  target_link_libraries(vividcam_platform PUBLIC vividcam_core mfplat mf mfreadwrite mfuuid mfsensorgroup ole32 d3d11 dxgi bcrypt advapi32)
  target_compile_definitions(vividcam_platform PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

  add_library(vividcam_virtual_camera_source SHARED
    src/platform/windows/virtual_camera_activation_server.cpp
    src/platform/windows/vividcam_virtual_camera_source.def
  )
  target_link_libraries(vividcam_virtual_camera_source PRIVATE vividcam_core vividcam_platform)
  target_compile_definitions(vividcam_virtual_camera_source PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(vividcam_virtual_camera_source PROPERTIES PREFIX "")
else()
  add_library(vividcam_platform
    src/platform/portable/control_channel_transport.cpp
    src/platform/portable/producer_identity.cpp
    src/platform/portable/camera_devices.cpp
    src/platform/portable/camera_capture.cpp
    src/platform/portable/gpu_context.cpp
    src/platform/portable/gpu_pixel_converter.cpp
    src/platform/portable/media_foundation_adapter.cpp
    src/platform/portable/media_foundation_source.cpp
    src/platform/portable/virtual_camera_registration.cpp
    src/platform/portable/registered_virtual_camera_smoke.cpp
    src/platform/portable/frame_compositor.cpp
  )
  target_link_libraries(vividcam_platform PUBLIC vividcam_core)
endif()
target_include_directories(vividcam_platform PUBLIC include)

add_executable(vividcam_diagnostics src/diagnostics_main.cpp)
target_link_libraries(vividcam_diagnostics PRIVATE vividcam_core vividcam_platform)

add_executable(vividcam_engine src/engine_main.cpp)
target_link_libraries(vividcam_engine PRIVATE vividcam_core vividcam_platform)
if(MSVC)
  target_compile_options(vividcam_engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(vividcam_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(VIVIDCAM_BUILD_TESTS)
  enable_testing()
  add_executable(vividcam_core_tests tests/core_tests.cpp)
  target_link_libraries(vividcam_core_tests PRIVATE vividcam_core vividcam_platform)
  if(MSVC)
    target_compile_options(vividcam_core_tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(vividcam_core_tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME vividcam_core_tests COMMAND vividcam_core_tests)

  add_executable(vividcam_engine_tests tests/engine_host_tests.cpp)
  target_link_libraries(vividcam_engine_tests PRIVATE vividcam_core)
  if(MSVC)
    target_compile_options(vividcam_engine_tests PRIVATE /W4 /permissive- /UNDEBUG)
  else()
    target_compile_options(vividcam_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                           -UNDEBUG)
  endif()
  add_test(NAME vividcam_engine_tests COMMAND vividcam_engine_tests)

  add_executable(vividcam_control_channel_state_tests
    tests/control_channel_state_tests.cpp)
  target_link_libraries(vividcam_control_channel_state_tests PRIVATE vividcam_core)
  if(MSVC)
    target_compile_options(vividcam_control_channel_state_tests PRIVATE
      /W4 /permissive- /UNDEBUG)
  else()
    target_compile_options(vividcam_control_channel_state_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  add_test(NAME vividcam_control_channel_state_tests
    COMMAND vividcam_control_channel_state_tests)

  add_executable(vividcam_producer_ipc_protocol_tests
    tests/producer_ipc_protocol_tests.cpp)
  target_link_libraries(vividcam_producer_ipc_protocol_tests PRIVATE vividcam_core)
  if(MSVC)
    target_compile_options(vividcam_producer_ipc_protocol_tests PRIVATE
      /W4 /permissive- /UNDEBUG)
  else()
    target_compile_options(vividcam_producer_ipc_protocol_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  add_test(NAME vividcam_producer_ipc_protocol_tests
    COMMAND vividcam_producer_ipc_protocol_tests)

  add_executable(vividcam_producer_identity_tests
    tests/producer_identity_tests.cpp)
  target_link_libraries(vividcam_producer_identity_tests PRIVATE
    vividcam_platform)
  if(MSVC)
    target_compile_options(vividcam_producer_identity_tests PRIVATE
      /W4 /permissive- /UNDEBUG)
  else()
    target_compile_options(vividcam_producer_identity_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  add_test(NAME vividcam_producer_identity_tests
    COMMAND vividcam_producer_identity_tests)

  if(WIN32)
    add_executable(vividcam_control_channel_transport_tests
      tests/control_channel_transport_tests.cpp)
    target_link_libraries(vividcam_control_channel_transport_tests PRIVATE
      vividcam_core vividcam_platform)
    if(MSVC)
      target_compile_options(vividcam_control_channel_transport_tests PRIVATE
        /W4 /permissive- /UNDEBUG)
    else()
      target_compile_options(vividcam_control_channel_transport_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    endif()
    add_test(NAME vividcam_control_channel_transport_tests
      COMMAND vividcam_control_channel_transport_tests)
    set_tests_properties(vividcam_control_channel_transport_tests PROPERTIES
      TIMEOUT 20)

    add_test(NAME vividcam_install_registry_helpers
      COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
              -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-install-registry-helpers.ps1)
    set_tests_properties(vividcam_install_registry_helpers PROPERTIES
      TIMEOUT 20)

    add_test(NAME vividcam_camera_update_helpers
      COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
              -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-camera-update-helpers.ps1)
    set_tests_properties(vividcam_camera_update_helpers PROPERTIES
      TIMEOUT 20)
  endif()

  add_test(NAME vividcam_engine_bounded_smoke
           COMMAND ${CMAKE_COMMAND}
                   -DENGINE_EXECUTABLE=$<TARGET_FILE:vividcam_engine>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_engine_smoke.cmake)
  set_tests_properties(vividcam_engine_bounded_smoke PROPERTIES
    TIMEOUT 7)
endif()
