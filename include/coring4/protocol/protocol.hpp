#pragma once

#include <cstdint>
#include <vector>
namespace Coring4::Control {
constexpr std::uint32_t MAGIC = 0x4D505258;
constexpr std::uint16_t VERSION = 1;

constexpr std::size_t HEADER_SIZE = 16;
constexpr std::size_t MAX_PAYLOAD_SIZE = 64 * 1024;

enum class Command : std::uint8_t {
  Ping,
  Status,
  Stats,
  Shutdown,
};

enum class MessageType : std::uint8_t {
  Request,
  Response,
};

struct Header {
  std::uint32_t magic;
  std::uint16_t version;
  MessageType type;
  Command command;
  std::uint32_t payload_size;
  std::uint32_t request_id;
};

struct Message {
  Header header;
  std::vector<std::uint8_t> payload;
};

enum class ProtocolError : std::uint8_t {
  InvalidMagic,
  UnsupportedVersion,
  InvalidMessageType,
  InvalidCommand,
  PayloadTooLarge,
  InvalidPayloadSize,
};
} // namespace Coring4::Control
