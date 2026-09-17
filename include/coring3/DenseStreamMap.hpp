#pragma once

#include "coring3/SessionStorage.hpp"
#include "coring3/Stream.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
namespace Coring3 {
struct StreamBinding {
  SessionHandle client_stream_handle{};
  SessionHandle upstream_stream_handle{};
  uint32_t stream_id{UINT32_MAX};
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return stream_id != UINT32_MAX;
  }
};

template <std::size_t MaxConcurrentStreams = 64, bool IsHttp3 = false>
class DenseStreamMap {
  std::array<StreamBinding, MaxConcurrentStreams> slots_;
  std::size_t active_count_{0};

public:
  [[nodiscard]] static constexpr std::size_t
  id_to_index(uint32_t stream_id) noexcept {
    if constexpr (IsHttp3) {
      return static_cast<std::size_t>(stream_id >> 2);
    } else {
      return static_cast<std::size_t>((stream_id - 1) >> 1);
    }
  }

  bool insert(uint32_t stream_id, SessionHandle client_h,
              SessionHandle upstream_h) noexcept {
    const std::size_t idx = id_to_index(stream_id) % MaxConcurrentStreams;

    if (slots_[idx].is_valid()) {
      return false;
    }

    slots_[idx] = StreamBinding{.client_stream_handle = client_h,
                                .upstream_stream_handle = upstream_h,
                                .stream_id = stream_id};

    ++active_count_;
    return true;
  }

  [[nodiscard]] StreamBinding *find(uint32_t stream_id) noexcept {
    const std::size_t idx = id_to_index(stream_id) % MaxConcurrentStreams;
    auto &slot = slots_[idx];
    if (slot.stream_id == stream_id) {
      return &slot;
    }
    return nullptr;
  }

  void erase(uint32_t stream_id) noexcept {
    const std::size_t idx = id_to_index(stream_id) % MaxConcurrentStreams;
    auto &slot = slots_[idx];

    if (slot.stream_id == stream_id) {
      slot = StreamBinding{};
      --active_count_;
    }
  }

  void clear() noexcept {
    slots_.fill(StreamBinding{});
    active_count_ = 0;
  }
};
} // namespace Coring3