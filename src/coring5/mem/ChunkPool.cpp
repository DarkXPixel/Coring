#include "coring5/mem/ChunkPool.hpp"

namespace Coring5 {

Result<ChunkPool> ChunkPool::create(uint32_t chunk_size, uint32_t count) {
  if (chunk_size == 0 || count == 0 || count == kNil) {
    return unexpected_error(Errc::InvalidArgument);
  }
  auto slab = Region::map(
      static_cast<std::size_t>(count) * chunk_size,
      RegionOptions{.populate = false, .hugepage = false, .noreserve = true});
  if (!slab) {
    return std::unexpected(slab.error());
  }
  ChunkPool p;
  p.slab_ = std::move(*slab);
  p.next_ = std::make_unique<uint32_t[]>(count);
  for (uint32_t i = 0; i + 1 < count; ++i) {
    p.next_[i] = i + 1;
  }
  p.next_[count - 1] = kNil;
  p.chunk_size_ = chunk_size;
  p.count_ = count;
  p.free_ = count;
  p.free_head_ = 0;
  return p;
}

} // namespace Coring5
