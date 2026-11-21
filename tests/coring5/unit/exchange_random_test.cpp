// Seeded randomized lifecycle test for the Exchange state machine
// (docs/EXCHANGE.md section 4). Drives one Exchange through random sequences of
// the model's actions (tools/exchange_model.py) and checks the three properties
// the model proves:
//
//   SAFETY    queued + claimed <= H on every stream, after every action.
//   CLEAN     at reap both streams hold no bytes and no credit, and each is
//             Aborted or fully drained.
//   LOSTWAKE  no agent is parked on a wait that can no longer be satisfied
//             without a wakeup in flight.
//
// On any mismatch the failing seed is printed so the run can be reproduced.

#include "coring5/http/Exchange.hpp"
#include "coring5/http/ExchangePool.hpp"
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <string>

using namespace Coring5;

namespace {

constexpr uint32_t kChunk = 16;
constexpr uint32_t kHigh = 64;
constexpr uint32_t kLow = 16;

SideRef side(uint32_t index) {
  return SideRef{ConnRef{PoolId::Http1, Handle{index, 1}}, /*stream=*/index};
}

// The model's action labels, kept in the same order so a failing step can be
// reported by name.
enum class Action : uint8_t {
  RespPublishHead,
  RespCancelRequest,
  RespDetach,
  DeliverHead,
  DeliverReadable,
  IngressDetach,
  ConsumerWait,
  FaultIngressDies,
  FaultResponderDies,
  FaultResponderAbortsResponse,
  Free,
  Count_,
};

const char *action_name(Action a) {
  switch (a) {
  case Action::RespPublishHead:
    return "resp publish head";
  case Action::RespCancelRequest:
    return "resp cancel request";
  case Action::RespDetach:
    return "resp detach";
  case Action::DeliverHead:
    return "deliver head";
  case Action::DeliverReadable:
    return "deliver readable";
  case Action::IngressDetach:
    return "ingress detach";
  case Action::ConsumerWait:
    return "consumer wait";
  case Action::FaultIngressDies:
    return "FAULT ingress dies";
  case Action::FaultResponderDies:
    return "FAULT responder dies";
  case Action::FaultResponderAbortsResponse:
    return "FAULT responder aborts response";
  case Action::Free:
    return "FREE";
  case Action::Count_:
    break;
  }
  return "?";
}

// One Exchange under test, plus the local bookkeeping the model keeps: whether
// the ingress/responder are still attached and whether the head was delivered.
struct Harness {
  Harness(uint32_t seed, bool request_has_body)
      : rng(seed), has_body(request_has_body) {
    auto p = ChunkPool::create(kChunk, 512);
    EXPECT_TRUE(p);
    chunk_pool = std::move(*p);
    pool = std::make_unique<ExchangePool>(1, chunk_pool,
                                          StreamLimits{kHigh, kLow});
    RequestMeta m;
    m.has_body = has_body;
    m.declared_known = has_body;
    m.declared = has_body ? 1u << 20 : 0;
    h = pool->open(side(0), m);
    EXPECT_TRUE(h.valid());
  }

  Exchange &ex() { return *pool->get(h); }

  // SAFETY: queued + claimed <= H on both streams.
  void check_safety(const char *where) {
    for (StreamId s : {StreamId::Req, StreamId::Res}) {
      const BodyStream &b = ex().stream(s);
      const uint64_t qc = uint64_t{b.queued()} + b.claimed();
      ASSERT_LE(qc, kHigh) << "SAFETY violated at " << where << " (seed "
                           << seed_ << ")";
    }
  }

