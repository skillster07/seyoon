#include "vividcam/producer_ipc_protocol.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using vividcam::producer_ipc::MessageHeader;
using vividcam::producer_ipc::MessageType;
using vividcam::producer_ipc::MessageView;
using vividcam::producer_ipc::ProtocolError;

std::vector<std::byte> bytes_from(
    std::span<const std::uint8_t> values) {
  std::vector<std::byte> bytes;
  bytes.reserve(values.size());
  for (const std::uint8_t value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

void write_u16(std::vector<std::byte>& bytes, std::size_t offset,
               std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset,
               std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset,
               std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void expect_decode_error(const std::vector<std::byte>& bytes,
                         ProtocolError expected) {
  MessageView decoded;
  const ProtocolError actual = vividcam::producer_ipc::decode_message(bytes, decoded);
  assert(actual == expected);
  assert(!vividcam::producer_ipc::protocol_error_message(actual).empty());
  assert(decoded.payload.empty());
}

} // namespace

int main() {
  using namespace vividcam::producer_ipc;

  static_assert(kHeaderBytes == 64);
  static_assert(kMaximumPayloadBytes == 65'536);
  static_assert(static_cast<std::uint16_t>(MessageType::SourceHello) == 0x0001);
  static_assert(static_cast<std::uint16_t>(MessageType::ProducerHello) == 0x0002);
  static_assert(static_cast<std::uint16_t>(MessageType::OpenStream) == 0x0010);
  static_assert(static_cast<std::uint16_t>(MessageType::StreamReady) == 0x0011);
  static_assert(static_cast<std::uint16_t>(MessageType::StopStream) == 0x0012);
  static_assert(static_cast<std::uint16_t>(MessageType::StreamStopped) == 0x0013);
  static_assert(static_cast<std::uint16_t>(MessageType::ProducerState) == 0x0020);
  static_assert(static_cast<std::uint16_t>(MessageType::Heartbeat) == 0x0021);
  static_assert(static_cast<std::uint16_t>(MessageType::HeartbeatAck) == 0x0022);
  static_assert(static_cast<std::uint16_t>(MessageType::TransportOffer) == 0x0030);
  static_assert(static_cast<std::uint16_t>(MessageType::TransportAccepted) ==
                0x0031);
  static_assert(static_cast<std::uint16_t>(MessageType::Error) == 0x00f0);
  static_assert(static_cast<std::uint16_t>(MessageType::Goodbye) == 0x00f1);

  const std::array all_message_types = {
      MessageType::SourceHello,       MessageType::ProducerHello,
      MessageType::OpenStream,        MessageType::StreamReady,
      MessageType::StopStream,        MessageType::StreamStopped,
      MessageType::ProducerState,     MessageType::Heartbeat,
      MessageType::HeartbeatAck,      MessageType::TransportOffer,
      MessageType::TransportAccepted, MessageType::Error,
      MessageType::Goodbye};
  for (const MessageType type : all_message_types) {
    assert(is_known_message_type(type));
  }
  assert(!is_known_message_type(static_cast<MessageType>(0)));
  assert(!is_known_message_type(static_cast<MessageType>(0xffffU)));

  MessageHeader header;
  header.message_type = MessageType::Heartbeat;
  header.flags = 0x01020304U;
  header.payload_bytes = 3;
  header.message_sequence = 0x0102030405060708ULL;
  header.correlation_id = 0x1112131415161718ULL;
  for (std::size_t index = 0; index < header.connection_id.size(); ++index) {
    header.connection_id[index] = static_cast<std::uint8_t>(index);
  }
  const std::array<std::uint8_t, 3> payload_values = {0xa1U, 0xb2U, 0xc3U};
  const std::vector<std::byte> payload = bytes_from(payload_values);

  std::vector<std::byte> encoded;
  assert(encode_message(header, payload, encoded) == ProtocolError::None);

  // Golden VCIP 1.0 message. Multi-byte values deliberately have distinct
  // bytes so an accidental host-endian or struct-memcpy codec cannot pass.
  constexpr std::array<std::uint8_t, 67> golden_values = {
      0x56, 0x43, 0x49, 0x50, 0x40, 0x00, 0x01, 0x00,
      0x00, 0x00, 0x21, 0x00, 0x04, 0x03, 0x02, 0x01,
      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xa1, 0xb2, 0xc3};
  assert(encoded == bytes_from(golden_values));

  MessageHeader decoded_header;
  assert(decode_header(std::span<const std::byte>{encoded}.first(kHeaderBytes),
                       decoded_header) == ProtocolError::None);
  assert(decoded_header.header_bytes == kHeaderBytes);
  assert(decoded_header.protocol_major == kProtocolMajor);
  assert(decoded_header.protocol_minor == kProtocolMinor);
  assert(decoded_header.message_type == MessageType::Heartbeat);
  assert(decoded_header.flags == header.flags);
  assert(decoded_header.payload_bytes == payload.size());
  assert(decoded_header.message_sequence == header.message_sequence);
  assert(decoded_header.correlation_id == header.correlation_id);
  assert(decoded_header.connection_id == header.connection_id);

  MessageView decoded;
  assert(decode_message(encoded, decoded) == ProtocolError::None);
  assert(decoded.header.message_type == header.message_type);
  assert(decoded.payload.size() == payload.size());
  for (std::size_t index = 0; index < payload.size(); ++index) {
    assert(decoded.payload[index] == payload[index]);
  }

  auto malformed = encoded;
  malformed.resize(kHeaderBytes - 1U);
  expect_decode_error(malformed, ProtocolError::HeaderTruncated);

  malformed = encoded;
  malformed[0] = std::byte{'X'};
  expect_decode_error(malformed, ProtocolError::WrongMagic);

  malformed = encoded;
  write_u16(malformed, 4, kHeaderBytes - 1U);
  expect_decode_error(malformed, ProtocolError::WrongHeaderSize);

  malformed = encoded;
  write_u16(malformed, 6, kProtocolMajor + 1U);
  expect_decode_error(malformed, ProtocolError::UnsupportedMajorVersion);

  // Higher minor versions remain decodable under the same compatible major.
  malformed = encoded;
  write_u16(malformed, 8, kProtocolMinor + 1U);
  assert(decode_message(malformed, decoded) == ProtocolError::None);
  assert(decoded.header.protocol_minor == kProtocolMinor + 1U);

  malformed = encoded;
  write_u16(malformed, 10, 0x7fffU);
  expect_decode_error(malformed, ProtocolError::UnknownMessageType);

  malformed = encoded;
  write_u32(malformed, 16, kMaximumPayloadBytes + 1U);
  expect_decode_error(malformed, ProtocolError::PayloadTooLarge);

  malformed = encoded;
  write_u32(malformed, 20, 1U);
  expect_decode_error(malformed, ProtocolError::NonzeroReserved0);

  malformed = encoded;
  write_u64(malformed, 24, 0U);
  expect_decode_error(malformed, ProtocolError::ZeroMessageSequence);

  malformed = encoded;
  write_u64(malformed, 56, 1U);
  expect_decode_error(malformed, ProtocolError::NonzeroReserved1);

  malformed = encoded;
  malformed.pop_back();
  expect_decode_error(malformed, ProtocolError::PayloadTruncated);

  malformed = encoded;
  malformed.push_back(std::byte{0});
  expect_decode_error(malformed, ProtocolError::TrailingBytes);

  MessageHeader invalid = header;
  invalid.payload_bytes = 2;
  assert(encode_message(invalid, payload, encoded) ==
         ProtocolError::PayloadSizeMismatch);
  invalid = header;
  invalid.message_sequence = 0;
  assert(encode_message(invalid, payload, encoded) ==
         ProtocolError::ZeroMessageSequence);
  invalid = header;
  invalid.protocol_minor = kProtocolMinor + 1U;
  assert(encode_message(invalid, payload, encoded) ==
         ProtocolError::UnsupportedMinorVersion);

  std::array<std::byte, kHeaderBytes - 1U> short_destination{};
  assert(encode_header(header, short_destination) ==
         ProtocolError::OutputBufferTooSmall);

  const std::vector<std::byte> maximum_payload(kMaximumPayloadBytes,
                                               std::byte{0x5a});
  MessageHeader maximum_header;
  maximum_header.message_type = MessageType::ProducerState;
  maximum_header.payload_bytes = kMaximumPayloadBytes;
  maximum_header.message_sequence = 1;
  assert(encode_message(maximum_header, maximum_payload, encoded) ==
         ProtocolError::None);
  assert(decode_message(encoded, decoded) == ProtocolError::None);
  assert(decoded.payload.size() == kMaximumPayloadBytes);

  const std::vector<std::byte> oversized_payload(
      static_cast<std::size_t>(kMaximumPayloadBytes) + 1U, std::byte{0});
  maximum_header.payload_bytes = kMaximumPayloadBytes;
  assert(encode_message(maximum_header, oversized_payload, encoded) ==
         ProtocolError::PayloadTooLarge);

  assert(protocol_error_message(ProtocolError::WrongMagic) ==
         std::string_view{"message magic is not VCIP"});

  std::cout << "VIVIDCAM producer IPC protocol tests passed\n";
  return 0;
}
