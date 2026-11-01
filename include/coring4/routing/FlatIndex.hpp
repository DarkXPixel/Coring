#pragma once

// Immutable open-addressing hash index: 64-bit key -> 32-bit value.
//
//  * built once (at table compile time), then only read -> no synchronisation;
//  * power-of-two capacity, load factor <= 0.5, linear probing: a lookup is
//    normally a single cache line (4 entries of 16 bytes);
//  * keys are the (already well mixed) chain hashes, so slot = key & mask;
//  * several entries may share a key (hash collision); find() hands every
//    candidate to the caller's `verify` predicate, so a collision can never
//    produce a wrong route.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Coring4 {

class FlatIndex {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  void build(const std::vector<std::pair<uint64_t, uint32_t>> &items) {
    std::size_t cap = 4;
    while (cap < items.size() * 2) {
      cap <<= 1;
    }
    slots_.assign(cap, Entry{});
    mask_ = cap - 1;
    for (const auto &[key, value] : items) {
      std::size_t i = key & mask_;
      while (slots_[i].value != kNone) {
        i = (i + 1) & mask_;
      }
      slots_[i] = Entry{key, value};
    }
    size_ = items.size();
  }

  // Returns the first value whose key matches and for which verify(value) is
  // true, or kNone.
  template <class Verify>
  [[nodiscard]] uint32_t find(uint64_t key, Verify &&verify) const noexcept {
    if (slots_.empty()) [[unlikely]] {
      return kNone;
    }
    std::size_t i = key & mask_;
    for (;;) {
      const Entry &e = slots_[i];
      if (e.value == kNone) {
        return kNone;
      }
      if (e.key == key && verify(e.value)) {
        return e.value;
      }
      i = (i + 1) & mask_;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t memory_bytes() const noexcept {
    return slots_.size() * sizeof(Entry);
  }

private:
  struct Entry {
    uint64_t key{0};
    uint32_t value{kNone};
    uint32_t reserved{0};
  };

  std::vector<Entry> slots_;
  std::size_t mask_{0};
  std::size_t size_{0};
};

} // namespace Coring4
