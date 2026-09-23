#include "coring4/Engine.hpp"
#include "coring4/ManageService.hpp"

namespace Coring4 {

void Engine::process_manage(io_uring_cqe *cqe) {
  auto requests = channel_->pop_all();
  for (auto &req : requests) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, Manage::AddListenerRequest>) {
            [[maybe_unused]] auto _ =
                this->add_listener(arg.port, arg.protocol, arg.version);
          }
        },
        req.request);
  }
  recv_manage_singnal();
}
} // namespace Coring4