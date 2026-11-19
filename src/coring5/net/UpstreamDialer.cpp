#include "coring5/net/UpstreamDialer.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <sys/socket.h>

namespace Coring5 {

namespace {

// Errors that say nothing about the backend: our own resources are short, so
// neither retrying elsewhere nor charging the backend's health makes sense.
bool is_local_exhaustion(int err) noexcept {
  return err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM ||
         err == EADDRNOTAVAIL;
}

// Interrupted results carry no information about the backend either.
bool is_transient(int err) noexcept { return err == EINTR || err == EAGAIN; }

} // namespace

DialStatus UpstreamDialer::fail(Errc why) noexcept {
  failure_ = why;
  status_ = DialStatus::Failed;
  return status_;
}

DialStatus UpstreamDialer::start(IoContext &ctx, ConnCore &core,
                                 UpstreamPool &pool, UpstreamId upstream) {
  pool_ = &pool;
  upstream_ = upstream;
  attempts_ = 0;
  pending_cqes_ = 0;
  lease_.reset();

  const uint32_t backends = pool.backend_count(upstream);
  if (backends == 0) {
    return fail(Errc::UpstreamUnavailable);
  }
  max_attempts_ = std::min(kMaxAttempts, backends);
  budget_ = pool.connect_timeout(upstream, ctx.limits->connect_timeout);
  deadline_ = ctx.now + budget_;
  status_ = DialStatus::Pending;
  attempt(ctx, core);
  return status_;
}

void UpstreamDialer::attempt(IoContext &ctx, ConnCore &core) {
  const std::span<const uint32_t> tried(tried_, attempts_);
  auto ref = pool_->pick(upstream_, ctx.now, tried);
  if (!ref) {
    fail(Errc::UpstreamUnavailable);
    return;
  }
  const Endpoint *ep = pool_->endpoint(*ref);
  if (ep == nullptr) { // cannot happen right after pick(); stay defensive
    fail(Errc::Internal);
    return;
  }
  if (ref->panic) {
    inc(ctx.metrics.upstream_panic_picks);
  }
  target_ = *ep; // the connect SQE needs a stable sockaddr

  FdOwner sock(::socket(target_.family(),
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!sock) {
    fail(is_local_exhaustion(errno) ? Errc::Overloaded : Errc::Internal);
    return;
  }
  const int one = 1;
  (void)::setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Slice the remaining budget over the attempts still allowed, so the whole
  // phase stays within ONE connect timeout.
  const auto remaining = std::max(
      std::chrono::duration_cast<Nanos>(deadline_ - ctx.now), Nanos{1'000'000});
  const auto slice = remaining / (max_attempts_ - attempts_);
  connect_ts_.tv_sec =
      std::chrono::duration_cast<std::chrono::seconds>(slice).count();
  connect_ts_.tv_nsec = (slice % std::chrono::seconds{1}).count();

  // No operation is in flight on the previous socket (settle() waited for both
  // CQEs), so replacing it closes an idle fd (I3).
  core.set_peer(std::move(sock));
  lease_ = BackendLease(*pool_, *ref);
  tried_[attempts_++] = ref->backend;

  if (!core.counted(ctx.ring.connect_with_timeout(
                        core.peer_fd(), target_.sa(), target_.len, &connect_ts_,
                        core.tag(OpKind::Connect),
                        core.tag(OpKind::LinkTimeout)),
                    2)) {
    lease_.reset();
    fail(Errc::Overloaded); // SQ exhausted: shed
    return;
  }
  pending_cqes_ = 2;
  connect_res_ = 0;
}

DialStatus UpstreamDialer::on_cqe(IoContext &ctx, ConnCore &core,
                                  const Cqe &cqe) {
  if (status_ != DialStatus::Pending) {
    return status_;
  }
  switch (cqe.tag.kind()) {
  case OpKind::Connect:
    connect_res_ = cqe.res;
    [[fallthrough]];
  case OpKind::LinkTimeout:
    if (pending_cqes_ > 0 && --pending_cqes_ == 0) {
      settle(ctx, core);
    }
    break;
  default:
    break;
  }
  return status_;
}

void UpstreamDialer::settle(IoContext &ctx, ConnCore &core) {
  const int err = connect_res_ < 0 ? -connect_res_ : 0;
  if (err == 0) {
    pool_->report(lease_.ref(), true, ctx.now);
    inc(ctx.metrics.upstream_connect_ok);
    status_ = DialStatus::Connected;
    return;
  }

  inc(ctx.metrics.upstream_connect_fail);
  if (is_local_exhaustion(err)) {
    lease_.reset();
    fail(Errc::Overloaded);
    return;
  }
  // -ECANCELED here is the LINK_TIMEOUT firing, which does count. Refused,
  // unreachable, reset and timed out all count against the backend.
  if (!is_transient(err) && pool_->report(lease_.ref(), false, ctx.now)) {
    inc(ctx.metrics.upstream_ejections);
  }
  lease_.reset();

  if (attempts_ < max_attempts_ && ctx.now < deadline_ &&
      core.state() == ConnCore::State::Active) {
    inc(ctx.metrics.upstream_retries);
    attempt(ctx, core);
    return;
  }
  fail(Errc::UpstreamUnavailable);
}

DialedUpstream UpstreamDialer::take(ConnCore &core) noexcept {
  assert(status_ == DialStatus::Connected && "take() before Connected");
  status_ = DialStatus::Failed; // single use: a second take() is a bug
  failure_ = Errc::Internal;
  return DialedUpstream{core.take_peer(), std::move(lease_)};
}

} // namespace Coring5
