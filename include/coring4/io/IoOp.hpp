#pragma once

#include "coring4/core/Types.hpp"
#include "coring4/storage/ObjectPool.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
namespace Coring4 {
enum class IoOpState : uint8_t {
  Free,
  Prepared,
  Submitted,
  CancelRequest,
  Completting,
};

struct IoOp {
  union {
    void *ptr;
    BufferHandle buffer_handle{};
  };
  std::size_t size{0};
  uint64_t offset{0};
  int fd{-1};
  PoolId pool_id{};
  // BufferHandle, PoolBufferHandle, PoolId buffer;
  OpCode op_code;
  IoOpState state{IoOpState::Free};

  // uint32_t flags{};
  bool multishot{false};
};

struct WorkerId {
  uint16_t id;
};

using IoOpPool = ObjectPool<IoOp>;
} // namespace Coring4