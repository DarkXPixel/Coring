#pragma once

// L4 (TCP passthrough) relay connection (M3).
//
// One slot owns BOTH sockets: the accepted client (core.fd()) and the upstream
// (core.peer_fd()). That is an L4-only choice (see ADR 0009): the two byte
// streams live and die together. ConnCore cancels the two fds separately on
// close (I2/I3); the relay queues are returned to the chunk pool only in the
// destructor, i.e. after inflight == 0, because a send SQE reads straight from
// queue memory.
//
//   Connecting --dialer Connected--> Relaying --both directions done--> close
//       |  dialer Failed
//       +--> close(dialer.failure())
//
// Connecting is delegated to net/UpstreamDialer (backend pick, connect with
// LINK_TIMEOUT, retry within one budget, health reporting). The client is not
// read until the dial succeeded, which is what makes the dialer's retries safe.
// This class only keeps the connected socket as its peer and the BackendLease
// for as long as the relay lives.
//
// Relaying. Two directions, each with: single-shot recv from provided buffers
// -> copy into a RelayQueue (the lease is released at once, I10) -> send from
// the queue head (short sends just consume less). At most one recv and one send
// per direction are in flight.
//   * Backpressure (I6): a direction stops re-arming its recv when its queue
//     reaches limits.queue_high_watermark (or the chunk pool is nearly empty
//     and the queue is not) and resumes at queue_low_watermark. Memory per
//     direction is bounded by high watermark + one recv.
//   * Half-close: EOF on a source -> flush its queue -> IORING_OP_SHUTDOWN
//     (SHUT_WR) on the sink; the connection closes when both directions are
//     done. Any hard error closes everything.
//   * Deadlines (I9): idle (no progress at all) and write stall (a non-empty
//     queue whose sink made no progress for idle_timeout) share the one timer
//     node, armed for the earlier of the two. -ENOBUFS on a recv is transient:
//     the recv is re-armed on the next timer tick.
//   * Drain: stop reading, flush what is queued, then close.
//
// bytes_in/bytes_out count every byte read from / written to any socket.

#include "coring5/io/ConnCore.hpp"
#include "coring5/net/UpstreamDialer.hpp"
#include "coring5/proto/l4/RelayQueue.hpp"
#include "coring5/upstream/UpstreamPool.hpp"

namespace Coring5 {

class L4Conn {
public:
  static constexpr PoolId kPool = PoolId::L4;

  explicit L4Conn(FdOwner fd) noexcept : core(std::move(fd)) {}
  ~L4Conn();
  L4Conn(const L4Conn &) = delete;
  L4Conn &operator=(const L4Conn &) = delete;

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);

private:
  enum class Phase : uint8_t { Connecting, Relaying };

  // One direction: dir 0 = client -> upstream, dir 1 = upstream -> client.
  struct Dir {
    RelayQueue q;
    TimePoint
        progress_at{}; // when the sink last progressed / queue became non-empty
    bool recv_inflight{false};
    bool send_inflight{false};
    bool paused{false};     // recv not re-armed because of a watermark
    bool retry_recv{false}; // recv hit -ENOBUFS: re-arm on the next tick
    bool src_eof{false};
    bool shut_queued{false};
    bool done{false};
  };

  // connect phase (delegated to the dialer)
  void on_dial(IoContext &ctx, DialStatus st);
  void start_relay(IoContext &ctx);

  // relay phase
  void on_recv(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease,
               unsigned d);
  void on_send(IoContext &ctx, const Cqe &cqe, unsigned d);
  void on_shutdown(IoContext &ctx, unsigned d);
  void arm_recv(IoContext &ctx, unsigned d);
  void kick_send(IoContext &ctx, unsigned d);
  void maybe_shutdown_sink(IoContext &ctx, unsigned d);
  void maybe_resume(IoContext &ctx, unsigned d);
  void check_drain(IoContext &ctx);
  void check_both_done(IoContext &ctx);
  [[nodiscard]] TimePoint deadline(const IoContext &ctx) const noexcept;
  void rearm_timer(IoContext &ctx);

  [[nodiscard]] int src_fd(unsigned d) const noexcept {
    return d == 0 ? core.fd() : core.peer_fd();
  }
  [[nodiscard]] int dst_fd(unsigned d) const noexcept {
    return d == 0 ? core.peer_fd() : core.fd();
  }

  Dir dir_[2];
  UpstreamDialer dialer_;
  BackendLease backend_; // held for the whole relay (least-connections count)
  TimePoint last_activity_{};
  ChunkPool *chunks_{nullptr};
  Phase phase_{Phase::Connecting};
};

static_assert(ConnectionType<L4Conn>);

} // namespace Coring5
