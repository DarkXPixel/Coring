#pragma once

// Fixed-capacity pool with generation-checked handles (invariant I4, I7).
//
//  * Capacity is fixed at construction: create() never allocates and returns
//    {invalid, nullptr} when full (caller sheds load).
//  * Slots hold polymorphic objects in-place: HandlePool<Connection, 256>
//    can create<EchoConnection>() as long as it fits in 256 bytes.
//  * `gen` is a plain 32-bit counter, 0 is reserved for "invalid". It is a
//    tripwire, not the primary safety mechanism: for a connection's own
//    in-flight operations, safety comes from "destroy only when inflight == 0".
//    A stale lookup returns nullptr and the caller bumps `stale_events`.
//  * Single-threaded by design (owned by one worker).

#include "coring5/base/Types.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Coring5 {

template <class T, std::size_t SlotBytes = sizeof(T)> class HandlePool {
public:
  explicit HandlePool(uint32_t capacity)
      : capacity_(capacity), slots_(std::make_unique<Slot[]>(capacity)) {
    assert(capacity > 0 && capacity <= kMaxPoolCapacity);
    // reserve(), NOT resize(): the legacy SessionStorage used resize() followed
    // by push_back() and ended up with `capacity` bogus zero entries at the
    // bottom of its free list (index 0 handed out twice).
    free_.reserve(capacity);
    for (uint32_t i = capacity; i > 0; --i) {
      free_.push_back(i - 1); // index 0 is handed out first
    }
  }

  HandlePool(const HandlePool &) = delete;
  HandlePool &operator=(const HandlePool &) = delete;

  ~HandlePool() {
    for (uint32_t i = 0; i < capacity_; ++i) {
      if (slots_[i].obj != nullptr) {
        slots_[i].obj->~T();
        slots_[i].obj = nullptr;
      }
    }
  }

  // Returns {invalid handle, nullptr} if the pool is full.
  template <class Derived = T, class... Args>
  [[nodiscard]] std::pair<Handle, Derived *> create(Args &&...args) {
    static_assert(std::is_base_of_v<T, Derived>);
    static_assert(sizeof(Derived) <= SlotBytes, "object does not fit the slot");
    static_assert(alignof(Derived) <= alignof(std::max_align_t));
    if (free_.empty()) [[unlikely]] {
      return {Handle{}, nullptr};
    }
    const uint32_t idx = free_.back();
    free_.pop_back();
    Slot &s = slots_[idx];
    auto *obj = ::new (static_cast<void *>(s.storage))
        Derived(std::forward<Args>(args)...);
    s.obj = obj;
    ++live_;
    return {Handle{idx, s.gen}, obj};
  }

  [[nodiscard]] T *get(Handle h) noexcept {
    if (h.index >= capacity_) [[unlikely]] {
      return nullptr;
    }
    Slot &s = slots_[h.index];
    return (s.obj != nullptr && s.gen == h.gen) ? s.obj : nullptr;
  }

  // Stale or invalid handles are ignored (returns false).
  bool destroy(Handle h) noexcept {
    if (get(h) == nullptr) {
      return false;
    }
    Slot &s = slots_[h.index];
    s.obj->~T();
    s.obj = nullptr;
    if (++s.gen == 0) { // skip the invalid value on wrap (every 2^32 reuses)
      s.gen = 1;
    }
    free_.push_back(h.index);
    --live_;
    return true;
  }

  // Safe against destroy() of the visited element from inside `f`.
  template <class F> void for_each(F &&f) {
    for (uint32_t i = 0; i < capacity_; ++i) {
      if (slots_[i].obj != nullptr) {
        f(Handle{i, slots_[i].gen}, *slots_[i].obj);
      }
    }
  }

  [[nodiscard]] uint32_t live() const noexcept { return live_; }
  [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] uint32_t available() const noexcept {
    return capacity_ - live_;
  }

  // Test hook: set a slot's generation (used to exercise wrap-around).
  void debug_set_generation(uint32_t index, uint32_t gen) noexcept {
    slots_[index].gen = gen;
  }

private:
  struct Slot {
    alignas(std::max_align_t) std::byte storage[SlotBytes];
    T *obj{nullptr};
    uint32_t gen{1};
  };

  uint32_t capacity_;
  uint32_t live_{0};
  std::unique_ptr<Slot[]> slots_;
  std::vector<uint32_t> free_;
};

} // namespace Coring5
