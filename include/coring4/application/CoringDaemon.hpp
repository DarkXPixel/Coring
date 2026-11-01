#pragma once

#include <atomic>
namespace Coring4::Control {
class CoringDaemon {
public:
  void start() { running_.store(true); }
  void stop() { running_.store(false); }

  bool is_running() const noexcept { return running_.load(); }

private:
  std::atomic_bool running_{false};
};
} // namespace Coring4::Control
