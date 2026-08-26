#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vividcam {

// W4b-2b intentionally starts with one fixed CPU fallback contract. Dynamic
// formats and D3D11 shared textures are negotiated by later transport versions.
inline constexpr std::uint32_t kCpuFrameWidth = 1920;
inline constexpr std::uint32_t kCpuFrameHeight = 1080;
inline constexpr std::uint32_t kCpuFrameYStrideBytes = kCpuFrameWidth;
inline constexpr std::uint32_t kCpuFrameUvStrideBytes = kCpuFrameWidth;
inline constexpr std::size_t kCpuFrameNv12Bytes =
    static_cast<std::size_t>(kCpuFrameWidth) * kCpuFrameHeight * 3U / 2U;
inline constexpr std::size_t kCpuFrameMailboxSlotCount = 2;
inline constexpr std::uint32_t kCpuFrameMailboxLayoutVersion = 1;

using CpuFrameConnectionId = std::array<std::uint8_t, 16>;

enum class CpuFrameMailboxScope {
  // Local namespace works in an ordinary interactive session and is only for
  // loopback/CI routes that do not cross a Windows session boundary.
  NonProductionLocal,
  // The FrameServer source in session 0 creates this object in Global so the
  // installed medium-integrity engine can open it from the active session.
  ProductionGlobal,
  // Applies the exact production DACL and integrity label in Local namespace.
  // This is a regression-test seam for unprivileged Windows CI and must never
  // be selected by installed runtime code.
  ProductionSecurityLocalTest,
};

struct CpuNv12Frame {
  std::uint64_t sequence{0};
  std::int64_t timestamp_100ns{0};
  std::uint32_t width{kCpuFrameWidth};
  std::uint32_t height{kCpuFrameHeight};
  std::uint32_t y_stride_bytes{kCpuFrameYStrideBytes};
  std::uint32_t uv_stride_bytes{kCpuFrameUvStrideBytes};
  std::vector<std::uint8_t> bytes;

  [[nodiscard]] bool valid() const noexcept;
};

struct CpuFrameMailboxOptions {
  CpuFrameMailboxScope scope{CpuFrameMailboxScope::NonProductionLocal};
  // The lowercase/uppercase 64-hex SHA-256 route digest. The canonical object
  // name lowercases it and never embeds the original registration route.
  std::wstring route_digest;
  CpuFrameConnectionId connection_id{};
  // Required by both production endpoints. It is the canonical EngineUserSid
  // from the protected producer identity manifest and is rechecked against the
  // mapping DACL when the producer opens the data plane.
  std::wstring producer_user_sid;
};

struct CpuFrameMailboxSnapshot {
  bool open{false};
  std::uint64_t published_generation{0};
  std::uint64_t consumed_generation{0};
  std::uint64_t published_frames{0};
  std::uint64_t consumed_frames{0};
  std::uint64_t overwritten_frames{0};
  std::uint64_t torn_reads{0};
  std::uint64_t invalid_frames{0};
};

