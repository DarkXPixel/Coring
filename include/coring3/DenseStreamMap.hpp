#pragma once

#include "coring3/SessionStorage.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
namespace Coring3 {
struct StreamBinding {
  uint32_t stream_id{0};
  SessionHandle client_stream_handle{};
  SessionHandle upstream_stream_handle{};
};

template <std::size_t MaxConcurrentStreams = 64> class DenseStreamMap {
  std::array<StreamBinding, MaxConcurrentStreams> streams_;
  std::size_t active_count{0};

public:
  bool insert(uint32_t stream_id, SessionHandle client_h,
              SessionHandle upstream_h) noexcept {
    for (auto &slot : streams_) {
      if (slot.stream_id == 0) {
        slot.stream_id = stream_id;
        slot.client_stream_handle = client_h;
        slot.upstream_stream_handle = upstream_h;
        ++active_count;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] StreamBinding *find(uint32_t stream_id) noexcept {
    for (auto &slot : streams_) {
      if (slot.stream_id == stream_id) {
        return &slot;
      }
    }
    return nullptr;
  }

  void erase(uint32_t stream_id) noexcept {
    for (auto &slot : streams_) {
      if (slot.stream_id == stream_id) {
        slot = StreamBinding{};
        --active_count;
        return;
      }
    }
  }
};
} // namespace Coring3