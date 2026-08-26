#include "vividcam/producer_ipc_protocol.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using vividcam::producer_ipc::MessageHeader;
using vividcam::producer_ipc::MessageType;
using vividcam::producer_ipc::MessageView;
using vividcam::producer_ipc::NegotiationPayloadError;
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

vividcam::producer_ipc::OpenStreamPayload valid_nv12_open_stream() {
  using namespace vividcam::producer_ipc;
  OpenStreamPayload payload;
  payload.width = 1920;
  payload.height = 1080;
  payload.frame_rate_numerator = 60;
  payload.frame_rate_denominator = 1;
  payload.pixel_format = FramePixelFormat::Nv12;
  payload.plane0_stride_bytes = 1920;
  payload.plane1_stride_bytes = 1920;
  payload.frame_bytes = 3'110'400;
  return payload;
}

vividcam::producer_ipc::TransportOfferPayload valid_transport_offer(
    std::uint32_t frame_capacity_bytes) {
  using namespace vividcam::producer_ipc;
  TransportOfferPayload payload;
  payload.frame_capacity_bytes = frame_capacity_bytes;
  payload.mapping_capacity_bytes =
      cpu_frame_mapping_capacity(frame_capacity_bytes);
  return payload;
}

vividcam::producer_ipc::TransportDescriptorPayload valid_transport_descriptor(
    std::uint32_t frame_capacity_bytes) {
  using namespace vividcam::producer_ipc;
  TransportDescriptorPayload payload;
  payload.frame_capacity_bytes = frame_capacity_bytes;
  payload.mapping_capacity_bytes =
      cpu_frame_mapping_capacity(frame_capacity_bytes);
  return payload;
}

void expect_open_stream_decode_error(std::vector<std::byte> bytes,
                                     NegotiationPayloadError expected) {
  vividcam::producer_ipc::OpenStreamPayload decoded;
  const auto actual =
      vividcam::producer_ipc::decode_open_stream_payload(bytes, decoded);
  assert(actual == expected);
  assert(!vividcam::producer_ipc::negotiation_payload_error_message(actual)
              .empty());
}

void expect_transport_offer_decode_error(
    std::vector<std::byte> bytes, NegotiationPayloadError expected) {
  vividcam::producer_ipc::TransportOfferPayload decoded;
  const auto actual =
      vividcam::producer_ipc::decode_transport_offer_payload(bytes, decoded);
  assert(actual == expected);
  assert(!vividcam::producer_ipc::negotiation_payload_error_message(actual)
              .empty());
}

void expect_transport_descriptor_decode_error(
    std::vector<std::byte> bytes, NegotiationPayloadError expected) {
  vividcam::producer_ipc::TransportDescriptorPayload decoded;
  const auto actual =
      vividcam::producer_ipc::decode_transport_descriptor_payload(bytes,
                                                                  decoded);
  assert(actual == expected);
  assert(!vividcam::producer_ipc::negotiation_payload_error_message(actual)
              .empty());
}

