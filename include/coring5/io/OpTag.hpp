#pragma once

// 64-bit io_uring user_data layout (invariant I4):
//
//   63            32 31          12 11      6 5       0
//   +---------------+--------------+---------+--------+
//   |      gen      |    index     |  kind   |  aux   |
//   +---------------+--------------+---------+--------+
//        32 bits        20 bits       6 bits    6 bits
//
// Explicit shifts, not bit-fields: bit-field layout is implementation-defined.
//
// index/gen refer to a slot in a HandlePool (connections) or to a listener
// slot; kind says what the completed operation was.

#include "coring5/base/Types.hpp"
#include <cassert>
#include <cstdint>

namespace Coring5 {

enum class OpKind : uint8_t {
  Nop = 0,
  Accept, // multishot; index = listener slot
  Recv,   // single-shot or multishot, provided buffers
  Send,
  SendZc, // two CQEs: result (F_MORE) then F_NOTIF
  Connect,
  LinkTimeout, // linked to the previous SQE; has its own CQE
  Cancel,      // cancel SQEs have their own CQE and COUNT as in-flight (I2)
  Mailbox,     // multishot poll on the worker's eventfd
  Tick,        // timer-wheel tick (IORING_OP_TIMEOUT)
  Read,
  Splice,
  RecvMsg,
  Count_ // must stay <= 64
};

static_assert(static_cast<unsigned>(OpKind::Count_) <= 64);

// `aux` meaning for OpKind::Cancel: who issued it.
inline constexpr uint8_t kAuxConnection = 0;
inline constexpr uint8_t kAuxWorker =
    1; // listener removal etc.; not counted per-connection

struct OpTag {
  uint64_t raw{0};

  static constexpr OpTag make(OpKind kind, uint32_t index, uint32_t gen,
                              uint8_t aux = 0) noexcept {
    assert(index <= kMaxHandleIndex);
    assert(aux < 64);
    return OpTag{
        (static_cast<uint64_t>(gen) << 32) |
        (static_cast<uint64_t>(index & 0xFFFFFU) << 12) |
        (static_cast<uint64_t>(static_cast<uint8_t>(kind) & 0x3FU) << 6) |
        static_cast<uint64_t>(aux & 0x3FU)};
  }

  static constexpr OpTag make(OpKind kind, Handle h, uint8_t aux = 0) noexcept {
    return make(kind, h.index, h.gen, aux);
  }

  [[nodiscard]] constexpr uint32_t gen() const noexcept {
    return static_cast<uint32_t>(raw >> 32);
  }
  [[nodiscard]] constexpr uint32_t index() const noexcept {
    return static_cast<uint32_t>((raw >> 12) & 0xFFFFFU);
  }
  [[nodiscard]] constexpr OpKind kind() const noexcept {
    return static_cast<OpKind>((raw >> 6) & 0x3FU);
  }
  [[nodiscard]] constexpr uint8_t aux() const noexcept {
    return static_cast<uint8_t>(raw & 0x3FU);
  }
  [[nodiscard]] constexpr Handle handle() const noexcept {
    return Handle{index(), gen()};
  }
};

static_assert(sizeof(OpTag) == 8);

} // namespace Coring5
