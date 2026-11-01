#include "coring5/io/TimerWheel.hpp"

#include <algorithm>

namespace Coring5 {

TimerWheel::TimerWheel(TimePoint start, Nanos tick, uint32_t slots)
    : start_(start), tick_(tick), slots_(slots) {}

void TimerWheel::schedule(TimerNode &n, TimePoint deadline) noexcept {
  n.cancel();
  uint64_t expiry = 0;
  if (deadline > start_) {
    // round up so a timer never fires early
    expiry =
        static_cast<uint64_t>((deadline - start_ + tick_ - Nanos{1}) / tick_);
  }
  expiry = std::max(expiry, cur_ + 1);
  n.expiry_tick_ = expiry;
  link_before(slots_[expiry % slots_.size()], n);
}

} // namespace Coring5
