#pragma once

// RAII io_uring wrapper owned by exactly ONE worker thread (invariant I1).
//
//  * Created with IORING_SETUP_SINGLE_ISSUER: it MUST be created on the thread
//    that will submit to it (hence Ring::create() is called from
//    Worker::init(), never from main()).
//  * IORING_SETUP_DEFER_TASKRUN is requested when available (kernel >= 6.1) and
//    silently dropped when the kernel rejects it.
//  * prep_* methods return false when the SQ is exhausted even after a forced
//    submit. The caller MUST then treat the operation as not submitted (do not
//    count it as in-flight) and shed load. They never dereference a null SQE
//    and never terminate().
//
// This header is the seam used by tests: tests/coring5/support/FakeRing.hpp
// mirrors this API (see docs/TESTING.md, "FakeRing").

#include "coring5/base/Result.hpp"
#include "coring5/io/OpTag.hpp"
#include <cstdint>
#include <liburing.h>
#include <memory>
#include <sys/socket.h>

namespace Coring5 {

struct RingConfig {
  uint32_t sq_entries{2048};
  uint32_t cq_entries{8192};
  bool defer_taskrun{true};
  bool enforce_min_kernel{true}; // reject kernels older than 6.1
};

struct RingCaps {
  unsigned kernel_major{0};
  unsigned kernel_minor{0};
  bool defer_taskrun{false};
  bool send_zc{false};
  bool splice{false};
  bool multishot_recv{false}; // kernel >= 6.0
  bool buf_ring{false};       // kernel >= 5.19
};

struct Cqe {
  OpTag tag;
  int32_t res;
  uint32_t flags;

  [[nodiscard]] bool more() const noexcept {
    return (flags & IORING_CQE_F_MORE) != 0U;
  }
  [[nodiscard]] bool has_buffer() const noexcept {
    return (flags & IORING_CQE_F_BUFFER) != 0U;
  }
  [[nodiscard]] bool is_notif() const noexcept {
    return (flags & IORING_CQE_F_NOTIF) != 0U;
  }
  [[nodiscard]] uint16_t buffer_id() const noexcept {
    return static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
  }
};

class Ring {
public:
  // Must be called on the thread that will use the ring.
  [[nodiscard]] static Result<std::unique_ptr<Ring>> create(const RingConfig &);

  Ring(const Ring &) = delete;
  Ring &operator=(const Ring &) = delete;
  Ring(Ring &&) = delete;
  Ring &operator=(Ring &&) = delete;
  ~Ring();

  [[nodiscard]] const RingCaps &caps() const noexcept { return caps_; }
  [[nodiscard]] io_uring *raw() noexcept { return &ring_; }

  // --- submission (all return false if no SQE could be obtained) ---
  [[nodiscard]] bool accept_multishot(int listen_fd, OpTag tag) noexcept;
  [[nodiscard]] bool poll_multishot(int fd, OpTag tag) noexcept;
  // Single-shot recv into a buffer chosen from provided-buffer group `bgid`.
  [[nodiscard]] bool recv_provided(int fd, uint16_t bgid, OpTag tag) noexcept;
  // Single-shot recv into caller-owned memory (must outlive the operation).
  [[nodiscard]] bool recv(int fd, void *buf, uint32_t len, OpTag tag) noexcept;
  // Multishot variant. Ends with a CQE lacking F_MORE (e.g. -ENOBUFS).
  [[nodiscard]] bool recv_multishot_provided(int fd, uint16_t bgid,
                                             OpTag tag) noexcept;
  [[nodiscard]] bool send(int fd, const void *buf, uint32_t len,
                          OpTag tag) noexcept;
  [[nodiscard]] bool send_zc(int fd, const void *buf, uint32_t len,
                             OpTag tag) noexcept;
  [[nodiscard]] bool connect(int fd, const sockaddr *sa, socklen_t len,
                             OpTag tag) noexcept;
  // connect + IORING_OP_LINK_TIMEOUT as ONE atomic unit: either both SQEs are
  // queued adjacent and linked (returns true; the caller must count TWO
  // in-flight operations, each producing its own CQE), or nothing was queued
  // (returns false; count nothing). The kernel_timespec and the sockaddr must
  // outlive the operation.
  [[nodiscard]] bool connect_with_timeout(int fd, const sockaddr *sa,
                                          socklen_t len,
                                          const __kernel_timespec *ts,
                                          OpTag connect_tag,
                                          OpTag timeout_tag) noexcept;
  // IORING_OP_SHUTDOWN: half-close (`how` = SHUT_WR etc.). Has its own CQE.
  [[nodiscard]] bool shutdown(int fd, int how, OpTag tag) noexcept;
  [[nodiscard]] bool timeout(const __kernel_timespec *ts, OpTag tag) noexcept;
  // Cancels every pending request on `fd`. The cancel has its own CQE.
  [[nodiscard]] bool cancel_fd(int fd, OpTag tag) noexcept;

  // Submits pending SQEs and waits for at least `min_complete` CQEs.
  [[nodiscard]] Result<unsigned>
  submit_and_wait(unsigned min_complete) noexcept;
  [[nodiscard]] Result<unsigned> submit() noexcept;
  // Asks the kernel to run deferred task work and flush CQ-overflow entries
  // into the CQ. Used after EBUSY from submit_and_wait: without it the reap
  // that follows could see an empty CQ forever (livelock). Errors are ignored.
  void flush_events() noexcept;

  // Invokes f(const Cqe&) for every ready CQE, then advances once.
  template <class F> unsigned reap(F &&f) {
    io_uring_cqe *cqe = nullptr;
    unsigned head = 0;
    unsigned n = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      ++n;
      f(Cqe{OpTag{cqe->user_data}, cqe->res, cqe->flags});
    }
    io_uring_cq_advance(&ring_, n);
    return n;
  }

  [[nodiscard]] bool cq_overflowed() const noexcept;
  [[nodiscard]] uint64_t sq_full_events() const noexcept { return sq_full_; }

private:
  Ring() = default;
  io_uring_sqe *sqe() noexcept;

  io_uring ring_{};
  RingCaps caps_{};
  uint64_t sq_full_{0};
  bool initialized_{false};
};

} // namespace Coring5
