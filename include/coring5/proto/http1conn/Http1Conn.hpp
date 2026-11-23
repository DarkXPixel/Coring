#pragma once

// HTTP/1.1 server connection (M4.1 S2). Thin I/O shell around the sans-IO
// Http1::ServerSession (proto/http1session): it owns the fd, the ring
// submissions and the timer; the session owns all protocol state.
//
//   recv CQE  -> session.on_data/on_eof -> sync()
//   send CQE  -> session.on_sent        -> sync()
//   timer     -> session.on_deadline    -> sync()
//   exchange  -> on_exchange() (called by the Worker's ready-list drain)
//
// Ingress owns one session and one active route. File/HTTP egress operations
// share its ConnCore and peer fd. Per-request storage stays in the slot until
// all SQEs using it settle. A peer reset cancels both sides even while ingress
// is waiting for an upstream response. All recv leases recycle in the CQE step.

#include "coring5/http/Handlers.hpp"
#include "coring5/io/ConnCore.hpp"
#include "coring5/obs/AccessLog.hpp"
#include "coring5/proto/http1session/ServerSession.hpp"
#include "coring5/proto/http1conn/Responders.hpp"
#include <optional>

namespace Coring5 {

class Http1Conn {
public:
  static constexpr PoolId kPool = PoolId::Http1;

  explicit Http1Conn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, IoBufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);
  // The ExchangePool delivered our exchange (Side::Ingress).
  void on_exchange(IoContext &ctx);

#ifdef CORING5_FAKE_RING
  // Test-only: the S2 lifecycle tests inspect the sans-IO session's observable
  // state (deadline, close reason, served count). Compiled out of production.
  [[nodiscard]] Http1::ServerSession &session() noexcept { return *s_; }
#endif

private:
  enum class ResponseKind : uint8_t { Synthetic, Respond, Redirect };

  void begin_request() noexcept;
  void capture_request_head(const Http1::RequestHead &head) noexcept;
  void log_request(const Http1::RequestOutcome &outcome) noexcept;
  void select_route(const Http1::RequestHead &head) noexcept;
  void apply_response(Exchange &ex) noexcept;
  void sync(IoContext &ctx);
  void arm_deadline(IoContext &ctx);

  IoContext *context_{nullptr};
  ListenerId listener_{};
  // Own only the selected immutable bytes, including across a snapshot reload.
  std::shared_ptr<const std::string> response_text_;
  ResponseField location_{};
  ApplyCursor cursor_{};
  uint16_t response_status_{0};
  ResponseKind response_kind_{ResponseKind::Synthetic};
  AccessRecord access_record_{};
  TimePoint request_started_{};
  std::variant<std::monostate, FileResponder, ProxyResponder> adapter_;
  std::optional<Http1::ServerSession> s_; // destroyed before response storage
  uint32_t armed_epoch_{0};
  bool watching_disconnect_{};
  bool recv_inflight_{false};
  bool send_inflight_{false};
};

static_assert(ConnectionType<Http1Conn>);

} // namespace Coring5
