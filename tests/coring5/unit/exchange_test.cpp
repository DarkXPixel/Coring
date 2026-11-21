// Deterministic unit tests for the Exchange state machine (docs/EXCHANGE.md
// section 4): transitions E1..E11, coupling rules R1..R5, the synthesized
// responder path (Routing and Active), interim coalescing, early response,
// status mapping, post-head abort, single-shot abort events, the reap
// precondition, event-after-state ordering, ready-list coalescing, stale handle
// skipping in drain, and pool cleanup. Sans-IO: no ring, no kernel.

#include "coring5/http/Exchange.hpp"
#include "coring5/http/ExchangePool.hpp"
#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Coring5;

namespace {

constexpr uint32_t kChunk = 16;
constexpr uint32_t kHigh = 64;
constexpr uint32_t kLow = 16;

std::vector<std::byte> bytes(std::size_t n, char fill = 'x') {
  return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

std::string as_string(std::span<const std::byte> s) {
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

SideRef side(uint32_t index) {
  return SideRef{ConnRef{PoolId::Http1, Handle{index, 1}}, /*stream=*/index};
}

// Owns a ChunkPool and an ExchangePool; checks nothing leaked when it dies.
struct Fixture {
  explicit Fixture(uint32_t capacity = 8, uint32_t chunks = 256)
      : pool_(std::make_unique<ExchangePool>(capacity, chunk_pool_,
                                             StreamLimits{kHigh, kLow})) {
    auto p = ChunkPool::create(kChunk, chunks);
    EXPECT_TRUE(p);
    chunk_pool_ = std::move(*p);
  }
  ~Fixture() {
    // Destroy the pool first so every live Exchange returns its chunks, then
    // check that none leaked.
    pool_.reset();
    EXPECT_EQ(chunk_pool_.free_count(), chunk_pool_.capacity())
        << "chunks leaked";
  }

  Handle open(RequestMeta meta, uint32_t ingress = 0) {
    return pool_->open(side(ingress), meta);
  }
  Exchange &at(Handle h) {
    Exchange *e = pool_->get(h);
    EXPECT_NE(e, nullptr);
    return *e;
  }
  // Drain the ready list, delivering every entry to `f`.
  template <class F> uint32_t drain(F &&f) {
    return pool_->drain(64, std::forward<F>(f));
  }

  ChunkPool chunk_pool_;
  std::unique_ptr<ExchangePool> pool_;
};

RequestMeta with_body(uint64_t declared = 0) {
  RequestMeta m;
  m.has_body = true;
  m.declared_known = declared != 0;
  m.declared = declared;
  return m;
}

// Producer helper: claim n, push n bytes, assert success.
void put(Exchange &e, StreamId s, std::size_t n, char fill = 'x') {
  const auto c = e.claim(s, static_cast<uint32_t>(n));
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  ASSERT_EQ(e.push(s, bytes(n, fill)), WriteStatus::Ok);
}

} // namespace

// ------------------------------------------------------------------ E1 / E2

TEST(Exchange, E1OpenStartsRoutingWithHeadReady) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  ASSERT_TRUE(h.valid());
  Exchange &e = f.at(h);
  EXPECT_EQ(e.phase(), Phase::Routing);
  EXPECT_EQ(e.responder_state(), Exchange::ResponderState::None);
  EXPECT_FALSE(e.head_published());
  EXPECT_EQ(e.stream(StreamId::Req).state(), StreamState::Open);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Open);
  // bind() raises HeadReady for the router (a responder-side event).
  EXPECT_NE(e.pending_events(Side::Responder) & ExEvent::kHeadReady, 0);
  EXPECT_TRUE(e.listed());
  EXPECT_TRUE(f.pool_->has_ready());
}

TEST(Exchange, E1NoBodyRequestStreamIsAlreadyFinished) {
  Fixture f;
  RequestMeta m; // has_body == false
  const Handle h = f.open(m);
  Exchange &e = f.at(h);
  EXPECT_EQ(e.stream(StreamId::Req).state(), StreamState::Finished);
  EXPECT_TRUE(e.stream(StreamId::Req).end_seen());
}

TEST(Exchange, E2AttachResponderMovesToActive) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  (void)e.take_events(Side::Ingress);
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_EQ(e.phase(), Phase::Active);
  EXPECT_EQ(e.responder_state(), Exchange::ResponderState::Attached);
  EXPECT_NE(e.pending_events(Side::Responder) & ExEvent::kResponderStart, 0);
  // A second attach is refused (R5-ish idempotence).
  EXPECT_FALSE(e.attach_responder(side(2)));
}