  // LOSTWAKE: a producer blocked on an aborted stream, or a consumer that
  // registered interest (wait_readable) on a stream that is no longer Open,
  // with no wakeup bit pending. Wakeup bits live on the Exchange (the
  // BodyStream bits are consumed by harvest()), so check the Exchange side.
  void check_lostwake(const char *where) {
    Exchange &e = ex();
    for (StreamId s : {StreamId::Req, StreamId::Res}) {
      const BodyStream &b = e.stream(s);
      const bool producer_attached =
          s == StreamId::Req ? ingress_attached : responder_attached;
      const bool consumer_attached =
          s == StreamId::Req ? responder_attached : ingress_attached;
      // The consumer of Req is the responder; of Res is the ingress.
      const Side consumer_side =
          s == StreamId::Req ? Side::Responder : Side::Ingress;
      const uint16_t readable_bit =
          s == StreamId::Req ? ExEvent::kReqReadable : ExEvent::kResReadable;
      const uint16_t writable_bit =
          s == StreamId::Req ? ExEvent::kReqWritable : ExEvent::kResWritable;
      const uint16_t ev = e.pending_events(consumer_side);
      if (producer_attached && b.want() != 0 &&
          b.state() == StreamState::Aborted && (ev & writable_bit) == 0) {
        ADD_FAILURE() << "LOSTWAKE: producer blocked on aborted stream at "
                      << where << " (seed " << seed_ << ")";
      }
      if (consumer_attached && consumer_waiting(s) &&
          b.state() != StreamState::Open && (ev & readable_bit) == 0) {
        ADD_FAILURE() << "LOSTWAKE: consumer waiting on non-open stream "
                      << (s == StreamId::Req ? "Req" : "Res")
                      << " state=" << static_cast<int>(b.state()) << " at "
                      << where << " (seed " << seed_ << ")";
      }
    }
  }

  bool &consumer_waiting(StreamId s) {
    return s == StreamId::Req ? req_consumer_waiting : res_consumer_waiting;
  }

  // CLEAN: at reap both streams hold no bytes/credit and are Aborted or
  // drained.
  void check_clean() {
    for (StreamId s : {StreamId::Req, StreamId::Res}) {
      const BodyStream &b = ex().stream(s);
      EXPECT_EQ(b.queued(), 0U) << "CLEAN: bytes left (seed " << seed_ << ")";
      EXPECT_EQ(b.claimed(), 0U) << "CLEAN: credit left (seed " << seed_ << ")";
      const bool drained =
          b.state() == StreamState::Aborted ||
          (b.state() == StreamState::Finished && b.queued() == 0);
      EXPECT_TRUE(drained) << "CLEAN: stream not drained (seed " << seed_
                           << ")";
    }
  }

  // Apply one action; returns false if the action was not applicable (the model
  // simply has no edge for it, so the caller retries with another action).
  bool apply(Action a) {
    Exchange &e = ex();
    switch (a) {
    case Action::RespPublishHead: {
      if (!responder_attached || head_published) {
        return false;
      }
      ResponseMeta m;
      m.status = 200;
      const HeadStatus st = e.publish_final(m);
      if (st != HeadStatus::Ok) {
        return false;
      }
      head_published = true;
      return true;
    }
    case Action::RespCancelRequest: {
      if (!responder_attached || !head_published) {
        return false;
      }
      if (e.stream(StreamId::Req).end_seen()) {
        return false;
      }
      return e.cancel_request_body();
    }
    case Action::RespDetach: {
      if (!responder_attached || !head_published) {
        return false;
      }
      if (!e.stream(StreamId::Res).end_seen()) {
        return false;
      }
      e.detach(Side::Responder, Errc::Canceled);
      responder_attached = false;
      return true;
    }
    case Action::DeliverHead: {
      // The ingress observes the HeadReady hint. We must not consume the whole
      // ingress event mask here (that would also drop a pending Readable), so
      // just gate on the head having been published.
      if (!head_published) {
        return false;
      }
      return true;
    }
    case Action::DeliverReadable: {
      // The consumer observes the Readable hint: its wait is satisfied.
      Exchange &e = ex();
      bool did = false;
      for (StreamId s : {StreamId::Req, StreamId::Res}) {
        const Side consumer_side =
            s == StreamId::Req ? Side::Responder : Side::Ingress;
        const uint16_t readable_bit =
            s == StreamId::Req ? ExEvent::kReqReadable : ExEvent::kResReadable;
        if (consumer_waiting(s) &&
            (e.pending_events(consumer_side) & readable_bit) != 0) {
          consumer_waiting(s) = false;
          did = true;
        }
      }
      return did;
    }
    case Action::IngressDetach: {
      if (!ingress_attached) {
        return false;
      }
      if (!e.stream(StreamId::Res).end_seen() ||
          !e.stream(StreamId::Req).end_seen()) {
        return false;
      }
      e.detach(Side::Ingress, Errc::Reset);
      ingress_attached = false;
      return true;
    }
    case Action::ConsumerWait: {
      // The consumer registers interest on an empty Open stream. Pick a stream
      // whose consumer is still attached and that is empty and Open.
      for (StreamId s : {StreamId::Req, StreamId::Res}) {
        const bool consumer_attached =
            s == StreamId::Req ? responder_attached : ingress_attached;
        const BodyStream &b = e.stream(s);
        if (consumer_attached && b.state() == StreamState::Open &&
            b.queued() == 0 && !consumer_waiting(s)) {
          if (e.wait_readable(s)) {
            consumer_waiting(s) = true;
            return true;
          }
        }
      }
      return false;
    }
    case Action::FaultIngressDies: {
      if (!ingress_attached) {
        return false;
      }
      e.detach(Side::Ingress, Errc::Reset);
      ingress_attached = false;
      return true;
    }
    case Action::FaultResponderDies: {
      if (!responder_attached) {
        return false;
      }
      if (!head_published && ingress_attached) {
        // Router synthesizes an error head, then the responder detaches.
        (void)e.respond_synthetic(502);
        head_published = true;
      }
      e.detach(Side::Responder, Errc::UpstreamUnavailable);
      responder_attached = false;
      return true;
    }
    case Action::FaultResponderAbortsResponse: {
      if (!responder_attached || !head_published) {
        return false;
      }
      if (e.stream(StreamId::Res).state() != StreamState::Open) {
        return false;
      }
      e.abort(StreamId::Res, Errc::Reset);
      return true;
    }
    case Action::Free: {
      if (ingress_attached || responder_attached) {
        return false;
      }
      return true; // the reap happens in the driver
    }
    case Action::Count_:
      break;
    }
    return false;
  }

