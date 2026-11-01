#pragma once

#include <unistd.h>
#include <utility>

namespace Coring5 {

// Move-only owner of a file descriptor.
class FdOwner {
public:
  FdOwner() = default;
  explicit FdOwner(int fd) noexcept : fd_(fd) {}
  FdOwner(const FdOwner &) = delete;
  FdOwner &operator=(const FdOwner &) = delete;
  FdOwner(FdOwner &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  FdOwner &operator=(FdOwner &&o) noexcept {
    if (this != &o) {
      reset(std::exchange(o.fd_, -1));
    }
    return *this;
  }
  ~FdOwner() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return valid(); }

  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_{-1};
};

} // namespace Coring5