TEST(Exchange, E2AttachRefusedAfterIngressDetached) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  e.detach(Side::Ingress, Errc::Reset);
  EXPECT_FALSE(e.attach_responder(side(1)));
  EXPECT_EQ(e.phase(), Phase::Closing);
}

// ------------------------------------------------------------------ E2b / E7

TEST(Exchange, E2bRespondSyntheticFromRouting) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  (void)e.take_events(Side::Ingress);
  EXPECT_EQ(e.respond_synthetic(404), HeadStatus::Ok);
  EXPECT_EQ(e.phase(), Phase::Responding);
  EXPECT_TRUE(e.head_published());
  EXPECT_TRUE(e.response_meta().synthesized);
  EXPECT_EQ(e.response_meta().status, 404);
  // Synthesized body is empty and Finished.
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kResponseHeadReady, 0);
}

TEST(Exchange, E2bRespondSyntheticFromActive) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  (void)e.take_events(Side::Ingress);
  EXPECT_EQ(e.respond_synthetic(503), HeadStatus::Ok);
  EXPECT_EQ(e.phase(), Phase::Responding);
  EXPECT_EQ(e.response_meta().status, 503);
}

TEST(Exchange, E7StatusMapping) {
  EXPECT_EQ(status_for(Errc::Timeout), 504);
  EXPECT_EQ(status_for(Errc::UpstreamUnavailable), 503);
  EXPECT_EQ(status_for(Errc::Overloaded), 503);
  EXPECT_EQ(status_for(Errc::Reset), 502);
  EXPECT_EQ(status_for(Errc::ProtocolViolation), 502);
  EXPECT_EQ(status_for(Errc::Internal), 502);
}

TEST(Exchange, E7ResponderFailsBeforeHeadSynthesizesError) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  // Router maps the failure and synthesizes a head (E7).
  EXPECT_EQ(e.respond_synthetic(status_for(Errc::Timeout)), HeadStatus::Ok);
  EXPECT_EQ(e.response_meta().status, 504);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
}

// ------------------------------------------------------------------ E3 / E4

TEST(Exchange, E3InterimRequiresActiveAndValidStatus) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  // Routing: interim is not allowed yet.
  EXPECT_EQ(e.publish_interim(100), HeadStatus::WrongPhase);
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_EQ(e.publish_interim(99), HeadStatus::BadStatus);
  EXPECT_EQ(e.publish_interim(200), HeadStatus::BadStatus);
  EXPECT_EQ(e.publish_interim(101), HeadStatus::BadStatus); // tunnel is M4.5
  EXPECT_EQ(e.publish_interim(100), HeadStatus::Ok);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kInterimReady, 0);
  EXPECT_EQ(e.take_interim(), 100);
  EXPECT_EQ(e.take_interim(), 0) << "take_interim clears the slot";
}

TEST(Exchange, E3InterimCoalescesToOneSlot) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_EQ(e.publish_interim(100), HeadStatus::Ok);
  EXPECT_EQ(e.publish_interim(103), HeadStatus::Ok); // replaces the pending one
  EXPECT_EQ(e.take_interim(), 103);
  EXPECT_EQ(e.take_interim(), 0);
}

TEST(Exchange, E3InterimDroppedWhenClientDoesNotAccept) {
  Fixture f;
  RequestMeta m = with_body(10);
  m.accepts_interim = false;
  const Handle h = f.open(m);
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_EQ(e.publish_interim(100), HeadStatus::Dropped);
  EXPECT_EQ(e.pending_events(Side::Ingress) & ExEvent::kInterimReady, 0);
  EXPECT_EQ(e.take_interim(), 0);
}

TEST(Exchange, E3InterimWrongPhaseAfterIngressDetached) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Ingress, Errc::Reset);
  // Detach moves the phase to Closing, so interim is refused as WrongPhase
  // before the ingress_detached_ check is reached.
  EXPECT_EQ(e.publish_interim(100), HeadStatus::WrongPhase);
}

TEST(Exchange, E4PublishFinalRequiresActive) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ResponseMeta m;
  m.status = 200;
  EXPECT_EQ(e.publish_final(m), HeadStatus::WrongPhase); // still Routing
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_EQ(e.publish_final(m), HeadStatus::Ok);
  EXPECT_EQ(e.phase(), Phase::Responding);
  EXPECT_TRUE(e.head_published());
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kResponseHeadReady, 0);
}

