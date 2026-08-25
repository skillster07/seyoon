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

[[nodiscard]] bool is_known_message_type(MessageType type) noexcept;
[[nodiscard]] std::string_view protocol_error_message(ProtocolError error) noexcept;

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
