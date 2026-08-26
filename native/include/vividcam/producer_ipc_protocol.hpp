#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vividcam::producer_ipc {

inline constexpr std::array<std::uint8_t, 4> kMagic = {
    0x56U, 0x43U, 0x49U, 0x50U}; // "VCIP"
inline constexpr std::uint16_t kHeaderBytes = 64;
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U;

// W4b-2b keeps large frame bytes outside VCIP. These compact payloads only
// negotiate the exact stream and bounded CPU mailbox layout.
inline constexpr std::uint16_t kNegotiationPayloadSchemaVersion = 1;
inline constexpr std::uint16_t kOpenStreamPayloadBytes = 48;
inline constexpr std::uint16_t kTransportOfferPayloadBytes = 40;
inline constexpr std::uint16_t kTransportDescriptorPayloadBytes = 40;
inline constexpr std::uint16_t kCpuFrameMailboxLayoutMajor = 1;
inline constexpr std::uint16_t kCpuFrameMailboxLayoutMinor = 0;
inline constexpr std::uint32_t kCpuFrameMailboxSlotCount = 2;
inline constexpr std::uint32_t kCpuFrameMailboxHeaderBytes = 4096;
inline constexpr std::uint32_t kCpuFrameMailboxSlotMetadataBytes = 64;
inline constexpr std::uint32_t kCpuFrameMailboxAlignmentBytes = 4096;
inline constexpr std::uint32_t kMaximumFrameDimension = 8192;
inline constexpr std::uint32_t kMaximumCpuFrameBytes = 16U * 1024U * 1024U;

using ConnectionId = std::array<std::uint8_t, 16>;

enum class MessageType : std::uint16_t {
  SourceHello = 0x0001,
  ProducerHello = 0x0002,
  OpenStream = 0x0010,
  StreamReady = 0x0011,
  StopStream = 0x0012,
  StreamStopped = 0x0013,
  ProducerState = 0x0020,
  Heartbeat = 0x0021,
  HeartbeatAck = 0x0022,
  TransportOffer = 0x0030,
  TransportAccepted = 0x0031,
  Error = 0x00f0,
  Goodbye = 0x00f1,
};

enum class ProtocolError {
  None,
  OutputBufferTooSmall,
  HeaderTruncated,
  WrongMagic,
  WrongHeaderSize,
  UnsupportedMajorVersion,
  UnsupportedMinorVersion,
  UnknownMessageType,
  PayloadTooLarge,
  PayloadSizeMismatch,
  PayloadTruncated,
  TrailingBytes,
  NonzeroReserved0,
  NonzeroReserved1,
  ZeroMessageSequence,
};

enum class FramePixelFormat : std::uint32_t {
  Nv12 = 1,
  Bgra = 2,
};

enum class FrameTransportKind : std::uint32_t {
  CpuSharedMemory = 1,
};

enum class NegotiationPayloadError {
  None,
  WrongPayloadSize,
  UnsupportedSchemaVersion,
  InvalidStreamId,
  UnknownPixelFormat,
  InvalidDimensions,
  InvalidFrameRate,
  InvalidStride,
  InvalidFrameBytes,
  UnknownTransportKind,
  UnsupportedTransportLayout,
  InvalidSlotCount,
  InvalidCapacity,
  ArithmeticOverflow,
  ContractMismatch,
  NonzeroFlags,
  NonzeroReserved,
};

struct MessageHeader {
  std::uint16_t header_bytes{kHeaderBytes};
  std::uint16_t protocol_major{kProtocolMajor};
  std::uint16_t protocol_minor{kProtocolMinor};
  MessageType message_type{MessageType::SourceHello};
  std::uint32_t flags{0};
  std::uint32_t payload_bytes{0};
  std::uint32_t reserved0{0};
  std::uint64_t message_sequence{0};
  std::uint64_t correlation_id{0};
  ConnectionId connection_id{};
  std::uint64_t reserved1{0};
};

struct MessageView {
  MessageHeader header;
  std::span<const std::byte> payload;
};