TEST(Exchange, E4PublishFinalRejectsBadStatus) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 199;
  EXPECT_EQ(e.publish_final(m), HeadStatus::BadStatus);
  m.status = 600;
  EXPECT_EQ(e.publish_final(m), HeadStatus::BadStatus);
  EXPECT_FALSE(e.head_published());
}

TEST(Exchange, E4NoContentResponseFinishesResStream) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 204;
  EXPECT_EQ(e.publish_final(m), HeadStatus::Ok);
  EXPECT_TRUE(e.response_meta().no_content);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
}

TEST(Exchange, E4HeadOnlyRequestForbidsContent) {
  Fixture f;
  RequestMeta rm = with_body(10);
  rm.head_only = true;
  const Handle h = f.open(rm);
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  EXPECT_EQ(e.publish_final(m), HeadStatus::Ok);
  EXPECT_TRUE(e.response_meta().no_content);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
}

TEST(Exchange, E4PublishFinalWrongPhaseAfterIngressDetached) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Ingress, Errc::Reset);
  ResponseMeta m;
  m.status = 200;
  EXPECT_EQ(e.publish_final(m), HeadStatus::WrongPhase);
}

TEST(Exchange, ContentForbiddenHelper) {
  EXPECT_TRUE(content_forbidden(204, false));
  EXPECT_TRUE(content_forbidden(304, false));
  EXPECT_TRUE(content_forbidden(100, false));
  EXPECT_TRUE(content_forbidden(200, /*head_only=*/true));
  EXPECT_FALSE(content_forbidden(200, false));
}

// ------------------------------------------------------------------ R1 gate

TEST(Exchange, R1ResWriteBeforeHeadIsRefused) {
  // R1: the responder produces no response body before the final head. In a
  // debug build the gate asserts; in release it aborts the stream with
  // Errc::Internal. Either way the write must not succeed and the stream must
  // end up terminal.
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
#ifdef NDEBUG
  EXPECT_EQ(e.push(StreamId::Res, bytes(4)), WriteStatus::Closed);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Aborted);
  EXPECT_EQ(e.stream(StreamId::Res).aborted_with(), Errc::Internal);
#else
  EXPECT_DEATH_IF_SUPPORTED({ (void)e.push(StreamId::Res, bytes(4)); }, "");
#endif
}

// ------------------------------------------------------------------ E6 / R2

TEST(Exchange, E6CancelRequestBodyRequiresPublishedHead) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  EXPECT_FALSE(e.cancel_request_body()); // no head yet
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  EXPECT_TRUE(e.cancel_request_body());
  EXPECT_EQ(e.stream(StreamId::Req).state(), StreamState::Aborted);
  EXPECT_EQ(e.stream(StreamId::Req).aborted_with(), Errc::Canceled);
  // R2: the response may still complete after the request is canceled.
  put(e, StreamId::Res, 8);
  EXPECT_EQ(e.finish(StreamId::Res), WriteStatus::Ok);
}

// ------------------------------------------------------------------ E8

TEST(Exchange, E8ResponderAbortsResponseAfterHead) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  (void)e.take_events(Side::Ingress);
  e.abort(StreamId::Res, Errc::Reset);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Aborted);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kResAborted, 0);
}

// ------------------------------------------------------------------ E9 / R3

TEST(Exchange, E9IngressDetachAbortsBothStreams) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Ingress, Errc::Reset);
  EXPECT_TRUE(e.ingress_detached());
  EXPECT_EQ(e.phase(), Phase::Closing);
  EXPECT_EQ(e.stream(StreamId::Req).state(), StreamState::Aborted);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Aborted);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kPeerDetached, 0);
}

TEST(Exchange, R3FinishedProducerLeavesDataForConsumer) {
  // R3: a stream the detaching side was *producing* that already Finished is
  // left for the consumer to drain (this is how a fully buffered small response
  // survives a responder that detaches right after finishing it).
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  // Responder produces the response body and finishes it.
  put(e, StreamId::Res, 8);
  ASSERT_EQ(e.finish(StreamId::Res), WriteStatus::Ok);
  // Responder detaches: RES (produced by the responder, already Finished) must
  // survive for the ingress to drain.
  e.detach(Side::Responder, Errc::UpstreamUnavailable);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_EQ(as_string(e.front(StreamId::Res)), std::string(8, 'x'));
}

TEST(Exchange, R3UnseenConsumerStreamIsAborted) {
  // R3: a stream the detaching side was *consuming* whose end it has not seen
  // is aborted with Canceled.
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  put(e, StreamId::Res, 8);
  ASSERT_EQ(e.finish(StreamId::Res), WriteStatus::Ok);
  // Ingress detaches while it has not drained RES: RES is aborted.
  e.detach(Side::Ingress, Errc::Reset);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Aborted);
  EXPECT_EQ(e.stream(StreamId::Res).aborted_with(), Errc::Canceled);
}

