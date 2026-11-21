#pragma once

// 64-bit io_uring user_data layout (invariant I4):
//
//   63            32 31          12 11     7 6      3 2     0
//   +---------------+--------------+--------+--------+-------+
//   |      gen      |    index     |  kind  |  pool  |  aux  |
//   +---------------+--------------+--------+--------+-------+
//        32 bits        20 bits      5 bits   4 bits   3 bits
//
// Explicit shifts, not bit-fields: bit-field layout is implementation-defined.
//
// pool/index/gen address a slot in one of the worker's connection pools (or a
// listener slot, where pool is 0 and unused); kind says what the completed
// operation was.
//
// Budget: up to 16 pools and 32 operation kinds. (M4.0 moved one bit from
// `kind`, which had 6, to `pool`, which had 3; see docs/adr/0010.) The widths
// below are the single source of truth; the static_asserts tie them to the
// enums so that adding a pool or kind past the limit fails the build.

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
  Shutdown, // IORING_OP_SHUTDOWN (half-close of a relayed stream)
  Count_    // must stay <= 1 << kOpKindBits (see below)
};

// Field geometry. Fields are packed from the least significant bit upwards.
inline constexpr unsigned kOpAuxBits = 3;
inline constexpr unsigned kOpPoolBits = 4;
inline constexpr unsigned kOpKindBits = 5;
inline constexpr unsigned kOpIndexBits = 20;
inline constexpr unsigned kOpGenBits = 32;

inline constexpr unsigned kOpPoolShift = kOpAuxBits;
inline constexpr unsigned kOpKindShift = kOpPoolShift + kOpPoolBits;
inline constexpr unsigned kOpIndexShift = kOpKindShift + kOpKindBits;
inline constexpr unsigned kOpGenShift = kOpIndexShift + kOpIndexBits;

static_assert(kOpGenShift == 32 && kOpGenShift + kOpGenBits == 64,
              "OpTag fields must tile all 64 bits exactly");
static_assert(kOpIndexBits == 20 &&
                  (kMaxHandleIndex + 1U) == (1U << kOpIndexBits),
              "kMaxHandleIndex must match the index field width");
static_assert(static_cast<unsigned>(OpKind::Count_) <= (1U << kOpKindBits),
              "too many OpKinds for the kind field");
static_assert(kPoolCount <= (1U << kOpPoolBits),
              "too many pools for the pool field");

inline constexpr uint64_t kOpAuxMask = (uint64_t{1} << kOpAuxBits) - 1;
inline constexpr uint64_t kOpPoolMask = (uint64_t{1} << kOpPoolBits) - 1;
inline constexpr uint64_t kOpKindMask = (uint64_t{1} << kOpKindBits) - 1;
inline constexpr uint64_t kOpIndexMask = (uint64_t{1} << kOpIndexBits) - 1;

// `aux` meaning for OpKind::Cancel: who issued it (3 bits: 0..7).
inline constexpr uint8_t kAuxConnection = 0;
inline constexpr uint8_t kAuxWorker =
    1; // listener removal etc.; not counted per-connection
// A connection that owns TWO sockets (l4: client + upstream) cancels each fd
// separately; the second cancel carries this aux so the two CQEs can be told
// apart in traces. Both count as in-flight (I2).
inline constexpr uint8_t kAuxPeer = 2;
inline constexpr uint8_t kAuxMax = 7;

struct OpTag {
  uint64_t raw{0};

  static constexpr OpTag make(OpKind kind, uint32_t index, uint32_t gen,
                              uint8_t aux = 0,
                              PoolId pool = PoolId::Echo) noexcept {
    assert(index <= kMaxHandleIndex);
    assert(aux <= kAuxMax);
    assert(static_cast<uint64_t>(pool) <= kOpPoolMask);
    assert(static_cast<uint64_t>(kind) <= kOpKindMask);
    return OpTag{
        (static_cast<uint64_t>(gen) << kOpGenShift) |
        ((static_cast<uint64_t>(index) & kOpIndexMask) << kOpIndexShift) |
        ((static_cast<uint64_t>(kind) & kOpKindMask) << kOpKindShift) |
        ((static_cast<uint64_t>(pool) & kOpPoolMask) << kOpPoolShift) |
        (static_cast<uint64_t>(aux) & kOpAuxMask)};
  }

  static constexpr OpTag make(OpKind kind, Handle h, uint8_t aux = 0,
                              PoolId pool = PoolId::Echo) noexcept {
    return make(kind, h.index, h.gen, aux, pool);
  }

  static constexpr OpTag make(OpKind kind, ConnRef c,
                              uint8_t aux = 0) noexcept {
    return make(kind, c.handle.index, c.handle.gen, aux, c.pool);
  }

  [[nodiscard]] constexpr uint32_t gen() const noexcept {
    return static_cast<uint32_t>(raw >> kOpGenShift);
  }
  [[nodiscard]] constexpr uint32_t index() const noexcept {
    return static_cast<uint32_t>((raw >> kOpIndexShift) & kOpIndexMask);
  }
  [[nodiscard]] constexpr OpKind kind() const noexcept {
    return static_cast<OpKind>((raw >> kOpKindShift) & kOpKindMask);
  }
  [[nodiscard]] constexpr PoolId pool() const noexcept {
    return static_cast<PoolId>((raw >> kOpPoolShift) & kOpPoolMask);
  }
  [[nodiscard]] constexpr uint8_t aux() const noexcept {
    return static_cast<uint8_t>(raw & kOpAuxMask);
  }
  [[nodiscard]] constexpr Handle handle() const noexcept {
    return Handle{index(), gen()};
  }
  [[nodiscard]] constexpr ConnRef conn() const noexcept {
    return ConnRef{pool(), handle()};
  }
};

static_assert(sizeof(OpTag) == 8);

} // namespace Coring5
