#pragma once

// HTTP/1.1 server session (M4.1 S1). Sans-IO: no ring, no sockets, no timers,
// no heap. One session per connection; it bridges raw bytes <-> one Exchange at
// a time (serial: it never parses a request ahead of the current response).
//
// Driving contract (the connection type, S2, does ALL I/O):
//   recv side   n = prepare_recv()   >0: arm ONE recv (may claim credit on the
//                                        request BodyStream; idempotent while a
//                                        claim is held); 0: do not recv.
//               on_data(bytes)       consumes ALL bytes (no lease is retained,
//                                    I10): partial head / blocked tail / the
//                                    pipelined tail are copied to a bounded
//                                    stage chunk. Unused credit is released.
//               on_eof(why)          peer closed / recv error.
//   exchange    on_exchange_events() call when the ExchangePool delivers our
//                                    exchange (Side::Ingress).
//                                    take_new_exchange() yields the handle once
//                                    after a request head was accepted: route
//                                    it (E2) now.
//   send side   next_send()          contiguous bytes to send (empty: nothing).
//               on_sent(n)           n bytes were sent (short sends allowed).
//   lifecycle   on_deadline(kind), on_drain(), release()
// Reloaded limits: refresh_limits() between requests (see below).
// After EVERY call check: next_send(), prepare_recv(), should_close(),
// deadline() (re-arm when `epoch` changed).
//
// Response head: ResponseWriter into the `out` chunk (status line, framing,
// Connection). Other header fields are not stored by the Exchange yet.
//
// Chunk requirement: the ChunkPool passed in has chunk_size >= max_head_bytes
// + buffer_size (config validates the sizes; the worker sizes the pool).

#include "coring5/base/Result.hpp"
#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/http/Exchange.hpp"
#include "coring5/http/ExchangePool.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/proto/http1/BodyFraming.hpp"
#include "coring5/proto/http1/ChunkedDecoder.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Coring5::Http1 {

enum class RxState : uint8_t {
  Idle,
  Head,
  ExpectWait,
  Body,
  BodyBlocked,
  Held,
  Stopped
};
enum class TxState : uint8_t {
  Idle,
  Interim,
  Head,
  ChunkHdr,
  Payload,
  End,
  Done
};

// Mirrors the io DeadlineKind names the connection maps to (no io dependency).
enum class SessionDeadline : uint8_t {
  None,
  KeepAlive,
  HeaderRead,
  BodyIdle,
  WriteStall,
  UpstreamFirstByte,
  MaxLifetime
};

struct DeadlineSpec {
  SessionDeadline kind{SessionDeadline::None};
  Millis after{0};
  uint32_t epoch{0}; // changes when the connection must (re)arm the timer
};

struct KeepAliveInput {
  bool request_keep_alive{true}; // RequestHead::keep_alive
  bool close_delimited{false};   // response framed by connection close
  bool request_incomplete{false};
  bool draining{false};
  bool limit_reached{false}; // limits.max_requests
  bool errored{false};       // framing error / abort / timeout
};

[[nodiscard]] constexpr bool
keep_alive_after(const KeepAliveInput &k) noexcept {
  return k.request_keep_alive && !k.close_delimited && !k.request_incomplete &&
         !k.draining && !k.limit_reached && !k.errored;
}

// Value of the `Connection` response header; empty = emit none.
[[nodiscard]] constexpr std::string_view
connection_token(bool keep, uint8_t minor) noexcept {
  if (!keep) {
    return "close";
  }
  return minor == 0 ? std::string_view{"keep-alive"} : std::string_view{};
}

inline constexpr std::string_view kContinue100 =
    "HTTP/1.1 100 Continue\r\n\r\n";

class ServerSession {
public:
  ServerSession(ExchangePool &exchanges, ChunkPool &stage, const Limits &limits,
                uint32_t buffer_size, SideRef self) noexcept;
  ~ServerSession();
  ServerSession(const ServerSession &) = delete;
  ServerSession &operator=(const ServerSession &) = delete;

  // ---- receive -----------------------------------------------------------
  [[nodiscard]] uint32_t prepare_recv() noexcept;
  void on_data(std::span<const char> in) noexcept;
  void on_eof(Errc why = Errc::Eof) noexcept;

  // ---- exchange ----------------------------------------------------------
  void on_exchange_events() noexcept;
  [[nodiscard]] Handle take_new_exchange() noexcept {
    const Handle h = fresh_;
    fresh_ = {};
    return h;
  }
  [[nodiscard]] Handle exchange() const noexcept { return ex_; }

  // ---- send --------------------------------------------------------------
  [[nodiscard]] std::span<const std::byte> next_send() noexcept;
  void on_sent(uint32_t n) noexcept;

