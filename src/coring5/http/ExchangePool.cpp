#include "coring5/http/ExchangePool.hpp"

namespace Coring5 {

ExchangePool::ExchangePool(uint32_t capacity, ChunkPool &chunks,
                           StreamLimits limits)
    : chunks_(&chunks), limits_(limits), pool_(capacity), ready_(capacity) {}

Handle ExchangePool::open(SideRef ingress, RequestMeta meta) noexcept {
  auto [h, ex] = pool_.create(*chunks_, limits_, ingress, meta);
  if (ex == nullptr) {
    ++stats_.shed;
    return Handle{};
  }
  ex->bind(h, ready_);
  ++stats_.opened;
  return h;
}

} // namespace Coring5
