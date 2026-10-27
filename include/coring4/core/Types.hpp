#pragma once

#include <cstddef>
#include <cstdint>
namespace Coring4 {
enum class OpCode : uint8_t {
  Accept,
  Send,
  Recv,
  StopSignal,
  Close,
  Cancel,
  Connect,
  Read,
  Write,
  Manage,
  Poll,
};

struct BufferHandle {
  void *ptr{nullptr};
  std::size_t size{0};
};

struct PoolBufferHandle {
  void *ptr{nullptr};
  std::size_t size{0};
  uint8_t pool_id;
};

enum class PoolId : uint8_t { Empty = 0 };

// struct PoolId {
//   uint8_t id{};
// };

} // namespace Coring4