// These offsets are the stable shared-memory ABI. Multi-byte fields are little
// endian. The aligned 64-bit sequence/counter fields are accessed with Windows
// Interlocked operations rather than process-local C++ mutexes/atomics.
namespace cpu_frame_mailbox_layout {
inline constexpr std::array<std::uint8_t, 4> kMagic = {
    0x56U, 0x43U, 0x46U, 0x4dU}; // "VCFM"
inline constexpr std::size_t kPageBytes = 4096;
inline constexpr std::size_t kHeaderBytes = kPageBytes;
inline constexpr std::size_t kSlotHeaderBytes = 64;
inline constexpr std::size_t kSlotStrideBytes =
    ((kSlotHeaderBytes + kCpuFrameNv12Bytes + kPageBytes - 1U) /
     kPageBytes) *
    kPageBytes;
inline constexpr std::size_t kMappingBytes =
    kHeaderBytes + kCpuFrameMailboxSlotCount * kSlotStrideBytes;

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kLayoutVersionOffset = 4;
inline constexpr std::size_t kHeaderBytesOffset = 6;
inline constexpr std::size_t kMappingBytesOffset = 8;
inline constexpr std::size_t kWidthOffset = 16;
inline constexpr std::size_t kHeightOffset = 20;
inline constexpr std::size_t kPixelFormatOffset = 24;
inline constexpr std::size_t kSlotCountOffset = 28;
inline constexpr std::size_t kYStrideOffset = 32;
inline constexpr std::size_t kUvStrideOffset = 36;
inline constexpr std::size_t kFrameBytesOffset = 40;
inline constexpr std::size_t kConnectionIdOffset = 48;
inline constexpr std::size_t kPublishedGenerationOffset = 128;
inline constexpr std::size_t kConsumedGenerationOffset = 136;
inline constexpr std::size_t kPublishedFramesOffset = 144;
inline constexpr std::size_t kConsumedFramesOffset = 152;
inline constexpr std::size_t kOverwrittenFramesOffset = 160;
inline constexpr std::size_t kTornReadsOffset = 168;
inline constexpr std::size_t kInvalidFramesOffset = 176;
// Exactly one producer may claim a mapping lifetime. Reconnects negotiate a
// new connection ID and therefore a new mapping rather than sharing a writer.
inline constexpr std::size_t kProducerClaimOffset = 184;

inline constexpr std::size_t kSlotBeginGenerationOffset = 0;
inline constexpr std::size_t kSlotEndGenerationOffset = 8;
inline constexpr std::size_t kSlotProducerSequenceOffset = 16;
inline constexpr std::size_t kSlotTimestampOffset = 24;
inline constexpr std::size_t kSlotPayloadBytesOffset = 32;
inline constexpr std::size_t kSlotPayloadOffset = kSlotHeaderBytes;
inline constexpr std::uint32_t kPixelFormatNv12 = 1;

[[nodiscard]] constexpr std::size_t slot_offset(std::size_t index) noexcept {
  return kHeaderBytes + index * kSlotStrideBytes;
}
} // namespace cpu_frame_mailbox_layout

[[nodiscard]] bool make_cpu_frame_mailbox_name(
    const CpuFrameMailboxOptions& options, std::wstring& name,
    std::string& error);

class CpuFrameMailboxSource {
 public:
  ~CpuFrameMailboxSource();
  CpuFrameMailboxSource(const CpuFrameMailboxSource&) = delete;
  CpuFrameMailboxSource& operator=(const CpuFrameMailboxSource&) = delete;

  // One bounded snapshot attempt. It never waits for a producer and never
  // spins if a slot is being overwritten; a torn attempt increments telemetry
  // and returns no frame so Media Foundation can repeat/fallback immediately.
  [[nodiscard]] std::optional<CpuNv12Frame> take_latest(std::string& error);
  [[nodiscard]] CpuFrameMailboxSnapshot snapshot() const;
  [[nodiscard]] std::wstring name() const;
  [[nodiscard]] bool open() const;
  void close() noexcept;

 private:
  class Impl;
  explicit CpuFrameMailboxSource(std::shared_ptr<Impl> impl);
  friend std::shared_ptr<CpuFrameMailboxSource>
  create_cpu_frame_mailbox_source(const CpuFrameMailboxOptions&, std::string&);

  mutable std::mutex mutex_;
  std::shared_ptr<Impl> impl_;
};

class CpuFrameMailboxProducer {
 public:
  ~CpuFrameMailboxProducer();
  CpuFrameMailboxProducer(const CpuFrameMailboxProducer&) = delete;
  CpuFrameMailboxProducer& operator=(const CpuFrameMailboxProducer&) = delete;

  // Copies directly into the inactive shared slot and atomically publishes it.
  // Consumer backpressure never queues frames or blocks on an acknowledgement.
  [[nodiscard]] bool publish(const CpuNv12Frame& frame, std::string& error);
  [[nodiscard]] CpuFrameMailboxSnapshot snapshot() const;
  [[nodiscard]] std::wstring name() const;
  [[nodiscard]] bool open() const;
  void close() noexcept;

 private:
  class Impl;
  explicit CpuFrameMailboxProducer(std::shared_ptr<Impl> impl);
  friend std::shared_ptr<CpuFrameMailboxProducer>
  open_cpu_frame_mailbox_producer(const CpuFrameMailboxOptions&, std::string&);

  mutable std::mutex mutex_;
  std::shared_ptr<Impl> impl_;
};

// The FrameServer-side source creates and initializes the mapping. The engine
// only opens an already initialized mapping after the authenticated control
// channel has accepted the per-connection transport offer.
[[nodiscard]] std::shared_ptr<CpuFrameMailboxSource>
create_cpu_frame_mailbox_source(const CpuFrameMailboxOptions& options,
                                std::string& error);
[[nodiscard]] std::shared_ptr<CpuFrameMailboxProducer>
open_cpu_frame_mailbox_producer(const CpuFrameMailboxOptions& options,
                                std::string& error);

} // namespace vividcam
