#pragma once

// Scripted stand-in for Ring / BufferRing (io/RingSeam.hpp, CORING5_FAKE_RING).
// Every prep_* records the submission (tag + kind) and returns true unless the
// test sets `sq_capacity`; completions are injected with complete(). No kernel,
// no fds are touched. Single-threaded, like the real ring.

#include "coring5/base/Result.hpp"
#include "coring5/io/Cqe.hpp"
#include "coring5/io/RingConfig.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace Coring5 {

struct FakeSubmission {
  OpKind kind{OpKind::Nop};
  OpTag tag{};
  int fd{-1};
  const void *buf{nullptr};
  uint32_t len{0};
  uint64_t offset{};
};

class FakeBufferRing;

class FakeRing {
public:
  [[nodiscard]] static Result<std::unique_ptr<FakeRing>>
  create(const RingConfig &cfg) {
    auto ring = std::make_unique<FakeRing>();
    ring->sq_capacity = cfg.sq_entries;
    return ring;
  }

  // SQ capacity before prep_* start failing (shed-load paths); ~0 = unlimited.
  uint32_t sq_capacity{~0U};
  uint64_t sq_full{0};

  bool open_file(int dir, const char *path, open_how *, OpTag t) noexcept {
    return rec(OpKind::FileOpen, t, dir, path);
  }
  bool stat_file(int fd, unsigned fields, struct statx *out, OpTag t) noexcept {
    return rec(OpKind::FileStat, t, fd, out, fields);
  }
  bool read_file(int fd, void *buf, uint32_t len, uint64_t offset,
                 OpTag t) noexcept {
    if (!rec(OpKind::Read, t, fd, buf, len)) {
      return false;
    }
    pending.back().offset = offset;
    return true;
  }
  bool cancel_tag(OpTag target, OpTag t) noexcept {
    if (!rec(OpKind::Cancel, t, -1)) {
      return false;
    }
    pending.back().offset = target.raw;
    return true;
  }
  [[nodiscard]] bool accept_multishot(int fd, OpTag t) noexcept {
    return rec(OpKind::Accept, t, fd);
  }
  bool poll_disconnect(int fd, OpTag t) noexcept {
    return rec(OpKind::Disconnect, t, fd);
  }
  [[nodiscard]] bool poll_multishot(int fd, OpTag t) noexcept {
    return rec(OpKind::Mailbox, t, fd);
  }
  [[nodiscard]] bool recv_provided(int fd, uint16_t, OpTag t) noexcept {
    return rec(OpKind::Recv, t, fd);
  }
  [[nodiscard]] bool recv(int fd, void *b, uint32_t n, OpTag t) noexcept {
    return rec(OpKind::Recv, t, fd, b, n);
  }
  [[nodiscard]] bool recv_multishot_provided(int fd, uint16_t,
                                             OpTag t) noexcept {
    return rec(OpKind::Recv, t, fd);
  }
  [[nodiscard]] bool send(int fd, const void *b, uint32_t n,
                          OpTag t) noexcept {
    return rec(OpKind::Send, t, fd, b, n);
  }
  [[nodiscard]] bool send_zc(int fd, const void *b, uint32_t n,
                             OpTag t) noexcept {
    return rec(OpKind::SendZc, t, fd, b, n);
  }
  [[nodiscard]] bool connect(int fd, const sockaddr *, socklen_t,
                             OpTag t) noexcept {
    return rec(OpKind::Connect, t, fd);
  }
  [[nodiscard]] bool connect_with_timeout(int fd, const sockaddr *, socklen_t,
                                          const __kernel_timespec *, OpTag t,
                                          OpTag lt) noexcept {
    if (pending.size() + 2 > sq_capacity) {
      ++sq_full;
      return false;
    }
    return rec(OpKind::Connect, t, fd) && rec(OpKind::LinkTimeout, lt, fd);
  }
  [[nodiscard]] bool shutdown(int fd, int, OpTag t) noexcept {
    return rec(OpKind::Shutdown, t, fd);
  }
  [[nodiscard]] bool cancel_fd(int fd, OpTag t) noexcept {
    return rec(OpKind::Cancel, t, fd);
  }
  [[nodiscard]] bool timeout(const __kernel_timespec *, OpTag t) noexcept {
    return rec(OpKind::Tick, t, -1);
  }