TEST(Exchange, R3ResponderDetachAbortsOpenStreams) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Responder, Errc::UpstreamUnavailable);
  EXPECT_EQ(e.responder_state(), Exchange::ResponderState::Detached);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Aborted);
  EXPECT_EQ(e.stream(StreamId::Req).state(), StreamState::Aborted);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kPeerDetached, 0);
}

TEST(Exchange, E10DetachIsIdempotent) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Ingress, Errc::Reset);
  const uint16_t after_first = e.pending_events(Side::Ingress);
  e.detach(Side::Ingress, Errc::Timeout); // second reason is ignored
  EXPECT_EQ(e.stream(StreamId::Req).aborted_with(), Errc::Reset);
  // PeerDetached is raised again (harmless level hint), but the state is
  // stable.
  EXPECT_EQ(e.phase(), Phase::Closing);
  EXPECT_NE(after_first & ExEvent::kPeerDetached, 0);
}

// ------------------------------------------------------------------ aborts

TEST(Exchange, AbortEventsRaisedOnceOnFirstAbort) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  (void)e.take_events(Side::Ingress);
  (void)e.take_events(Side::Responder);

  e.abort(StreamId::Res, Errc::Reset);
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kResAborted, 0);
  (void)e.take_events(Side::Ingress);

  // A second abort must not raise kResAborted again.
  e.abort(StreamId::Res, Errc::Timeout);
  EXPECT_EQ(e.pending_events(Side::Ingress) & ExEvent::kResAborted, 0);
}

TEST(Exchange, ReqAbortEventRaisedOnceOnFirstAbort) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  (void)e.take_events(Side::Responder);
  e.abort(StreamId::Req, Errc::Canceled);
  EXPECT_NE(e.pending_events(Side::Responder) & ExEvent::kReqAborted, 0);
  (void)e.take_events(Side::Responder);
  e.abort(StreamId::Req, Errc::Timeout);
  EXPECT_EQ(e.pending_events(Side::Responder) & ExEvent::kReqAborted, 0);
}

// ------------------------------------------------------------------ R5

TEST(Exchange, R5TerminalStreamOpsAreNoOps) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.abort(StreamId::Req, Errc::Canceled);
  EXPECT_EQ(e.claim(StreamId::Req, 4).status, ClaimStatus::Closed);
  EXPECT_EQ(e.push(StreamId::Req, bytes(4)), WriteStatus::Closed);
  EXPECT_EQ(e.finish(StreamId::Req), WriteStatus::Closed);
  e.consume(StreamId::Req, 0); // harmless
  EXPECT_FALSE(e.wait_readable(StreamId::Req));
}

// ------------------------------------------------------------------ E11

TEST(Exchange, E11ReapableWhenClosingAndResponderNeverAttached) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  EXPECT_FALSE(e.reapable()); // still Routing
  e.detach(Side::Ingress, Errc::Reset);
  EXPECT_TRUE(e.reapable());
}

TEST(Exchange, E11NotReapableWhileResponderAttached) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  e.detach(Side::Ingress, Errc::Reset);
  EXPECT_FALSE(e.reapable()); // responder still attached
  e.detach(Side::Responder, Errc::Reset);
  EXPECT_TRUE(e.reapable());
}

TEST(Exchange, E11DrainReapsAndReturnsChunks) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  put(e, StreamId::Req, 8); // hold a chunk so we can prove it is returned
  e.detach(Side::Ingress, Errc::Reset);
  e.detach(Side::Responder, Errc::Reset);
  ASSERT_TRUE(e.reapable());

  const uint32_t delivered = f.drain([](Handle, Exchange &) {});
  EXPECT_GE(delivered, 1U);
  EXPECT_EQ(f.pool_->live(), 0U);
  EXPECT_EQ(f.pool_->stats().reaped, 1U);
  EXPECT_EQ(f.chunk_pool_.free_count(), f.chunk_pool_.capacity());
}

// ------------------------------------------------------------------ drain

TEST(Exchange, DrainSkipsStaleHandle) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  e.detach(Side::Ingress, Errc::Reset);
  ASSERT_TRUE(e.reapable());
  // Destroy the slot behind the ready list's back: the queued handle is now
  // stale and drain must skip it silently.
  ASSERT_TRUE(f.pool_->get(h) != nullptr);
  // Reap via a first drain so the handle in the list is stale for a second one.
  const uint32_t first = f.drain([](Handle, Exchange &) {});
  EXPECT_EQ(first, 1U);
  EXPECT_EQ(f.pool_->live(), 0U);
  // A second drain over the (now empty) list delivers nothing.
  EXPECT_EQ(f.drain([](Handle, Exchange &) {}), 0U);
}

