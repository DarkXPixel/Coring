#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>
namespace Coring4 {

struct SessionHandle {
  uint32_t index{0};
  uint32_t generation{0};
  [[nodiscard]] bool is_valid() const noexcept { return generation != 0; }
  bool operator==(const SessionHandle &) const = default;
};

template <typename VariantType, std::size_t MaxCapacity = 10000>
class SessionStorage {
  struct Slot {
    VariantType data;
    uint32_t generation{1};
    bool is_active{false};
  };

  std::vector<Slot> slots_;
  std::vector<uint32_t> free_list_;

public:
  SessionStorage() {
    slots_.resize(MaxCapacity);
    free_list_.resize(MaxCapacity);
    for (uint32_t i = MaxCapacity; i > 0; --i) {
      free_list_.push_back(i - 1);
    }
  }
  template <typename ConcreteSession, typename... Args>
  std::pair<SessionHandle, VariantType *> emplace(Args &&...args) {
    if (free_list_.empty()) {
      return {SessionHandle{}, nullptr};
    }

    uint32_t idx = free_list_.back();
    free_list_.pop_back();
    Slot &slot = slots_[idx];
    slot.is_active = true;

    slot.data.template emplace<ConcreteSession>(std::forward<Args>(args)...);

    SessionHandle handle{.index = idx, .generation = slot.generation};
    return {handle, &slot.data};
  }

  VariantType *get(SessionHandle handle) noexcept {
    if (handle.index >= slots_.size()) {
      return nullptr;
    }

    Slot &slot = slots_[handle.index];
    if (!slot.is_active || slot.generation != handle.generation) {
      return nullptr;
    }

    return &slot.data;
  }
  void release(SessionHandle handle) noexcept {
    if (handle.index >= slots_.size()) {
      return;
    }

    Slot &slot = slots_[handle.index];
    if (!slot.is_active || slot.generation != handle.generation) {
      return;
    }

    slot.data.template emplace<std::monostate>();

    slot.is_active = false;
    ++slot.generation;
    if (slot.generation == 0) [[unlikely]] {
      slot.generation = 1;
    }

    free_list_.push_back(handle.index);
  }
};
} // namespace Coring4