struct OpenStreamPayload {
  std::uint16_t schema_version{kNegotiationPayloadSchemaVersion};
  std::uint16_t payload_bytes{kOpenStreamPayloadBytes};
  std::uint32_t stream_id{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t frame_rate_numerator{0};
  std::uint32_t frame_rate_denominator{0};
  FramePixelFormat pixel_format{FramePixelFormat::Nv12};
  std::uint32_t plane0_stride_bytes{0};
  std::uint32_t plane1_stride_bytes{0};
  std::uint32_t frame_bytes{0};
  std::uint32_t flags{0};
  std::uint32_t reserved{0};
};

struct TransportOfferPayload {
  std::uint16_t schema_version{kNegotiationPayloadSchemaVersion};
  std::uint16_t payload_bytes{kTransportOfferPayloadBytes};
  FrameTransportKind transport_kind{FrameTransportKind::CpuSharedMemory};
  std::uint16_t layout_major{kCpuFrameMailboxLayoutMajor};
  std::uint16_t layout_minor{kCpuFrameMailboxLayoutMinor};
  std::uint32_t slot_count{kCpuFrameMailboxSlotCount};
  std::uint32_t mapping_header_bytes{kCpuFrameMailboxHeaderBytes};
  std::uint32_t frame_capacity_bytes{0};
  std::uint64_t mapping_capacity_bytes{0};
  std::uint32_t flags{0};
  std::uint32_t reserved{0};
};

// TransportAccepted and StreamReady both carry this exact descriptor. Echoing
// the selected layout prevents either peer from silently changing the offer.
struct TransportDescriptorPayload {
  std::uint16_t schema_version{kNegotiationPayloadSchemaVersion};
  std::uint16_t payload_bytes{kTransportDescriptorPayloadBytes};
  FrameTransportKind transport_kind{FrameTransportKind::CpuSharedMemory};
  std::uint16_t layout_major{kCpuFrameMailboxLayoutMajor};
  std::uint16_t layout_minor{kCpuFrameMailboxLayoutMinor};
  std::uint32_t slot_count{kCpuFrameMailboxSlotCount};
  std::uint32_t mapping_header_bytes{kCpuFrameMailboxHeaderBytes};
  std::uint32_t frame_capacity_bytes{0};
  std::uint64_t mapping_capacity_bytes{0};
  std::uint32_t flags{0};
  std::uint32_t reserved{0};
};

[[nodiscard]] bool is_known_message_type(MessageType type) noexcept;
[[nodiscard]] std::string_view protocol_error_message(ProtocolError error) noexcept;
[[nodiscard]] std::string_view negotiation_payload_error_message(
    NegotiationPayloadError error) noexcept;

// Returns the one valid mapping size for a two-slot CPU mailbox with the
// supplied frame capacity. Zero means invalid input or arithmetic overflow.
[[nodiscard]] std::uint64_t cpu_frame_mapping_capacity(
    std::uint32_t frame_capacity_bytes) noexcept;

[[nodiscard]] NegotiationPayloadError encode_open_stream_payload(
    const OpenStreamPayload& payload, std::vector<std::byte>& destination);
[[nodiscard]] NegotiationPayloadError decode_open_stream_payload(
    std::span<const std::byte> source, OpenStreamPayload& payload) noexcept;

[[nodiscard]] NegotiationPayloadError encode_transport_offer_payload(
    const TransportOfferPayload& payload,
    std::vector<std::byte>& destination);
[[nodiscard]] NegotiationPayloadError decode_transport_offer_payload(
    std::span<const std::byte> source,
    TransportOfferPayload& payload) noexcept;

[[nodiscard]] NegotiationPayloadError encode_transport_descriptor_payload(
    const TransportDescriptorPayload& payload,
    std::vector<std::byte>& destination);
[[nodiscard]] NegotiationPayloadError decode_transport_descriptor_payload(
    std::span<const std::byte> source,
    TransportDescriptorPayload& payload) noexcept;

// Cross-message validation is intentionally public so the control state
// machine cannot accept individually valid payloads that describe different
// stream/layout contracts or silently change an accepted offer.
[[nodiscard]] NegotiationPayloadError validate_transport_offer_for_open_stream(
    const OpenStreamPayload& stream,
    const TransportOfferPayload& offer) noexcept;
[[nodiscard]] NegotiationPayloadError validate_transport_descriptor_for_offer(
    const TransportOfferPayload& offer,
    const TransportDescriptorPayload& descriptor) noexcept;

// Header encoding is explicit and byte-oriented. It never serializes the native
// MessageHeader object representation.
[[nodiscard]] ProtocolError encode_header(
    const MessageHeader& header, std::span<std::byte> destination) noexcept;

// The payload size must match header.payload_bytes exactly.
[[nodiscard]] ProtocolError encode_message(
    const MessageHeader& header, std::span<const std::byte> payload,
    std::vector<std::byte>& destination);

// decode_header validates the fixed header but deliberately does not require its
// input to contain the payload. This permits a stream reader to learn the frame
// length before reading the rest of the message.
[[nodiscard]] ProtocolError decode_header(
    std::span<const std::byte> source, MessageHeader& header) noexcept;

// decode_message is strict: the source must contain one complete message and no
// trailing bytes. The returned payload view borrows storage from source.
[[nodiscard]] ProtocolError decode_message(
    std::span<const std::byte> source, MessageView& message) noexcept;

} // namespace vividcam::producer_ipc
