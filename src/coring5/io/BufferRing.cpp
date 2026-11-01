#include "coring5/io/BufferRing.hpp"

#include <bit>

namespace Coring5 {

Result<BufferRing> BufferRing::create(Ring &ring, uint16_t bgid, uint32_t count,
                                      uint32_t buf_size) {
  if (!std::has_single_bit(count) || count < 2 || count > 32768 ||
      buf_size == 0) {
    return unexpected_error(Errc::InvalidArgument);
  }
  auto slab = Region::map(static_cast<std::size_t>(count) * buf_size,
                          RegionOptions{.populate = true, .hugepage = true});
  if (!slab) {
    return std::unexpected(slab.error());
  }

  int err = 0;
  io_uring_buf_ring *br =
      io_uring_setup_buf_ring(ring.raw(), count, bgid, 0, &err);
  if (br == nullptr) {
    return unexpected_error(Errc::Unsupported, -err);
  }

  BufferRing r;
  r.ring_ = &ring;
  r.br_ = br;
  r.slab_ = std::move(*slab);
  r.count_ = count;
  r.buf_size_ = buf_size;
  r.bgid_ = bgid;
  r.mask_ = io_uring_buf_ring_mask(count);

  for (uint32_t i = 0; i < count; ++i) {
    io_uring_buf_ring_add(
        br, r.slab_.data() + static_cast<std::size_t>(i) * buf_size, buf_size,
        static_cast<unsigned short>(i), r.mask_, static_cast<int>(i));
  }
  io_uring_buf_ring_advance(br, static_cast<int>(count));
  r.available_ = count;
  return r;
}

BufferRing::BufferRing(BufferRing &&o) noexcept
    : ring_(std::exchange(o.ring_, nullptr)),
      br_(std::exchange(o.br_, nullptr)), slab_(std::move(o.slab_)),
      count_(o.count_), buf_size_(o.buf_size_), available_(o.available_),
      mask_(o.mask_), bgid_(o.bgid_), pending_(o.pending_) {}

BufferRing &BufferRing::operator=(BufferRing &&o) noexcept {
  if (this != &o) {
    destroy();
    ring_ = std::exchange(o.ring_, nullptr);
    br_ = std::exchange(o.br_, nullptr);
    slab_ = std::move(o.slab_);
    count_ = o.count_;
    buf_size_ = o.buf_size_;
    available_ = o.available_;
    mask_ = o.mask_;
    bgid_ = o.bgid_;
    pending_ = o.pending_;
  }
  return *this;
}

BufferRing::~BufferRing() { destroy(); }

void BufferRing::destroy() noexcept {
  if (br_ != nullptr && ring_ != nullptr) {
    io_uring_free_buf_ring(ring_->raw(), br_, count_, bgid_);
  }
  br_ = nullptr;
  ring_ = nullptr;
}

void BufferRing::recycle(uint16_t bid) noexcept {
  io_uring_buf_ring_add(
      br_, slab_.data() + static_cast<std::size_t>(bid) * buf_size_, buf_size_,
      bid, mask_, static_cast<int>(pending_));
  ++pending_;
  ++available_;
}

void BufferRing::publish() noexcept {
  if (pending_ != 0) {
    io_uring_buf_ring_advance(br_, static_cast<int>(pending_));
    pending_ = 0;
  }
}

} // namespace Coring5
