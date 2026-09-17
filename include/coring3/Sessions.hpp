#pragma once

#include "EmptySession.hpp"
#include "coring3/BufferHandle.hpp"
#include "coring3/DenseStreamMap.hpp"
#include "coring3/SessionStorage.hpp"
#include <array>
#include <cstddef>
#include <variant>
namespace Coring3 {

class DetectionSession {
public:
  int fd{-1};
  std::array<std::byte, 24> peek_buf{};
  size_t bytes_read{0};
};
class alignas(64) Http1ClientSession {
public:
  SessionHandle active_stream_handle{};
  int32_t fd{-1};
  uint32_t flags{0};
  uint16_t pending_rx_offset{0};
  uint16_t partial_rx_bytes{0};

  BufferHandle partial_rx_accumulator;
};

class Http2ClientSession {
public:
  int32_t fd{-1};

  // HPackDecoder

  enum class FrameParserState : uint8_t {
    ReadHeader,
    ReadPayload,
    SkipPayload
  } parserState{FrameParserState::ReadHeader};

  uint32_t last_client_stream_id{0};
  uint32_t local_window_size{65535};
  uint32_t remote_window_size{65535};

  // DenseStreamMap<64> streams;
};

class Http3ClientSession {
public:
  int fd{-1};

  uint64_t connecition_id{0}; // CID

  // QpackDecoder qpack_decoder;

  DenseStreamMap<128> streams;
};

class Http1UpstreamSession {
public:
  int fd{-1};
};

using ClientConnectionVariant =
    std::variant<EmptySession, DetectionSession, Http1ClientSession,
                 Http2ClientSession, Http3ClientSession>;

using UpstreamConnectionVariant =
    std::variant<EmptySession, Http1UpstreamSession>;
} // namespace Coring3