  // Test side: queue a completion for reap().
  void complete(OpTag t, int32_t res, uint32_t flags = 0) {
    cq_.push_back(Cqe{t, res, flags});
  }
  template <class F> unsigned reap(F &&f) {
    unsigned n = 0;
    while (!cq_.empty()) {
      const Cqe c = cq_.front();
      cq_.pop_front();
      f(c);
      ++n;
    }
    if (after_batch) {
      after_batch();
    }
    return n;
  }
  // "Submitting" moves recorded ops to `submitted` and frees SQ space.
  void flush() {
    submitted.insert(submitted.end(), pending.begin(), pending.end());
    pending.clear();
  }
  [[nodiscard]] Result<unsigned> submit_nowait() {
    const auto n = static_cast<unsigned>(pending.size());
    flush();
    return n;
  }
  // A blocking wait needs a scripted batch. Fail the test at an unexpected
  // wait (e.g. if the worker tries to sleep with a ready Exchange).
  [[nodiscard]] Result<unsigned> submit_and_wait(unsigned min_complete) {
    if (cq_.size() < min_complete) {
      throw std::logic_error("FakeRing: waiting for an unscripted completion");
    }
    return submit_nowait();
  }
  void flush_events() noexcept {}
  [[nodiscard]] bool cq_overflowed() const noexcept { return false; }
  [[nodiscard]] uint64_t sq_full_events() const noexcept { return sq_full; }

  // Inspect closed connections at the CQ batch boundary, before worker reaping.
  std::function<void()> after_batch;
  std::vector<FakeSubmission> pending;
  std::vector<FakeSubmission> submitted;

private:
  bool rec(OpKind k, OpTag t, int fd, const void *b = nullptr,
           uint32_t n = 0) {
    if (pending.size() >= sq_capacity) {
      ++sq_full;
      return false;
    }
    pending.push_back(FakeSubmission{k, t, fd, b, n});
    return true;
  }
  std::deque<Cqe> cq_;
};

class FakeBufferRing {
public:
  class Lease {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&o) noexcept
        : ring_(std::exchange(o.ring_, nullptr)), bid_(o.bid_), len_(o.len_) {}
    Lease &operator=(Lease &&o) noexcept {
      if (this != &o) {
        release();
        ring_ = std::exchange(o.ring_, nullptr);
        bid_ = o.bid_;
        len_ = o.len_;
      }
      return *this;
    }
    ~Lease() { release(); }
    [[nodiscard]] bool valid() const noexcept { return ring_ != nullptr; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] uint32_t size() const noexcept { return len_; }
    [[nodiscard]] uint16_t id() const noexcept { return bid_; }
    void release() noexcept;

  private:
    friend class FakeBufferRing;
    Lease(FakeBufferRing *r, uint16_t b, uint32_t n) noexcept
        : ring_(r), bid_(b), len_(n) {}
    FakeBufferRing *ring_{nullptr};
    uint16_t bid_{0};
    uint32_t len_{0};
  };

  [[nodiscard]] static Result<FakeBufferRing>
  create(FakeRing &, uint16_t, uint32_t count, uint32_t buf_size) {
    return FakeBufferRing(count, buf_size);
  }
  FakeBufferRing(uint32_t count, uint32_t buf_size)
      : buf_size_(buf_size), slab_(static_cast<std::size_t>(count) * buf_size),
        available_(count), count_(count) {}

  // Test side: copy `data` into buffer `bid` (len <= buf_size) and lease it.
  [[nodiscard]] Lease fill(uint16_t bid, std::span<const std::byte> data) {
    std::copy(data.begin(), data.end(),
              slab_.begin() + static_cast<std::ptrdiff_t>(bid) * buf_size_);
    return take(bid, static_cast<uint32_t>(data.size()));
  }
  [[nodiscard]] Lease take(uint16_t bid, uint32_t len) noexcept {
    --available_;
    return Lease(this, bid, len);
  }
  [[nodiscard]] std::span<const std::byte> view(uint16_t bid,
                                                uint32_t len) const noexcept {
    return {slab_.data() + static_cast<std::size_t>(bid) * buf_size_, len};
  }
  void recycle(uint16_t) noexcept { ++available_; }
  void publish() noexcept {}
  [[nodiscard]] uint16_t bgid() const noexcept { return 0; }
  [[nodiscard]] uint32_t count() const noexcept { return count_; }
  [[nodiscard]] uint32_t buf_size() const noexcept { return buf_size_; }
  [[nodiscard]] uint32_t free_count() const noexcept { return available_; }

private:
  uint32_t buf_size_;
  std::vector<std::byte> slab_;
  uint32_t available_;
  uint32_t count_;
};

inline std::span<const std::byte>
FakeBufferRing::Lease::bytes() const noexcept {
  return ring_ ? ring_->view(bid_, len_) : std::span<const std::byte>{};
}
inline void FakeBufferRing::Lease::release() noexcept {
  if (ring_ != nullptr) {
    ring_->recycle(bid_);
    ring_ = nullptr;
  }
}

} // namespace Coring5
