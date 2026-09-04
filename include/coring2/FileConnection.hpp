#pragma once
#include <cstdint>
#include <memory>
#include <unistd.h>
#include <variant>
namespace Coring2 {
template <typename LoopTYpe> class alignas(64) FileConnection {
  enum class State : uint8_t { Active = 0, Closing = 1 };

public:
  int fd_{-1};
  uint8_t pending_io_count_{0};
  State state_{State::Active};

  // std::variant<std::unique_ptr<typename Tp>>

  FileConnection(int fd) : fd_(fd) {}

  ~FileConnection() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
};
} // namespace Coring2