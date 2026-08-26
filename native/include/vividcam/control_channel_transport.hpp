#pragma once

#include "vividcam/cpu_frame_transport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vividcam {

inline constexpr std::uint32_t kControlChannelTransportSchemaVersion = 1;
inline constexpr std::wstring_view kVividCamPrimaryControlRoute =
    L"vividcam.virtual-camera.source.{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}";

struct ControlChannelTransportSnapshot {
  std::uint32_t schema_version{kControlChannelTransportSchemaVersion};
  bool running{false};
  bool connected{false};
  std::uint64_t connection_attempts{0};
  std::uint64_t successful_handshakes{0};
  std::uint64_t heartbeats_sent{0};
  std::uint64_t heartbeat_acks{0};
  std::uint64_t protocol_errors{0};
  std::uint64_t rejected_peers{0};
  std::uint32_t peer_process_id{0};
  std::string last_error;
};

enum class CpuFramePublishResult {
  Published,
  TransportUnavailable,
  MailboxChanged,
  Failed,
};

// Verifies that a registered VIVIDCAM endpoint has a nonempty symbolic link,
// then returns the stable, non-secret single-camera source identity above.
// The canonical route is hashed and is never embedded directly in a pipe path.
[[nodiscard]] bool find_registered_vividcam_control_route(
    std::wstring& route, std::string& error);

// Derives a non-identifying pipe name from the SHA-256 digest of the UTF-16
// route bytes.
[[nodiscard]] bool make_vividcam_control_pipe_name(
    std::wstring_view route, std::wstring& pipe_name, std::string& error);

class ProducerControlServer {
 public:
  ProducerControlServer();
  ~ProducerControlServer();
  ProducerControlServer(const ProducerControlServer&) = delete;
  ProducerControlServer& operator=(const ProducerControlServer&) = delete;

  [[nodiscard]] bool start(std::wstring route, std::string engine_instance_id,
                           std::string& error);
  void stop() noexcept;
  [[nodiscard]] ControlChannelTransportSnapshot snapshot() const;
  // Publishes only when the currently negotiated mailbox still has the name
  // observed by the caller. The comparison and publish happen while holding
  // the same control-session lock, so a frame built for an old connection can
  // never leak into a replacement mailbox.
  [[nodiscard]] CpuFramePublishResult publish_cpu_frame_for_mailbox(
      const CpuNv12Frame& frame, std::wstring_view expected_mailbox_name,
      std::string& error);
  [[nodiscard]] CpuFrameMailboxSnapshot frame_mailbox_snapshot() const;
  [[nodiscard]] std::wstring frame_mailbox_name() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class SourceControlClient {
 public:
  SourceControlClient();
  ~SourceControlClient();
  SourceControlClient(const SourceControlClient&) = delete;
  SourceControlClient& operator=(const SourceControlClient&) = delete;

  // On Windows the client validates the named-pipe server PID, executable
  // basename, token principal, and session before sending SourceHello. This is
  // a defense-in-depth local peer gate, not cryptographic nonce/signature
  // authentication.
  [[nodiscard]] bool start(std::wstring route, std::string& error);
  void stop() noexcept;
  [[nodiscard]] ControlChannelTransportSnapshot snapshot() const;
  // Returns no frame (without an error) while disconnected, negotiating,
  // stale, reconnecting, or when no newer mailbox generation is available.
  [[nodiscard]] std::optional<CpuNv12Frame> take_latest_cpu_frame(
      std::string& error);
  [[nodiscard]] CpuFrameMailboxSnapshot frame_mailbox_snapshot() const;
  [[nodiscard]] std::wstring frame_mailbox_name() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vividcam
