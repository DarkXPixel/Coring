// M4.1 S2: Http1Conn — the I/O shell around the sans-IO ServerSession.
//
// These tests drive a real Http1Conn through the io/RingSeam.hpp test seam
// (CORING5_FAKE_RING): a FakeRing records every submission, FakeBufferRing
// hands out leases, a real TimerWheel injects deadlines and a real socketpair
// supplies fds. The harness plays the Worker (CQE prologue/epilogue, exchange
// drain, reap). Every test that closes the connection ends with the
// INVARIANTS.md review-checklist state: live()==0, inflight==0,
// stale_events==0, pools full, no leaked fds.
//
// Routing is not implemented yet: Http1Conn answers every accepted request with
// a synthetic 501, which is exactly what these tests assert.

#include "http1_conn_harness.hpp"

using namespace Coring5;
using namespace Coring5::Test;

namespace {

// A recv CQE carrying `data`; then deliver any exchange it opened.
void feed(ConnHarness &h, std::string_view data) {
  h.deliver_recv(data);
  h.drain_exchanges();
}

// Index of the most recent un-completed Send submission, or -1.
int pending_send_index(ConnHarness &h) {
  int idx = -1;
  for (std::size_t i = 0; i < h.outstanding().size(); ++i) {
    if (h.outstanding()[i].kind == OpKind::Send) {
      idx = static_cast<int>(i);
    }
  }
  return idx;
}

// Drain every pending send by completing it with the full requested length.
std::string drain_sends(ConnHarness &h) {
  std::string out;
  for (int i = 0; i < 64; ++i) {
    const int idx = pending_send_index(h);
    if (idx < 0) {
      break;
    }
    const FakeSubmission s = h.outstanding()[static_cast<std::size_t>(idx)];
    out.append(static_cast<const char *>(s.buf), s.len);
    h.deliver_send(static_cast<int32_t>(s.len));
    h.drain_exchanges();
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------- start/sync

TEST(Http1Conn, StartArmsOneRecvAndHeaderDeadline) {
  ConnHarness h;
  int recvs = 0, sends = 0;
  for (const auto &p : h.ring().pending) {
    recvs += p.kind == OpKind::Recv;
    sends += p.kind == OpKind::Send;
  }
  EXPECT_EQ(recvs, 1);
  EXPECT_EQ(sends, 0);
  EXPECT_EQ(h.c().core.inflight(), 1U);
  EXPECT_FALSE(h.closing());
  EXPECT_EQ(h.session().deadline().kind, Http1::SessionDeadline::HeaderRead);
}

TEST(Http1Conn, RequestHeadIsAnsweredWithSynthetic501) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  const std::string out = drain_sends(h);
  EXPECT_NE(out.find("HTTP/1.1 501"), std::string::npos) << out;
  EXPECT_FALSE(h.closing());
  EXPECT_EQ(h.session().served(), 1U);
  // Keep-alive: a fresh recv is armed and the connection is idle.
  EXPECT_EQ(h.session().rx(), Http1::RxState::Idle);
  EXPECT_EQ(h.session().deadline().kind, Http1::SessionDeadline::KeepAlive);
}

TEST(Http1Conn, SendIsQueuedAtMostOncePerSync) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  int sends = 0;
  for (const auto &p : h.ring().pending) {
    sends += p.kind == OpKind::Send;
  }
  EXPECT_EQ(sends, 1);
  // A second sync (via a spurious exchange drain) must not queue another send.
  h.drain_exchanges();
  sends = 0;
  for (const auto &p : h.ring().pending) {
    sends += p.kind == OpKind::Send;
  }
  EXPECT_EQ(sends, 1);
}

TEST(Http1Conn, ShortSendResumesRemainder) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  const int idx = pending_send_index(h);
  ASSERT_GE(idx, 0);
  const uint32_t full = h.outstanding()[static_cast<std::size_t>(idx)].len;
  h.deliver_send(3); // short send
  h.drain_exchanges();
  const int idx2 = pending_send_index(h);
  ASSERT_GE(idx2, 0);
  EXPECT_LT(h.outstanding()[static_cast<std::size_t>(idx2)].len, full);
  EXPECT_FALSE(h.closing());
}

// ------------------------------------------------------------- recv CQE paths

