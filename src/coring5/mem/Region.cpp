#include "coring5/mem/Region.hpp"

#include <cerrno>
#include <sys/mman.h>

namespace Coring5 {

Result<Region> Region::map(std::size_t bytes, RegionOptions opts) {
  if (bytes == 0) {
    return unexpected_error(Errc::InvalidArgument);
  }
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (opts.populate) {
    flags |= MAP_POPULATE;
  }
  void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (p == MAP_FAILED) {
    return unexpected_error(Errc::Overloaded, errno);
  }
  if (opts.hugepage) {
    (void)::madvise(p, bytes, MADV_HUGEPAGE); // best effort
  }
  Region r;
  r.data_ = static_cast<std::byte *>(p);
  r.size_ = bytes;
  return r;
}

void Region::reset() noexcept {
  if (data_ != nullptr) {
    ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
  }
}

} // namespace Coring5
