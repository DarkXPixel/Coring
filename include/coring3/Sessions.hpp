#pragma once

#include "EmptySession.hpp"
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
class Http1ClientSession {
public:
  int fd{-1};

  // BufferHandle rx_buffer;
  // BufferHandle tx_buffer;
  SessionHandle active_stream_handle{};

  bool keep_alive{true};
  bool reading_body{false};
};

class Http2ClientSession {
public:
  int fd{-1};
  // BufferHandle rx_buffer;
  // BufferHandle tx_buffer;

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