TEST(Http1Conn, RecvEofInHeadClosesGracefully) {
  ConnHarness h;
  h.deliver_recv_eof();
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(h.c().core.inflight(), 0U);
  EXPECT_EQ(h.session().close_reason(), Errc::Eof);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, RecvErrorInHeadClosesGracefully) {
  ConnHarness h;
  h.deliver_recv_error(-ECONNRESET);
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  // In the head state a peer error is an orderly close (no request in flight).
  EXPECT_EQ(h.session().close_reason(), Errc::Eof);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, RecvErrorInBodyIsHardError) {
  ConnHarness h;
  feed(h, req("POST", "/", "Content-Length: 5\r\n"));
  h.deliver_recv_error(-ECONNRESET);
  h.flush_pending(); // a 501 send was still in flight when the error hit
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(h.session().close_reason(), Errc::Reset);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, RecvEnoBufsShedsConnection) {
  ConnHarness h;
  h.deliver_recv_error(-ENOBUFS);
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(get(h.metrics().nobufs), 1U);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, SendErrorClosesWithReset) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  h.deliver_send(-EPIPE);
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  // Http1Conn closes the fd itself on a send error (not via the session).
  EXPECT_EQ(get(h.metrics().closes[static_cast<std::size_t>(Errc::Reset)]), 1U);
  h.reap_closed();
  h.expect_clean_end();
}

// --------------------------------------------------------------- timers (I9)

TEST(Http1Conn, HeaderDeadlineFires408) {
  ConnHarness h;
  h.fire_timer(); // header-read deadline with no request yet
  const std::string out = drain_sends(h);
  EXPECT_NE(out.find("408"), std::string::npos) << out;
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(h.session().close_reason(), Errc::Timeout);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, KeepAliveDeadlineClosesAfterOneRequest) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  drain_sends(h);
  EXPECT_EQ(h.session().deadline().kind, Http1::SessionDeadline::KeepAlive);
  h.fire_timer();
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(h.session().close_reason(), Errc::Timeout);
  h.reap_closed();
  h.expect_clean_end();
}

// -------------------------------------------------------------- on_drain

TEST(Http1Conn, DrainWhileIdleClosesImmediately) {
  ConnHarness h;
  h.on_drain();
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(h.session().close_reason(), Errc::Shutdown);
  h.reap_closed();
  h.expect_clean_end();
}

TEST(Http1Conn, DrainFinishesInFlightResponse) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  h.on_drain();
  const std::string out = drain_sends(h);
  EXPECT_NE(out.find("HTTP/1.1 501"), std::string::npos) << out;
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  h.reap_closed();
  h.expect_clean_end();
}

// ------------------------------------------------------- refresh_limits (I8)

TEST(Http1Conn, RefreshLimitsAppliesBetweenRequests) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  // Change the snapshot between requests; the sync() that runs when the
  // response finishes (rx_ == Idle) adopts it (refresh_limits).
  Limits l = h.limits_;
  l.keepalive_timeout = Millis{1234};
  h.limits_ = l;
  drain_sends(h);
  ASSERT_EQ(h.session().rx(), Http1::RxState::Idle);
  EXPECT_EQ(h.session().deadline().kind, Http1::SessionDeadline::KeepAlive);
  EXPECT_EQ(h.session().deadline().after, Millis{1234});
}

TEST(Http1Conn, RefreshLimitsIgnoredMidRequest) {
  ConnHarness h;
  // A request with a body keeps the session mid-request (rx_ != Idle).
  feed(h, req("POST", "/", "Content-Length: 5\r\n"));
  Limits l = h.limits_;
  l.header_timeout = Millis{1234};
  h.limits_ = l;
  h.deliver_recv("ab");
  // The in-flight request keeps its limits: the new header timeout is not used.
  EXPECT_NE(h.session().deadline().after, Millis{1234});
}

// ------------------------------------------------------------- on_exchange

TEST(Http1Conn, ExchangeDeliveredAfterCloseReleasesSession) {
  ConnHarness h;
  feed(h, req("GET", "/"));
  // Close while the exchange is still live, then deliver it: the session must
  // release it once nothing is in flight.
  h.c().core.close(h.ctx(), Errc::Shutdown);
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  h.reap_closed();
  // The session's release() detached the exchange; expect_clean_end() drains
  // it.
  h.expect_clean_end();
}

// ------------------------------------------------------------- pipelining

TEST(Http1Conn, PipelinedRequestsAreSerial) {
  ConnHarness h;
  feed(h, req("GET", "/a") + req("GET", "/b"));
  const std::string out = drain_sends(h);
  EXPECT_NE(out.find("HTTP/1.1 501"), std::string::npos);
  EXPECT_EQ(h.session().served(), 2U);
}

// ------------------------------------------------------------- SQ exhaustion

TEST(Http1Conn, SqFullOnRecvShedsConnection) {
  ConnHarness h;
  // Complete the request, then make the next submission fail (SQ exhausted).
  feed(h, req("GET", "/"));
  drain_sends(h);
  h.ring().sq_capacity = 0; // every submission now fails
  h.deliver_recv("G");      // sync wants to re-arm a recv -> SQ full -> shed
  h.flush_pending();
  EXPECT_TRUE(h.closed());
  EXPECT_EQ(get(h.metrics().closes[static_cast<std::size_t>(Errc::Overloaded)]),
            1U);
  h.reap_closed();
  h.expect_clean_end();
}