void test_open_stream_payload_codec() {
  using namespace vividcam::producer_ipc;
  static_assert(kOpenStreamPayloadBytes == 48);

  const OpenStreamPayload nv12 = valid_nv12_open_stream();
  std::vector<std::byte> encoded;
  assert(encode_open_stream_payload(nv12, encoded) ==
         NegotiationPayloadError::None);

  // Golden fixed OpenStream payload. Frame bytes are metadata only; the
  // 3,110,400-byte NV12 frame is never embedded in VCIP.
  constexpr std::array<std::uint8_t, kOpenStreamPayloadBytes> golden_values = {
      0x01, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00,
      0x3c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00,
      0x80, 0x07, 0x00, 0x00, 0x00, 0x76, 0x2f, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  assert(encoded == bytes_from(golden_values));

  OpenStreamPayload decoded;
  assert(decode_open_stream_payload(encoded, decoded) ==
         NegotiationPayloadError::None);
  assert(decoded.schema_version == kNegotiationPayloadSchemaVersion);
  assert(decoded.payload_bytes == kOpenStreamPayloadBytes);
  assert(decoded.stream_id == 0);
  assert(decoded.width == 1920 && decoded.height == 1080);
  assert(decoded.frame_rate_numerator == 60 &&
         decoded.frame_rate_denominator == 1);
  assert(decoded.pixel_format == FramePixelFormat::Nv12);
  assert(decoded.plane0_stride_bytes == 1920 &&
         decoded.plane1_stride_bytes == 1920);
  assert(decoded.frame_bytes == 3'110'400);
  assert(decoded.flags == 0 && decoded.reserved == 0);

  OpenStreamPayload bgra = nv12;
  bgra.pixel_format = FramePixelFormat::Bgra;
  bgra.plane0_stride_bytes = 7680;
  bgra.plane1_stride_bytes = 0;
  bgra.frame_bytes = 8'294'400;
  assert(encode_open_stream_payload(bgra, encoded) ==
         NegotiationPayloadError::None);
  assert(decode_open_stream_payload(encoded, decoded) ==
         NegotiationPayloadError::None);
  assert(decoded.pixel_format == FramePixelFormat::Bgra);
  assert(decoded.frame_bytes == 8'294'400);

  assert(encode_open_stream_payload(nv12, encoded) ==
         NegotiationPayloadError::None);
  auto malformed = encoded;
  malformed.pop_back();
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  malformed.push_back(std::byte{0});
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  write_u16(malformed, 2, kOpenStreamPayloadBytes - 1U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  write_u16(malformed, 0, kNegotiationPayloadSchemaVersion + 1U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::UnsupportedSchemaVersion);
  malformed = encoded;
  write_u32(malformed, 4, 1U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidStreamId);
  malformed = encoded;
  write_u32(malformed, 24, 0xffffU);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::UnknownPixelFormat);
  malformed = encoded;
  write_u32(malformed, 8, 0U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidDimensions);
  malformed = encoded;
  write_u32(malformed, 8, 1919U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidDimensions);
  malformed = encoded;
  write_u32(malformed, 16, 0U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidFrameRate);
  malformed = encoded;
  write_u32(malformed, 20, 0U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidFrameRate);
  malformed = encoded;
  write_u32(malformed, 28, 1919U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidStride);
  malformed = encoded;
  write_u32(malformed, 32, 0U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidStride);
  malformed = encoded;
  write_u32(malformed, 36, 3'110'399U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::InvalidFrameBytes);
  malformed = encoded;
  write_u32(malformed, 40, 1U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::NonzeroFlags);
  malformed = encoded;
  write_u32(malformed, 44, 1U);
  expect_open_stream_decode_error(
      malformed, NegotiationPayloadError::NonzeroReserved);

  OpenStreamPayload oversized = nv12;
  oversized.width = kMaximumFrameDimension;
  oversized.height = kMaximumFrameDimension;
  oversized.plane0_stride_bytes = kMaximumFrameDimension;
  oversized.plane1_stride_bytes = kMaximumFrameDimension;
  oversized.frame_bytes = 96U * 1024U * 1024U;
  encoded.assign(1, std::byte{0x5a});
  assert(encode_open_stream_payload(oversized, encoded) ==
         NegotiationPayloadError::InvalidFrameBytes);
  assert(encoded.empty());

  OpenStreamPayload dimension_too_large = bgra;
  dimension_too_large.width = kMaximumFrameDimension + 1U;
  dimension_too_large.height = 2;
  dimension_too_large.plane0_stride_bytes =
      dimension_too_large.width * 4U;
  dimension_too_large.frame_bytes =
      dimension_too_large.plane0_stride_bytes * dimension_too_large.height;
  assert(encode_open_stream_payload(dimension_too_large, encoded) ==
         NegotiationPayloadError::InvalidDimensions);

  OpenStreamPayload overflow = bgra;
  overflow.width = std::numeric_limits<std::uint32_t>::max();
  overflow.height = std::numeric_limits<std::uint32_t>::max();
  assert(encode_open_stream_payload(overflow, encoded) ==
         NegotiationPayloadError::ArithmeticOverflow);

  OpenStreamPayload invalid_bgra_stride = bgra;
  invalid_bgra_stride.plane0_stride_bytes -= 4;
  assert(encode_open_stream_payload(invalid_bgra_stride, encoded) ==
         NegotiationPayloadError::InvalidStride);
  invalid_bgra_stride = bgra;
  invalid_bgra_stride.plane1_stride_bytes = 1920;
  assert(encode_open_stream_payload(invalid_bgra_stride, encoded) ==
         NegotiationPayloadError::InvalidStride);
}

void test_transport_payload_codecs() {
  using namespace vividcam::producer_ipc;
  static_assert(kTransportOfferPayloadBytes == 40);
  static_assert(kTransportDescriptorPayloadBytes == 40);
  static_assert(kCpuFrameMailboxSlotCount == 2);

  assert(cpu_frame_mapping_capacity(0) == 0);
  assert(cpu_frame_mapping_capacity(kMaximumCpuFrameBytes + 1U) == 0);
  assert(cpu_frame_mapping_capacity(3'110'400) == 6'230'016);
  assert(cpu_frame_mapping_capacity(8'294'400) == 16'601'088);

  const TransportOfferPayload offer = valid_transport_offer(3'110'400);
  std::vector<std::byte> encoded;
  assert(encode_transport_offer_payload(offer, encoded) ==
         NegotiationPayloadError::None);
  constexpr std::array<std::uint8_t, kTransportOfferPayloadBytes>
      golden_values = {
          0x01, 0x00, 0x28, 0x00, 0x01, 0x00, 0x00, 0x00,
          0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
          0x00, 0x10, 0x00, 0x00, 0x00, 0x76, 0x2f, 0x00,
          0x00, 0x10, 0x5f, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  assert(encoded == bytes_from(golden_values));

  TransportOfferPayload decoded_offer;
  assert(decode_transport_offer_payload(encoded, decoded_offer) ==
         NegotiationPayloadError::None);
  assert(decoded_offer.transport_kind ==
         FrameTransportKind::CpuSharedMemory);
  assert(decoded_offer.layout_major == kCpuFrameMailboxLayoutMajor &&
         decoded_offer.layout_minor == kCpuFrameMailboxLayoutMinor);
  assert(decoded_offer.slot_count == kCpuFrameMailboxSlotCount);
  assert(decoded_offer.mapping_header_bytes == kCpuFrameMailboxHeaderBytes);
  assert(decoded_offer.frame_capacity_bytes == 3'110'400);
  assert(decoded_offer.mapping_capacity_bytes == 6'230'016);
  assert(validate_transport_offer_for_open_stream(valid_nv12_open_stream(),
                                                  offer) ==
         NegotiationPayloadError::None);

  const TransportDescriptorPayload descriptor =
      valid_transport_descriptor(3'110'400);
  std::vector<std::byte> encoded_descriptor;
  assert(encode_transport_descriptor_payload(descriptor, encoded_descriptor) ==
         NegotiationPayloadError::None);
  assert(encoded_descriptor == encoded);
  TransportDescriptorPayload decoded_descriptor;
  assert(decode_transport_descriptor_payload(encoded_descriptor,
                                             decoded_descriptor) ==
         NegotiationPayloadError::None);
  assert(decoded_descriptor.mapping_capacity_bytes == 6'230'016);
  assert(validate_transport_descriptor_for_offer(offer, descriptor) ==
         NegotiationPayloadError::None);

  const TransportOfferPayload wrong_stream_capacity =
      valid_transport_offer(1);
  assert(validate_transport_offer_for_open_stream(valid_nv12_open_stream(),
                                                  wrong_stream_capacity) ==
         NegotiationPayloadError::ContractMismatch);
  OpenStreamPayload same_bytes_wrong_geometry = valid_nv12_open_stream();
  same_bytes_wrong_geometry.width = 1280;
  same_bytes_wrong_geometry.height = 1620;
  same_bytes_wrong_geometry.plane0_stride_bytes = 1280;
  same_bytes_wrong_geometry.plane1_stride_bytes = 1280;
  assert(same_bytes_wrong_geometry.frame_bytes == offer.frame_capacity_bytes);
  assert(validate_transport_offer_for_open_stream(same_bytes_wrong_geometry,
                                                  offer) ==
         NegotiationPayloadError::ContractMismatch);
  OpenStreamPayload wrong_frame_rate = valid_nv12_open_stream();
  wrong_frame_rate.frame_rate_numerator = 30;
  assert(validate_transport_offer_for_open_stream(wrong_frame_rate, offer) ==
         NegotiationPayloadError::ContractMismatch);
  OpenStreamPayload same_bytes_wrong_format = valid_nv12_open_stream();
  same_bytes_wrong_format.width = 960;
  same_bytes_wrong_format.height = 810;
  same_bytes_wrong_format.pixel_format = FramePixelFormat::Bgra;
  same_bytes_wrong_format.plane0_stride_bytes = 3840;
  same_bytes_wrong_format.plane1_stride_bytes = 0;
  assert(same_bytes_wrong_format.frame_bytes == offer.frame_capacity_bytes);
  assert(validate_transport_offer_for_open_stream(same_bytes_wrong_format,
                                                  offer) ==
         NegotiationPayloadError::ContractMismatch);
  const TransportDescriptorPayload changed_descriptor =
      valid_transport_descriptor(8'294'400);
  assert(validate_transport_descriptor_for_offer(offer,
                                                 changed_descriptor) ==
         NegotiationPayloadError::ContractMismatch);
  OpenStreamPayload invalid_stream = valid_nv12_open_stream();
  invalid_stream.frame_bytes -= 1;
  assert(validate_transport_offer_for_open_stream(invalid_stream, offer) ==
         NegotiationPayloadError::InvalidFrameBytes);
  TransportDescriptorPayload invalid_echo = descriptor;
  invalid_echo.mapping_capacity_bytes -= 1;
  assert(validate_transport_descriptor_for_offer(offer, invalid_echo) ==
         NegotiationPayloadError::InvalidCapacity);

  // Both response message IDs use the same strict transport descriptor.
  for (const MessageType type : {MessageType::TransportAccepted,
                                 MessageType::StreamReady}) {
    MessageHeader header;
    header.message_type = type;
    header.payload_bytes = kTransportDescriptorPayloadBytes;
    header.message_sequence = 3;
    header.correlation_id = 2;
    std::vector<std::byte> message;
    assert(encode_message(header, encoded_descriptor, message) ==
           ProtocolError::None);
    MessageView view;
    assert(decode_message(message, view) == ProtocolError::None);
    assert(decode_transport_descriptor_payload(view.payload,
                                               decoded_descriptor) ==
           NegotiationPayloadError::None);
  }

  auto malformed = encoded;
  malformed.pop_back();
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  malformed.push_back(std::byte{0});
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  write_u16(malformed, 2, kTransportOfferPayloadBytes - 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);
  malformed = encoded;
  write_u16(malformed, 0, kNegotiationPayloadSchemaVersion + 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::UnsupportedSchemaVersion);
  malformed = encoded;
  write_u32(malformed, 4, 99U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::UnknownTransportKind);
  malformed = encoded;
  write_u16(malformed, 8, kCpuFrameMailboxLayoutMajor + 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::UnsupportedTransportLayout);
  malformed = encoded;
  write_u16(malformed, 10, kCpuFrameMailboxLayoutMinor + 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::UnsupportedTransportLayout);
  malformed = encoded;
  write_u32(malformed, 12, 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidSlotCount);
  malformed = encoded;
  write_u32(malformed, 16, kCpuFrameMailboxHeaderBytes - 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidCapacity);
  malformed = encoded;
  write_u32(malformed, 20, 0U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidCapacity);
  malformed = encoded;
  write_u32(malformed, 20, kMaximumCpuFrameBytes + 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidCapacity);
  malformed = encoded;
  write_u64(malformed, 24, offer.mapping_capacity_bytes - 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidCapacity);
  malformed = encoded;
  write_u64(malformed, 24, std::numeric_limits<std::uint64_t>::max());
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::InvalidCapacity);
  malformed = encoded;
  write_u32(malformed, 32, 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::NonzeroFlags);
  malformed = encoded;
  write_u32(malformed, 36, 1U);
  expect_transport_offer_decode_error(
      malformed, NegotiationPayloadError::NonzeroReserved);

  malformed = encoded_descriptor;
  write_u32(malformed, 12, 3U);
  expect_transport_descriptor_decode_error(
      malformed, NegotiationPayloadError::InvalidSlotCount);
  malformed = encoded_descriptor;
  malformed.pop_back();
  expect_transport_descriptor_decode_error(
      malformed, NegotiationPayloadError::WrongPayloadSize);

  TransportOfferPayload invalid_offer = offer;
  invalid_offer.mapping_capacity_bytes -= 1;
  encoded.assign(1, std::byte{0x5a});
  assert(encode_transport_offer_payload(invalid_offer, encoded) ==
         NegotiationPayloadError::InvalidCapacity);
  assert(encoded.empty());
  TransportDescriptorPayload invalid_descriptor = descriptor;
  invalid_descriptor.reserved = 1;
  assert(encode_transport_descriptor_payload(invalid_descriptor, encoded) ==
         NegotiationPayloadError::NonzeroReserved);
}

} // namespace

