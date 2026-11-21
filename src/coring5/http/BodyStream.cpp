#include "coring5/http/BodyStream.hpp"

#include <algorithm>
#include <cassert>

namespace Coring5 {

// A contract violation is a programming error: assert in debug builds; in
// release builds fail safe by aborting the stream instead of letting the credit
// accounting drift.
#define BODY_STREAM_REQUIRE(cond, ret)                                         \
  do {                                                                         \
    assert(cond);                                                              \
    if (!(cond)) [[unlikely]] {                                                \
      abort_with(Errc::Internal);                                              \
      return ret;                                                              \
    }                                                                          \
  } while (false)

BodyStream::BodyStream(ChunkPool &pool, StreamLimits limits) noexcept
    : pool_(&pool), high_(limits.high), low_(limits.low) {
  assert(limits.valid());
  high_ = std::max<uint32_t>(high_, 1);
  low_ = std::min(low_, high_ - 1);
}

BodyStream::~BodyStream() { queue_.clear(*pool_); }

// ------------------------------------------------------------------ producer

ClaimResult BodyStream::claim(uint32_t n) noexcept {
  if (state_ == StreamState::Aborted) {
    return {ClaimStatus::Closed, 0};
  }
  // Claiming after finish() is a producer bug, but aborting would discard the
  // data the consumer has yet to read, so just refuse.
  assert(state_ == StreamState::Open && "claim() after finish()");
  if (state_ != StreamState::Open) {
    return {ClaimStatus::Closed, 0};
  }
  BODY_STREAM_REQUIRE(claimed_ == 0 && want_ == 0 && n > 0,
                      (ClaimResult{ClaimStatus::Closed, 0}));

  const uint32_t c = std::min(n, high_); // a claim above H is never grantable
  if (credit() >= c) {
    claimed_ = c;
    assert(queue_.bytes() + uint64_t{claimed_} <= high_);
    return {ClaimStatus::Granted, c};
  }
  want_ = c;
  return {ClaimStatus::Blocked, 0};
}

WriteStatus BodyStream::push(std::span<const std::byte> data) noexcept {
  if (state_ == StreamState::Aborted) {
    return WriteStatus::Closed;
  }
  assert(state_ == StreamState::Open && "push() after finish()");
  if (state_ != StreamState::Open) {
    return WriteStatus::Closed;
  }
  BODY_STREAM_REQUIRE(claimed_ > 0 && data.size() <= claimed_,
                      WriteStatus::Closed);

  if (declared_set_ && pushed_ + data.size() > declared_) {
    abort_with(Errc::ProtocolViolation);
    return WriteStatus::Violation;
  }
  if (!queue_.push(*pool_, data)) {
    abort_with(Errc::Overloaded); // also drops the claim (I7: shed)
    return WriteStatus::Exhausted;
  }
  // Settle the WHOLE claim: queued grows by what arrived, claimed drops to 0,
  // so the unused part of a short read is returned without waking anybody.
  claimed_ = 0;
  pushed_ += data.size();
  assert(queue_.bytes() <= high_);
  if (!data.empty()) {
    wake_reader();
  }
  return WriteStatus::Ok;
}

WriteStatus BodyStream::finish(bool has_trailers) noexcept {
  if (state_ == StreamState::Aborted) {
    return WriteStatus::Closed;
  }
  assert(state_ == StreamState::Open && "finish() twice");
  if (state_ != StreamState::Open) {
    return WriteStatus::Closed;
  }
  // A producer blocked on credit has not read yet, so it cannot have seen EOF.
  BODY_STREAM_REQUIRE(want_ == 0, WriteStatus::Closed);

  claimed_ = 0; // EOF read on a granted claim: give the credit back
  if (declared_set_ && pushed_ != declared_) {
    abort_with(Errc::ProtocolViolation);
    return WriteStatus::Violation;
  }
  state_ = StreamState::Finished;
  has_trailers_ = has_trailers;
  wake_reader();
  return WriteStatus::Ok;
}

bool BodyStream::set_declared(uint64_t total) noexcept {
  if (state_ != StreamState::Open || declared_set_ || pushed_ > total) {
    return false;
  }
  declared_ = total;
  declared_set_ = true;
  return true;
}

// ------------------------------------------------------------------ consumer

void BodyStream::consume(uint32_t n) noexcept {
  if (state_ == StreamState::Aborted) {
    assert(n == 0 && "consume() on an aborted stream");
    return;
  }
  assert(n <= queue_.bytes());
  n = std::min(n, queue_.bytes());
  if (n == 0) {
    return;
  }
  queue_.consume(*pool_, n);
  wake_writer();
}

bool BodyStream::wait_readable() noexcept {
  if (state_ == StreamState::Open && queue_.empty()) {
    reader_waiting_ = true;
    return true;
  }
  return false;
}

// -------------------------------------------------------------------- either

void BodyStream::abort(Errc why) noexcept {
  assert(why != Errc::Ok);
  if (why == Errc::Ok) {
    why = Errc::Canceled;
  }
  if (state_ == StreamState::Aborted) {
    return; // the first reason wins
  }
  if (state_ == StreamState::Finished && queue_.empty()) {
    return; // the consumer already saw the whole body: nothing to cancel
  }
  abort_with(why);
}

void BodyStream::abort_with(Errc why) noexcept {
  if (state_ == StreamState::Aborted) {
    return;
  }
  state_ = StreamState::Aborted;
  error_ = why;
  queue_.clear(*pool_);
  claimed_ = 0;
  has_trailers_ = false;
  // Wake whoever registered interest, state first (already set above).
  if (want_ != 0) {
    want_ = 0;
    events_ |= StreamEvent::kWritable;
  }
  wake_reader();
}

void BodyStream::wake_writer() noexcept {
  if (want_ != 0 && state_ == StreamState::Open && queue_.bytes() <= low_ &&
      credit() >= want_) {
    want_ = 0;
    events_ |= StreamEvent::kWritable;
  }
}

void BodyStream::wake_reader() noexcept {
  if (reader_waiting_) {
    reader_waiting_ = false;
    events_ |= StreamEvent::kReadable;
  }
}

#undef BODY_STREAM_REQUIRE

} // namespace Coring5