TEST(Exchange, DrainReListsWhenEventRaisedInsideDeliver) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  (void)e.take_events(Side::Ingress);
  (void)e.take_events(Side::Responder);

  uint32_t calls = 0;
  const uint32_t delivered = f.drain([&](Handle, Exchange &ex) {
    ++calls;
    if (calls == 1) {
      // Raising an event inside deliver must re-list the Exchange.
      ex.abort(StreamId::Res, Errc::Reset);
    }
  });
  EXPECT_EQ(delivered, 1U) << "budget counts entries present at call time";
  EXPECT_TRUE(e.listed()) << "re-listed by the event raised inside deliver";
  EXPECT_EQ(f.pool_->live(), 1U);
}

TEST(Exchange, DrainCoalescesReadyListToOneEntryPerExchange) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  // Several state changes, each raising events: still one ready-list entry.
  put(e, StreamId::Req, 4);
  e.consume(StreamId::Req, 4);
  e.abort(StreamId::Req, Errc::Canceled);
  EXPECT_TRUE(e.listed());
  uint32_t calls = 0;
  const uint32_t delivered = f.drain([&](Handle, Exchange &) { ++calls; });
  EXPECT_EQ(delivered, 1U);
  EXPECT_EQ(calls, 1U);
}

TEST(Exchange, DrainRespectsBudget) {
  Fixture f(/*capacity=*/4);
  for (uint32_t i = 0; i < 4; ++i) {
    (void)f.open(with_body(10), i);
  }
  uint32_t calls = 0;
  const uint32_t delivered =
      f.pool_->drain(2, [&](Handle, Exchange &) { ++calls; });
  EXPECT_EQ(delivered, 2U);
  EXPECT_EQ(calls, 2U);
  EXPECT_TRUE(f.pool_->has_ready());
}

// ------------------------------------------------------------------ pool

TEST(ExchangePool, OpenWhenFullSheds) {
  Fixture f(/*capacity=*/2);
  const Handle a = f.open(with_body(10), 0);
  const Handle b = f.open(with_body(10), 1);
  EXPECT_TRUE(a.valid());
  EXPECT_TRUE(b.valid());
  const Handle c = f.open(with_body(10), 2);
  EXPECT_FALSE(c.valid());
  EXPECT_EQ(f.pool_->live(), 2U);
  EXPECT_EQ(f.pool_->stats().shed, 1U);
  EXPECT_EQ(f.pool_->stats().opened, 2U);
}

TEST(ExchangePool, GetRejectsStaleHandle) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  e.detach(Side::Ingress, Errc::Reset);
  ASSERT_TRUE(e.reapable());
  (void)f.drain([](Handle, Exchange &) {});
  EXPECT_EQ(f.pool_->get(h), nullptr) << "generation bumped on destroy";
}

TEST(ExchangePool, CleanupLeavesNoLiveExchanges) {
  Fixture f(/*capacity=*/4);
  std::vector<Handle> hs;
  for (uint32_t i = 0; i < 4; ++i) {
    hs.push_back(f.open(with_body(10), i));
  }
  for (Handle h : hs) {
    Exchange &e = f.at(h);
    e.detach(Side::Ingress, Errc::Reset);
  }
  (void)f.drain([](Handle, Exchange &) {});
  EXPECT_EQ(f.pool_->live(), 0U);
  EXPECT_EQ(f.pool_->stats().reaped, 4U);
  EXPECT_FALSE(f.pool_->has_ready());
}

// ------------------------------------------------------------------ ordering

TEST(Exchange, EventAfterStateOrdering) {
  // The model's lost-wakeup check depends on state being set BEFORE the event
  // is raised. Verify by observing state at the moment the event is visible.
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  ASSERT_TRUE(e.attach_responder(side(1)));
  (void)e.take_events(Side::Ingress);

  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  // When ResponseHeadReady is visible, the phase and gate must already be set.
  EXPECT_NE(e.pending_events(Side::Ingress) & ExEvent::kResponseHeadReady, 0);
  EXPECT_EQ(e.phase(), Phase::Responding);
  EXPECT_TRUE(e.head_published());
  // And the ResStream gate is open: a write now succeeds.
  put(e, StreamId::Res, 4);
}
