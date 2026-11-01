#include "coring5/runtime/Mailbox.hpp"

#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace Coring5 {

Mailbox::Mailbox() : efd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
Mailbox::~Mailbox() = default;

void Mailbox::post(Message m) {
  {
    std::lock_guard lock(mtx_);
    queue_.push_back(std::move(m));
  }
  const uint64_t one = 1;
  (void)!::write(efd_.get(), &one, sizeof(one));
}

void Mailbox::drain(std::vector<Message> &out) {
  uint64_t counter = 0;
  (void)!::read(efd_.get(), &counter, sizeof(counter)); // EAGAIN is fine
  std::lock_guard lock(mtx_);
  for (auto &m : queue_) {
    out.push_back(std::move(m));
  }
  queue_.clear();
}

} // namespace Coring5