int main() {
  using namespace vividcam::producer_ipc;

  static_assert(kHeaderBytes == 64);
  static_assert(kMaximumPayloadBytes == 65'536);
  static_assert(kOpenStreamPayloadBytes < kMaximumPayloadBytes);
  static_assert(kTransportOfferPayloadBytes < kMaximumPayloadBytes);
  static_assert(kTransportDescriptorPayloadBytes < kMaximumPayloadBytes);
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

  test_open_stream_payload_codec();
  test_transport_payload_codecs();

  const std::array all_negotiation_errors = {
      NegotiationPayloadError::None,
      NegotiationPayloadError::WrongPayloadSize,
      NegotiationPayloadError::UnsupportedSchemaVersion,
      NegotiationPayloadError::InvalidStreamId,
      NegotiationPayloadError::UnknownPixelFormat,
      NegotiationPayloadError::InvalidDimensions,
      NegotiationPayloadError::InvalidFrameRate,
      NegotiationPayloadError::InvalidStride,
      NegotiationPayloadError::InvalidFrameBytes,
      NegotiationPayloadError::UnknownTransportKind,
      NegotiationPayloadError::UnsupportedTransportLayout,
      NegotiationPayloadError::InvalidSlotCount,
      NegotiationPayloadError::InvalidCapacity,
      NegotiationPayloadError::ArithmeticOverflow,
      NegotiationPayloadError::ContractMismatch,
      NegotiationPayloadError::NonzeroFlags,
      NegotiationPayloadError::NonzeroReserved};
  for (const NegotiationPayloadError error : all_negotiation_errors) {
    assert(!negotiation_payload_error_message(error).empty());
  }

  std::cout << "VIVIDCAM producer IPC protocol tests passed\n";
  return 0;
}
