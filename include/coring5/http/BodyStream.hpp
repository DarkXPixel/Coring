#pragma once

// BodyStream: one direction of an HTTP exchange body (docs/EXCHANGE.md, section
// 3). Sans-IO and ring-free: it moves no bytes by itself, it is the bounded,
// credit-based queue between exactly ONE producer and ONE consumer.
//
// Credit protocol (invariant I6)
//   The producer asks for credit BEFORE it asks the transport for bytes:
//
//       claim(n)   -> Granted(g)   g = min(n, H) bytes may now be received
//                  -> Blocked      not enough credit; wait for Writable, then
//                                  call claim(n) again with the same n
//       push(data) data.size() <= g; settles the WHOLE claim (the unused part
//                  of a short read is returned implicitly)
//       finish()   end of body; returns an outstanding claim (EOF read)
//       abort(e)   terminal; drops queue and credit; wakes both sides
//
//   queued + claimed <= H holds after every operation. A claim is clamped to H
//   (an unclamped claim larger than H could never be granted).
//
// Wake rule: a blocked producer is woken (Writable) when the stream is Open,
//   queued <= L and H - queued - claimed >= want
// evaluated after every consume(). Releasing the unused part of a short claim
// does NOT wake anyone: a blocked producer holds no claim, so a release can
// never be what unblocks it (verified by tools/exchange_model.py).
//
// Events are level hints in a bitset, not callbacks: set the state first, then
// raise the bit. The owner collects them with take_events() (the Exchange will
// put the stream on the worker's ready list). Only a party that registered
// interest is woken: the producer by being Blocked, the consumer by
// wait_readable() returning true.
//
// Contract violations (a second claim before push/finish, push without a claim
// or larger than it, finish while blocked on credit) are programming errors:
// they assert in debug builds and, in release builds, abort the stream with
// Errc::Internal instead of corrupting the accounting. A producer that is
// *blocked* has not read yet and so cannot learn of EOF; finish() while blocked
// is such a violation.
//
// Memory: chunks come from the worker's ChunkPool through a ChunkQueue. push()
// on pool exhaustion aborts the stream with Errc::Overloaded itself (shed, I7)
// and reports Exhausted. The destructor returns every chunk.
//
// Trailers: this class only records WHETHER finish() carried trailers and keeps
// them invisible until the last data byte was consumed (so they can never be
// observed before the body). The trailer block itself, its storage and its
// validation belong to the Exchange (docs/EXCHANGE.md section 5).
//
// Single-threaded: owned by the worker that owns the Exchange.

#include "coring5/base/Result.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/mem/ChunkQueue.hpp"
#include <cstdint>
#include <span>

namespace Coring5 {

// High (H) and low (L) watermark in bytes. Requires 0 <= L < H and H > 0
// (validate_config already enforces L < H for the configured limits).
struct StreamLimits {
  uint32_t high{0};
  uint32_t low{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return high > 0 && low < high;
  }
  [[nodiscard]] static constexpr StreamLimits from(const Limits &l) noexcept {
    return StreamLimits{l.queue_high_watermark, l.queue_low_watermark};
  }
};

enum class StreamState : uint8_t { Open, Finished, Aborted };

enum class ClaimStatus : uint8_t {
  Granted, // `granted` bytes of credit are held
  Blocked, // wait for Writable, then claim again
  Closed,  // stream is terminal (or the call violated the contract)
};
struct ClaimResult {
  ClaimStatus status{ClaimStatus::Closed};
  uint32_t granted{0};
};

enum class WriteStatus : uint8_t {
  Ok,
  Closed,    // stream already terminal (aborted, or contract violation)
  Exhausted, // push only: chunk pool empty; the stream was aborted(Overloaded)
  Violation, // declared length exceeded/missed; the stream was aborted
};

// Bits of BodyStream::take_events().
struct StreamEvent {
  static constexpr uint8_t kReadable = 1; // consumer: look at the stream
  static constexpr uint8_t kWritable = 2; // producer: retry claim()
};

class BodyStream {
public:
  // `pool` must outlive the stream.
  BodyStream(ChunkPool &pool, StreamLimits limits) noexcept;
  ~BodyStream();
  BodyStream(const BodyStream &) = delete;
  BodyStream &operator=(const BodyStream &) = delete;
  BodyStream(BodyStream &&) = delete;
  BodyStream &operator=(BodyStream &&) = delete;

