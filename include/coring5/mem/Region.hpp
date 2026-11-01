#pragma once

// Anonymous mmap region (RAII). Used for the provided-buffer slab: one
// contiguous allocation so `buffer id -> address` is pure arithmetic.

#include "coring5/base/Result.hpp"
#include <cstddef>
#include <utility>

namespace Coring5 {

struct RegionOptions {
  bool populate{
      true}; // pre-fault on the creating (worker) thread: first-touch NUMA
  bool hugepage{false}; // madvise(MADV_HUGEPAGE); best effort
};

class Region {
public:
  Region() = default;
  Region(const Region &) = delete;
  Region &operator=(const Region &) = delete;
  Region(Region &&o) noexcept
      : data_(std::exchange(o.data_, nullptr)),
        size_(std::exchange(o.size_, 0)) {}
  Region &operator=(Region &&o) noexcept {
    if (this != &o) {
      reset();
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }
  ~Region() { reset(); }

  [[nodiscard]] static Result<Region> map(std::size_t bytes,
                                          RegionOptions opts = {});

  [[nodiscard]] std::byte *data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  void reset() noexcept;

private:
  std::byte *data_{nullptr};
  std::size_t size_{0};
};

} // namespace Coring5