  // ---- lifecycle ---------------------------------------------------------
  void on_deadline(SessionDeadline kind) noexcept;
  void on_drain() noexcept;
  // Adopts reloaded limits, but only BETWEEN requests (no exchange, nothing
  // staged, rx Idle): an in-flight request keeps the limits it started with.
  // Ignored if max_head_bytes + buffer_size no longer fits the stage chunk.
  // Cheap (one compare); call before prepare_recv() on every step.
  void refresh_limits(const Limits &l) noexcept;
  // Detaches the exchange and returns chunks. Idempotent; also in the dtor.
  void release() noexcept;

  // ---- queries -----------------------------------------------------------
  // Close the fd now: hard failure, or graceful close and output is flushed.
  [[nodiscard]] bool should_close() const noexcept {
    return hard_ || (want_close_ && !output_pending());
  }
  [[nodiscard]] bool closing() const noexcept { return want_close_ || hard_; }
  [[nodiscard]] const DeadlineSpec &deadline() const noexcept { return dl_; }
  [[nodiscard]] RxState rx() const noexcept { return rx_; }
  [[nodiscard]] TxState tx() const noexcept { return tx_; }
  [[nodiscard]] uint32_t served() const noexcept { return served_; }
  [[nodiscard]] uint32_t held_credit() const noexcept { return held_; }
  [[nodiscard]] uint32_t staged() const noexcept { return stage_len_; }
  [[nodiscard]] bool keep_alive() const noexcept { return keep_; }
  [[nodiscard]] bool hard_failed() const noexcept { return hard_; }
  // Why the session wants/wanted to close (Eof for a clean peer/keep-alive
  // end); meaningful once closing().
  [[nodiscard]] Errc close_reason() const noexcept { return close_why_; }

private:
  enum class Mode : uint8_t { None, Length, Chunked, CloseDelimited };

  [[nodiscard]] Exchange *cur() const noexcept {
    return ex_.valid() ? pool_->get(ex_) : nullptr;
  }
  [[nodiscard]] bool output_pending() const noexcept {
    return out_off_ < out_len_;
  }

  // rx
  std::size_t pump(std::span<const char> buf) noexcept;
  std::size_t run_body(std::span<const char> buf) noexcept;
  void handle_head(const RequestHead &h, std::size_t tail) noexcept;
  ClaimStatus claim_credit(Exchange &ex, uint32_t want) noexcept;
  void release_credit() noexcept;
  bool push_ok(WriteStatus st) noexcept;
  void body_complete(Exchange &ex) noexcept;
  void body_failed(Errc why, uint16_t status) noexcept;
  void stop_rx() noexcept;
  void overflow() noexcept;
  void run_stage() noexcept;
  bool stash(std::span<const char> d) noexcept;
  void stage_erase(std::size_t n) noexcept;
  void clear_stage() noexcept;
  [[nodiscard]] char *stage_ptr() const noexcept;

  // tx
  void static_error(uint16_t status, Errc why = Errc::ProtocolViolation) noexcept;
  bool begin_out() noexcept;
  void build_head(Exchange &ex) noexcept;
  void tx_pump() noexcept;
  void finish_cycle() noexcept;
  void res_failed() noexcept;
  [[nodiscard]] char *out_ptr() const noexcept;

  void fail_hard(Errc why) noexcept;
  void drop_exchange(Errc why) noexcept;
  void refresh() noexcept;
  [[nodiscard]] SessionDeadline compute_kind() const noexcept;

  ExchangePool *pool_;
  ChunkPool *chunks_;
  Limits limits_;
  uint32_t buffer_size_;
  uint32_t cap_;
  SideRef self_;

  Handle ex_{};
  Handle fresh_{};
  RxState rx_{RxState::Idle};
  TxState tx_{TxState::Idle};
  Mode mode_{Mode::None};             // response framing
  BodyKind body_kind_{BodyKind::None}; // request framing
  ContentLengthFramer cl_{};
  ChunkedDecoder dec_{};

  uint32_t stage_idx_{ChunkPool::kNil};
  uint32_t stage_len_{0};
  uint32_t out_idx_{ChunkPool::kNil};
  uint32_t out_len_{0};
  uint32_t out_off_{0};
  uint32_t held_{0};
  uint32_t pl_{0};      // chunked response: payload bytes of this chunk left
  uint64_t tx_left_{0}; // Content-Length / close-delimited bytes left
  uint32_t served_{0};
  uint8_t req_minor_{1};
  uint8_t send_src_{0}; // 0 = out chunk, 1 = response payload

  bool req_ka_{true};
  bool head_only_{false};
  bool keep_{true};
  bool req_done_{false};
  bool expect_{false};
  bool error_close_{false};
  bool drain_{false};
  bool want_close_{false};
  bool hard_{false};
  Errc close_why_{Errc::Eof};
  bool peer_eof_{false};
  bool progress_{false};
  bool payload_out_{false}; // a payload send borrows front(Res)
  DeadlineSpec dl_{};
};

} // namespace Coring5::Http1