  // Reap through the pool's drain and assert CLEAN + no live slots.
  void reap() {
    check_clean();
    (void)pool->drain(8, [](Handle, Exchange &) {});
    EXPECT_EQ(pool->live(), 0U) << "seed " << seed_;
    EXPECT_EQ(chunk_pool.free_count(), chunk_pool.capacity())
        << "chunks leaked (seed " << seed_ << ")";
  }

  uint32_t seed_{0};
  std::mt19937_64 rng;
  bool has_body{false};
  ChunkPool chunk_pool;
  std::unique_ptr<ExchangePool> pool;
  Handle h{};
  bool ingress_attached{true};
  bool responder_attached{true};
  bool head_published{false};
  bool req_consumer_waiting{false};
  bool res_consumer_waiting{false};
};

// Drive one seed to completion. Returns the number of applied actions.
uint32_t run_seed(uint32_t seed, bool request_has_body, bool faults) {
  Harness h(seed, request_has_body);
  h.seed_ = seed;
  std::uniform_int_distribution<int> pick(0,
                                          static_cast<int>(Action::Count_) - 1);

  uint32_t applied = 0;
  for (int step = 0; step < 200; ++step) {
    if (!h.ingress_attached && !h.responder_attached) {
      h.check_safety("pre-free");
      h.check_lostwake("pre-free");
      h.reap();
      return applied;
    }
    Action a = static_cast<Action>(pick(h.rng));
    if (!faults &&
        (a == Action::FaultIngressDies || a == Action::FaultResponderDies ||
         a == Action::FaultResponderAbortsResponse)) {
      a = Action::DeliverHead; // no-fault mode: skip fault actions
    }
    if (h.apply(a)) {
      ++applied;
      h.check_safety(action_name(a));
      h.check_lostwake(action_name(a));
    }
  }
  // Ran out of steps without freeing: force the detaches and reap.
  if (h.ingress_attached) {
    h.ex().detach(Side::Ingress, Errc::Shutdown);
    h.ingress_attached = false;
  }
  if (h.responder_attached) {
    h.ex().detach(Side::Responder, Errc::Shutdown);
    h.responder_attached = false;
  }
  h.check_safety("forced-free");
  h.reap();
  return applied;
}

} // namespace

TEST(ExchangeRandom, NoFaultLifecyclesStaySafeAndClean) {
  for (uint32_t seed = 1; seed <= 2000; ++seed) {
    SCOPED_TRACE(std::string("seed ") + std::to_string(seed));
    (void)run_seed(seed, /*request_has_body=*/seed % 2 == 0, /*faults=*/false);
  }
}

TEST(ExchangeRandom, FaultLifecyclesStaySafeAndClean) {
  for (uint32_t seed = 1; seed <= 4000; ++seed) {
    SCOPED_TRACE(std::string("seed ") + std::to_string(seed));
    (void)run_seed(seed, /*request_has_body=*/seed % 3 != 0, /*faults=*/true);
  }
}
