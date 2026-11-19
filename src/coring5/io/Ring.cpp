#include "coring5/io/Ring.hpp"

#include <cerrno>
#include <cstdio>
#include <liburing.h>
#include <poll.h>
#include <sys/utsname.h>

namespace Coring5 {

namespace {

void probe_kernel(RingCaps &caps) {
  utsname u{};
  if (::uname(&u) == 0) {
    unsigned maj = 0;
    unsigned min = 0;
    if (std::sscanf(u.release, "%u.%u", &maj, &min) == 2) {
      caps.kernel_major = maj;
      caps.kernel_minor = min;
    }
  }
  const unsigned v = caps.kernel_major * 100 + caps.kernel_minor;
  caps.buf_ring = v >= 519;
  caps.multishot_recv = v >= 600;
}

void probe_ops(io_uring &ring, RingCaps &caps) {
  io_uring_probe *p = io_uring_get_probe_ring(&ring);
  if (p == nullptr) {
    return;
  }
  caps.send_zc = io_uring_opcode_supported(p, IORING_OP_SEND_ZC) != 0;
  caps.splice = io_uring_opcode_supported(p, IORING_OP_SPLICE) != 0;
  io_uring_free_probe(p);
}

} // namespace

Result<std::unique_ptr<Ring>> Ring::create(const RingConfig &cfg) {
  std::unique_ptr<Ring> r(new Ring());
  probe_kernel(r->caps_);
  if (cfg.enforce_min_kernel &&
      (r->caps_.kernel_major * 100 + r->caps_.kernel_minor) < 601) {
    return unexpected_error(Errc::Unsupported, ENOSYS);
  }

  auto init = [&](bool defer) {
    io_uring_params p{};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
    if (defer) {
      p.flags |= IORING_SETUP_DEFER_TASKRUN;
    }
    p.cq_entries = cfg.cq_entries;
    return io_uring_queue_init_params(cfg.sq_entries, &r->ring_, &p);
  };

  bool defer = cfg.defer_taskrun;
  int rc = init(defer);
  if (rc == -EINVAL && defer) {
    defer = false;
    rc = init(false);
  }
  if (rc < 0) {
    // EPERM/ENOSYS: io_uring blocked by seccomp / kernel.io_uring_disabled.
    const bool blocked = (rc == -EPERM || rc == -ENOSYS);
    return unexpected_error(blocked ? Errc::Unsupported : Errc::Internal, -rc);
  }
  r->initialized_ = true;
  r->caps_.defer_taskrun = defer;
  probe_ops(r->ring_, r->caps_);
  return r;
}

Ring::~Ring() {
  if (initialized_) {
    io_uring_queue_exit(&ring_);
  }
}

io_uring_sqe *Ring::sqe() noexcept {
  io_uring_sqe *s = io_uring_get_sqe(&ring_);
  if (s == nullptr) [[unlikely]] {
    ++sq_full_;
    (void)io_uring_submit(&ring_); // drains the SQ into the kernel
    s = io_uring_get_sqe(&ring_);
  }
  return s;
}

bool Ring::accept_multishot(int listen_fd, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_multishot_accept(s, listen_fd, nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::poll_multishot(int fd, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_poll_multishot(s, fd, POLLIN);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::recv_provided(int fd, uint16_t bgid, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_recv(s, fd, nullptr, 0, 0);
  s->flags |= IOSQE_BUFFER_SELECT;
  s->buf_group = bgid;
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::recv(int fd, void *buf, uint32_t len, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_recv(s, fd, buf, len, 0);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::recv_multishot_provided(int fd, uint16_t bgid, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_recv_multishot(s, fd, nullptr, 0, 0);
  s->flags |= IOSQE_BUFFER_SELECT;
  s->buf_group = bgid;
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::send(int fd, const void *buf, uint32_t len, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_send(s, fd, buf, len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::send_zc(int fd, const void *buf, uint32_t len, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_send_zc(s, fd, buf, len, MSG_NOSIGNAL, 0);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::connect(int fd, const sockaddr *sa, socklen_t len,
                   OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_connect(s, fd, sa, len);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::connect_with_timeout(int fd, const sockaddr *sa, socklen_t len,
                                const __kernel_timespec *ts, OpTag connect_tag,
                                OpTag timeout_tag) noexcept {
  // The two SQEs must be adjacent in ONE submission: a link flag set on an SQE
  // that a forced submit already handed to the kernel is silently ignored, and
  // the connect would then run without its deadline. So make room for both
  // BEFORE taking either (submitting first if needed), then prepare the pair.
  if (io_uring_sq_space_left(&ring_) < 2) [[unlikely]] {
    ++sq_full_;
    (void)io_uring_submit(&ring_);
    if (io_uring_sq_space_left(&ring_) < 2) {
      return false; // nothing was prepared: caller must not count anything
    }
  }
  io_uring_sqe *c = io_uring_get_sqe(&ring_);
  io_uring_sqe *t = io_uring_get_sqe(&ring_);
  if (c == nullptr || t == nullptr) [[unlikely]] {
    // Cannot happen after the check above. If it ever did, `c` is a valid but
    // unprepared SQE: turn it into a harmless NOP so the kernel never sees
    // garbage, and report failure (the caller counts nothing, the stray NOP CQE
    // carries a zero tag and is ignored by the worker).
    if (c != nullptr) {
      io_uring_prep_nop(c);
      io_uring_sqe_set_data64(c, 0);
    }
    return false;
  }
  io_uring_prep_connect(c, fd, sa, len);
  io_uring_sqe_set_data64(c, connect_tag.raw);
  c->flags |= IOSQE_IO_LINK;
  io_uring_prep_link_timeout(t, const_cast<__kernel_timespec *>(ts), 0);
  io_uring_sqe_set_data64(t, timeout_tag.raw);
  return true;
}

bool Ring::shutdown(int fd, int how, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_shutdown(s, fd, how);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::timeout(const __kernel_timespec *ts, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_timeout(s, const_cast<__kernel_timespec *>(ts), 0, 0);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

bool Ring::cancel_fd(int fd, OpTag tag) noexcept {
  io_uring_sqe *s = sqe();
  if (s == nullptr) {
    return false;
  }
  io_uring_prep_cancel_fd(s, fd, IORING_ASYNC_CANCEL_ALL);
  io_uring_sqe_set_data64(s, tag.raw);
  return true;
}

Result<unsigned> Ring::submit_and_wait(unsigned min_complete) noexcept {
  const int rc = io_uring_submit_and_wait(&ring_, min_complete);
  if (rc < 0) {
    return unexpected_error(Errc::Internal, -rc);
  }
  return static_cast<unsigned>(rc);
}

Result<unsigned> Ring::submit() noexcept {
  const int rc = io_uring_submit(&ring_);
  if (rc < 0) {
    return unexpected_error(Errc::Internal, -rc);
  }
  return static_cast<unsigned>(rc);
}

void Ring::flush_events() noexcept { (void)io_uring_get_events(&ring_); }

bool Ring::cq_overflowed() const noexcept {
  return (IO_URING_READ_ONCE(*ring_.sq.kflags) & IORING_SQ_CQ_OVERFLOW) != 0U;
}

} // namespace Coring5