  // ---- producer ----------------------------------------------------------
  [[nodiscard]] ClaimResult claim(uint32_t n) noexcept;
  [[nodiscard]] WriteStatus push(std::span<const std::byte> data) noexcept;
  [[nodiscard]] WriteStatus finish(bool has_trailers = false) noexcept;
  // Declared body length (Content-Length). Only before the first byte beyond
  // it; false if already set, not Open, or `total` < bytes already pushed.
  // Once set: pushing more, or finishing with fewer, aborts with
  // Errc::ProtocolViolation.
  [[nodiscard]] bool set_declared(uint64_t total) noexcept;

  // ---- consumer ----------------------------------------------------------
  // Contiguous bytes at the head; valid until consume() passes them, even while
  // push() appends behind them (a byte-stream session may send() from it and
  // consume() on the send CQE).
  [[nodiscard]] std::span<const std::byte> front() const noexcept {
    return queue_.front(*pool_);
  }
  // Drops n <= queued() bytes; may raise Writable (wake rule).
  void consume(uint32_t n) noexcept;
  // Registers interest when nothing is readable. true: stream is empty and
  // Open, you will get Readable. false: data or the end is available now.
  [[nodiscard]] bool wait_readable() noexcept;

  // ---- either side -------------------------------------------------------
  // Idempotent; the first reason wins. A Finished stream whose end the consumer
  // already saw is left as it is. A Finished stream with unread data is
  // aborted (this is how a consumer cancels). Raises Writable/Readable for a
  // blocked producer / waiting consumer.
  void abort(Errc why) noexcept;

  // Returns and clears the pending events.
  [[nodiscard]] uint8_t take_events() noexcept {
    const uint8_t e = events_;
    events_ = 0;
    return e;
  }
  [[nodiscard]] uint8_t pending_events() const noexcept { return events_; }

  // ---- queries -----------------------------------------------------------
  [[nodiscard]] StreamState state() const noexcept { return state_; }
  // Aborted, or Finished and fully drained: the consumer has seen everything.
  [[nodiscard]] bool end_seen() const noexcept {
    return state_ == StreamState::Aborted ||
           (state_ == StreamState::Finished && queue_.empty());
  }
  // Errc::Ok unless Aborted.
  [[nodiscard]] Errc aborted_with() const noexcept { return error_; }
  // Finished with trailers AND the last data byte consumed.
  [[nodiscard]] bool trailers_available() const noexcept {
    return state_ == StreamState::Finished && queue_.empty() && has_trailers_;
  }
  [[nodiscard]] uint32_t queued() const noexcept { return queue_.bytes(); }
  [[nodiscard]] uint32_t claimed() const noexcept { return claimed_; }
  [[nodiscard]] uint32_t want() const noexcept { return want_; }
  [[nodiscard]] uint32_t credit() const noexcept {
    return high_ - queue_.bytes() - claimed_;
  }
  [[nodiscard]] uint64_t pushed() const noexcept { return pushed_; }
  [[nodiscard]] uint32_t high() const noexcept { return high_; }
  [[nodiscard]] uint32_t low() const noexcept { return low_; }

private:
  friend class Exchange;
  // The HTTP handshake used no request body. A negotiated tunnel starts a
  // fresh unframed stream with the same credit limits.
  [[nodiscard]] bool begin_tunnel() noexcept;
  void abort_with(Errc why) noexcept;
  void wake_writer() noexcept;
  void wake_reader() noexcept;

  ChunkPool *pool_;
  ChunkQueue queue_;
  uint64_t pushed_{0};
  uint64_t declared_{0};
  uint32_t high_;
  uint32_t low_;
  uint32_t claimed_{0}; // the one outstanding claim (0 = none)
  uint32_t want_{0};    // size of the claim the producer is blocked on
  Errc error_{Errc::Ok};
  StreamState state_{StreamState::Open};
  bool declared_set_{false};
  bool has_trailers_{false};
  bool reader_waiting_{false};
  uint8_t events_{0};
};

} // namespace Coring5
