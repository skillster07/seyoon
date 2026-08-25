#include "vividcam/producer_ipc_protocol.hpp"

#include <algorithm>
#include <limits>

namespace vividcam::producer_ipc {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderBytesOffset = 4;
constexpr std::size_t kProtocolMajorOffset = 6;
constexpr std::size_t kProtocolMinorOffset = 8;
constexpr std::size_t kMessageTypeOffset = 10;
constexpr std::size_t kFlagsOffset = 12;
constexpr std::size_t kPayloadBytesOffset = 16;
constexpr std::size_t kReserved0Offset = 20;
constexpr std::size_t kMessageSequenceOffset = 24;
constexpr std::size_t kCorrelationIdOffset = 32;
constexpr std::size_t kConnectionIdOffset = 40;
constexpr std::size_t kReserved1Offset = 56;

static_assert(kReserved1Offset + sizeof(std::uint64_t) == kHeaderBytes);

std::uint16_t read_u16(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  const auto low = std::to_integer<std::uint8_t>(source[offset]);
  const auto high = std::to_integer<std::uint8_t>(source[offset + 1]);
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(low) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

void write_u16(std::span<std::byte> destination, std::size_t offset,
               std::uint16_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void write_u32(std::span<std::byte> destination, std::size_t offset,
               std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void write_u64(std::span<std::byte> destination, std::size_t offset,
               std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

ProtocolError validate_header(const MessageHeader& header,
                              bool encoding) noexcept {
  if (header.header_bytes != kHeaderBytes) {
    return ProtocolError::WrongHeaderSize;
  }
  if (header.protocol_major != kProtocolMajor) {
    return ProtocolError::UnsupportedMajorVersion;
  }
  if (encoding && header.protocol_minor != kProtocolMinor) {
    return ProtocolError::UnsupportedMinorVersion;
  }
  if (!is_known_message_type(header.message_type)) {
    return ProtocolError::UnknownMessageType;
  }
  if (header.payload_bytes > kMaximumPayloadBytes) {
    return ProtocolError::PayloadTooLarge;
  }
  if (header.reserved0 != 0) {
    return ProtocolError::NonzeroReserved0;
  }
  if (header.reserved1 != 0) {
    return ProtocolError::NonzeroReserved1;
  }
  if (header.message_sequence == 0) {
    return ProtocolError::ZeroMessageSequence;
  }
  return ProtocolError::None;
}

} // namespace

bool is_known_message_type(MessageType type) noexcept {
  switch (type) {
    case MessageType::SourceHello:
    case MessageType::ProducerHello:
    case MessageType::OpenStream:
    case MessageType::StreamReady:
    case MessageType::StopStream:
    case MessageType::StreamStopped:
    case MessageType::ProducerState:
    case MessageType::Heartbeat:
    case MessageType::HeartbeatAck:
    case MessageType::TransportOffer:
    case MessageType::TransportAccepted:
    case MessageType::Error:
    case MessageType::Goodbye:
      return true;
  }
  return false;
}

std::string_view protocol_error_message(ProtocolError error) noexcept {
  switch (error) {
    case ProtocolError::None:
      return "no protocol error";
    case ProtocolError::OutputBufferTooSmall:
      return "output buffer is smaller than the 64-byte header";
    case ProtocolError::HeaderTruncated:
      return "message is shorter than the 64-byte header";
    case ProtocolError::WrongMagic:
      return "message magic is not VCIP";
    case ProtocolError::WrongHeaderSize:
      return "header_bytes is not 64";
    case ProtocolError::UnsupportedMajorVersion:
      return "protocol major version is not supported";
    case ProtocolError::UnsupportedMinorVersion:
      return "protocol minor version cannot be emitted by this codec";
    case ProtocolError::UnknownMessageType:
      return "message type is unknown";
    case ProtocolError::PayloadTooLarge:
      return "payload exceeds 64 KiB";
    case ProtocolError::PayloadSizeMismatch:
      return "payload size does not match payload_bytes";
    case ProtocolError::PayloadTruncated:
      return "message payload is truncated";
    case ProtocolError::TrailingBytes:
      return "message contains trailing bytes";
    case ProtocolError::NonzeroReserved0:
      return "reserved0 must be zero";
    case ProtocolError::NonzeroReserved1:
      return "reserved1 must be zero";
    case ProtocolError::ZeroMessageSequence:
      return "message_sequence must be nonzero";
  }
  return "unknown protocol error";
}

ProtocolError encode_header(const MessageHeader& header,
                            std::span<std::byte> destination) noexcept {
  const ProtocolError validation = validate_header(header, true);
  if (validation != ProtocolError::None) return validation;
  if (destination.size() < kHeaderBytes) {
    return ProtocolError::OutputBufferTooSmall;
  }

  std::fill_n(destination.begin(), kHeaderBytes, std::byte{0});
  for (std::size_t index = 0; index < kMagic.size(); ++index) {
    destination[kMagicOffset + index] = static_cast<std::byte>(kMagic[index]);
  }
  write_u16(destination, kHeaderBytesOffset, header.header_bytes);
  write_u16(destination, kProtocolMajorOffset, header.protocol_major);
  write_u16(destination, kProtocolMinorOffset, header.protocol_minor);
  write_u16(destination, kMessageTypeOffset,
            static_cast<std::uint16_t>(header.message_type));
  write_u32(destination, kFlagsOffset, header.flags);
  write_u32(destination, kPayloadBytesOffset, header.payload_bytes);
  write_u32(destination, kReserved0Offset, header.reserved0);
  write_u64(destination, kMessageSequenceOffset, header.message_sequence);
  write_u64(destination, kCorrelationIdOffset, header.correlation_id);
  for (std::size_t index = 0; index < header.connection_id.size(); ++index) {
    destination[kConnectionIdOffset + index] =
        static_cast<std::byte>(header.connection_id[index]);
  }
  write_u64(destination, kReserved1Offset, header.reserved1);
  return ProtocolError::None;
}

ProtocolError encode_message(const MessageHeader& header,
                             std::span<const std::byte> payload,
                             std::vector<std::byte>& destination) {
  if (payload.size() > kMaximumPayloadBytes) {
    return ProtocolError::PayloadTooLarge;
  }
  if (header.payload_bytes != payload.size()) {
    return ProtocolError::PayloadSizeMismatch;
  }
  const ProtocolError validation = validate_header(header, true);
  if (validation != ProtocolError::None) return validation;

  destination.assign(static_cast<std::size_t>(kHeaderBytes) + payload.size(),
                     std::byte{0});
  const ProtocolError encoded = encode_header(header, destination);
  if (encoded != ProtocolError::None) {
    destination.clear();
    return encoded;
  }
  std::copy(payload.begin(), payload.end(),
            destination.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  return ProtocolError::None;
}

ProtocolError decode_header(std::span<const std::byte> source,
                            MessageHeader& header) noexcept {
  header = {};
  if (source.size() < kHeaderBytes) return ProtocolError::HeaderTruncated;
  for (std::size_t index = 0; index < kMagic.size(); ++index) {
    if (std::to_integer<std::uint8_t>(source[kMagicOffset + index]) !=
        kMagic[index]) {
      return ProtocolError::WrongMagic;
    }
  }

  header.header_bytes = read_u16(source, kHeaderBytesOffset);
  header.protocol_major = read_u16(source, kProtocolMajorOffset);
  header.protocol_minor = read_u16(source, kProtocolMinorOffset);
  header.message_type =
      static_cast<MessageType>(read_u16(source, kMessageTypeOffset));
  header.flags = read_u32(source, kFlagsOffset);
  header.payload_bytes = read_u32(source, kPayloadBytesOffset);
  header.reserved0 = read_u32(source, kReserved0Offset);
  header.message_sequence = read_u64(source, kMessageSequenceOffset);
  header.correlation_id = read_u64(source, kCorrelationIdOffset);
  for (std::size_t index = 0; index < header.connection_id.size(); ++index) {
    header.connection_id[index] =
        std::to_integer<std::uint8_t>(source[kConnectionIdOffset + index]);
  }
  header.reserved1 = read_u64(source, kReserved1Offset);
  return validate_header(header, false);
}

ProtocolError decode_message(std::span<const std::byte> source,
                             MessageView& message) noexcept {
  message = {};
  MessageHeader header;
  const ProtocolError decoded = decode_header(source, header);
  if (decoded != ProtocolError::None) return decoded;

  const std::size_t expected_bytes =
      static_cast<std::size_t>(kHeaderBytes) + header.payload_bytes;
  if (source.size() < expected_bytes) return ProtocolError::PayloadTruncated;
  if (source.size() > expected_bytes) return ProtocolError::TrailingBytes;

  message.header = header;
  message.payload = source.subspan(kHeaderBytes, header.payload_bytes);
  return ProtocolError::None;
}

} // namespace vividcam::producer_ipc
