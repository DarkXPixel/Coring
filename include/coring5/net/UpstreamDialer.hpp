#pragma once

// Protocol-neutral "dial an upstream" step: pick a backend, open a non-blocking
// TCP socket, connect with a LINK_TIMEOUT, retry on another backend, and keep
// the health books. It knows nothing about what is spoken afterwards, so an l4
// relay, an HTTP/1 handler or a future session pool can all use it.
//
// Ownership
//   * The dialer is a member of the connection that is dialing, and it is
//     non-movable: the sockaddr and kernel_timespec of an in-flight connect SQE
//     live inside it.
//   * While connecting, the socket is parked as the connection's PEER fd
//     (ConnCore::set_peer), so a close() during the dial cancels it like any
//     other fd (I2/I3). The dialer never calls core.close(): on Failed the
//     owner closes with failure().
//   * On Connected, take() moves the socket out together with a BackendLease.
//     The owner decides what to do with them (l4: set_peer again and relay;
//     an HTTP handler: hand them to an idle cache or a session).
//
// Contract
//   * start() and on_cqe() are the only entry points; feed on_cqe() every CQE
//     of kind Connect and LinkTimeout. Both CQEs of one attempt are awaited
//     before the attempt is judged, because only then is the fd free of
//     in-flight operations and safe to replace or hand over (I3).
//   * At most min(kMaxAttempts, backends) attempts, each using a slice of ONE
//     connect budget (upstream connect_timeout, else limits.connect_timeout).
//     Retrying is the CALLER'S guarantee to allow: dial before any client byte
//     has been consumed, so a retry cannot lose or duplicate data.
//   * Local resource exhaustion (EMFILE, ENOMEM, ...) is not the backend's
//     fault: it neither retries nor counts against the backend's health.
//   * Metrics: upstream_connect_{ok,fail}, upstream_retries,
//     upstream_ejections, upstream_panic_picks.

#include "coring5/io/ConnCore.hpp"
#include "coring5/upstream/UpstreamPool.hpp"

namespace Coring5 {

enum class DialStatus : uint8_t { Pending, Connected, Failed };

struct DialedUpstream {
  FdOwner fd;         // connected, non-blocking, TCP_NODELAY
  BackendLease lease; // balanced acquire/release + right to report()
};

class UpstreamDialer {
public:
  static constexpr uint32_t kMaxAttempts = 3;

  UpstreamDialer() = default;
  UpstreamDialer(const UpstreamDialer &) = delete;
  UpstreamDialer &operator=(const UpstreamDialer &) = delete;
  UpstreamDialer(UpstreamDialer &&) = delete;
  UpstreamDialer &operator=(UpstreamDialer &&) = delete;
  ~UpstreamDialer() = default;

  // Pending: a connect is in flight. Failed: nothing is in flight, close the
  // connection with failure(). Never returns Connected (connect is async).
  [[nodiscard]] DialStatus start(IoContext &ctx, ConnCore &core,
                                 UpstreamPool &pool, UpstreamId upstream);

  // For CQEs of kind Connect / LinkTimeout; other kinds are ignored.
  [[nodiscard]] DialStatus on_cqe(IoContext &ctx, ConnCore &core,
                                  const Cqe &cqe);

  // Only after Connected, once. The socket leaves the connection's peer slot.
  [[nodiscard]] DialedUpstream take(ConnCore &core) noexcept;

  [[nodiscard]] DialStatus status() const noexcept { return status_; }
  // Valid after Failed: the Errc to close the connection with.
  [[nodiscard]] Errc failure() const noexcept { return failure_; }
  // The whole dial phase (all attempts) is bounded by this. Valid after
  // start(); callers use it for a backstop deadline on their timer.
  [[nodiscard]] Millis budget() const noexcept { return budget_; }

private:
  void attempt(IoContext &ctx, ConnCore &core);
  void settle(IoContext &ctx, ConnCore &core);
  DialStatus fail(Errc why) noexcept;

  UpstreamPool *pool_{nullptr};
  BackendLease lease_; // the attempt in progress; moved out by take()
  Endpoint target_{};  // sockaddr must outlive the connect SQE
  __kernel_timespec connect_ts_{}; // ditto (LINK_TIMEOUT)
  TimePoint deadline_{};
  Millis budget_{0};
  UpstreamId upstream_{};
  uint32_t tried_[kMaxAttempts]{};
  uint32_t attempts_{0};
  uint32_t max_attempts_{0};
  int32_t connect_res_{0};
  uint8_t pending_cqes_{0}; // CQEs still owed by the connect/timeout pair
  DialStatus status_{DialStatus::Failed};
  Errc failure_{Errc::Internal};
};

} // namespace Coring5
