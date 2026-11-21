#pragma once

// Shared harness for the S2 Http1Conn tests (unit/http1_conn_test.cpp and
// unit/http1_conn_fake_test.cpp). It drives a real Http1Conn through the
// io/RingSeam.hpp test seam (CORING5_FAKE_RING): a FakeRing records every
// submission, FakeBufferRing hands out leases, a real TimerWheel injects
// deadlines, and a real socketpair supplies fds so ConnCore's close()/finish()
// (which call shutdown(2)/close(2)) behave exactly as in production.
//
// The harness plays the Worker: it dispatches CQEs through the same
// begin_cqe/on_cqe/end_cqe prologue the ConnPool uses, drains the ExchangePool
// into on_exchange(), reaps Closed slots, and asserts the end-state invariants
// (live()==0, inflight==0, stale_events==0, pools full, no leaked fds).

#include "coring5/http/ExchangePool.hpp"
#include "coring5/io/ConnCore.hpp"
#include "coring5/io/FakeRing.hpp"
#include "coring5/io/IoContext.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/obs/Metrics.hpp"
#include "coring5/proto/http1conn/Http1Conn.hpp"
#include <cerrno>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Coring5::Test {

inline constexpr uint32_t kChunk = 16;    // BodyStream chunk size
inline constexpr uint32_t kHigh = 64;     // BodyStream high watermark
inline constexpr uint32_t kLow = 16;      // BodyStream low watermark
inline constexpr uint32_t kBuffer = 64;   // "socket" buffer size
inline constexpr uint32_t kMaxHead = 512; // head limit
inline constexpr uint32_t kStageChunk = kMaxHead + kBuffer;

// One Http1Conn wired to the fake ring, plus the worker-side services it needs.
struct ConnHarness {
  static Limits base_limits() {
    Limits l;
    l.max_head_bytes = kMaxHead;
    l.queue_high_watermark = kHigh;
    l.queue_low_watermark = kLow;
    return l;
  }

  explicit ConnHarness(uint32_t exchange_cap = 8, uint32_t stage_chunks = 8,
                       Limits limits = base_limits())
      : limits_(limits), buffers_(64, kBuffer) {
    auto sp = ChunkPool::create(kStageChunk, stage_chunks);
    EXPECT_TRUE(sp);
    stage_ = std::move(*sp);
    auto bp = ChunkPool::create(kChunk, 256);
    EXPECT_TRUE(bp);
    chunk_pool_ = std::move(*bp);
    exchanges_ = std::make_unique<ExchangePool>(exchange_cap, chunk_pool_,
                                                StreamLimits{kHigh, kLow});
    timers_ = std::make_unique<TimerWheel>(Clock::now(), Nanos{1'000'000}, 256);
    ctx_ = std::make_unique<IoContext>(IoContext{
        ring_, buffers_, *timers_, metrics_, &limits_, Clock::now(), {}});
    ctx_->exchanges = exchanges_.get();
    ctx_->stage = &stage_;

    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    peer_fd_ = fds[1];
    conn_ = std::make_unique<Http1Conn>(FdOwner{fds[0]});
    conn_->core.bind(ConnRef{PoolId::Http1, Handle{1, 1}});
    conn_->start(*ctx_);
    reconcile(); // fold the initial recv into outstanding_
  }

  ~ConnHarness() {
    // The connection must be Closed (fd already released) before we tear down.
    conn_.reset();
    if (peer_fd_ >= 0) {
      ::close(peer_fd_);
    }
    exchanges_.reset();
    EXPECT_EQ(chunk_pool_.free_count(), chunk_pool_.capacity())
        << "body chunks leaked";
    EXPECT_EQ(stage_.free_count(), stage_.capacity()) << "stage chunks leaked";
  }

  Http1Conn &c() { return *conn_; }
  Http1::ServerSession &session() { return conn_->session(); }
  IoContext &ctx() { return *ctx_; }
  FakeRing &ring() { return ring_; }
  FakeBufferRing &buffers() { return buffers_; }
  ExchangePool &exchanges() { return *exchanges_; }
  WorkerMetrics &metrics() { return metrics_; }

  // ---- worker-side dispatch ---------------------------------------------
  // Submissions the connection has queued but not yet completed. FakeRing keeps
  // every submission forever (it only appends), so the harness tracks the live
  // ones itself: `reconciled_` is the prefix of ring_.pending already folded
  // into `outstanding_`. Each entry carries a unique `id` so a CQE can be tied
  // to one specific submission even when several share a tag (e.g. successive
  // sends on one connection).
  struct Outstanding : FakeSubmission {
    uint64_t id{0};
  };
  std::vector<Outstanding> outstanding_;
  std::size_t reconciled_{0};
  uint64_t next_id_{1};

  // Move any newly queued submission into `outstanding_`.
  void reconcile() {
    for (; reconciled_ < ring_.pending.size(); ++reconciled_) {
      Outstanding o;
      static_cast<FakeSubmission &>(o) = ring_.pending[reconciled_];
      o.id = next_id_++;
      outstanding_.push_back(o);
    }
  }

  // Deliver one CQE exactly like ConnPool::on_cqe (I2/I3 prologue + epilogue).
  void deliver(const Cqe &cqe, IoBufferRing::Lease &&lease = {}) {
    // Drop the matching outstanding submission (one CQE per counted op).
    for (auto it = outstanding_.begin(); it != outstanding_.end(); ++it) {
      if (it->tag.raw == cqe.tag.raw && it->kind == cqe.tag.kind()) {
        outstanding_.erase(it);
        break;
      }
    }
    if (conn_->core.begin_cqe(cqe)) {
      conn_->on_cqe(*ctx_, cqe, lease);
    }
    conn_->core.end_cqe(*ctx_);
    reconcile();
  }

  // Deliver a CQE for one specific recorded submission (unambiguous even when
  // several submissions share a tag, e.g. successive sends on one connection).
  void deliver_op(const Outstanding &s, int32_t res,
                  IoBufferRing::Lease &&lease = {}) {
    for (auto it = outstanding_.begin(); it != outstanding_.end(); ++it) {
      if (it->id == s.id) {
        outstanding_.erase(it);
        break;
      }
    }
    const Cqe cqe{s.tag, res, 0};
    if (conn_->core.begin_cqe(cqe)) {
      conn_->on_cqe(*ctx_, cqe, lease);
    }
    conn_->core.end_cqe(*ctx_);
    reconcile();
  }

  // True while a recv submission is queued and not yet completed: only then may
  // a recv CQE arrive (the kernel completes an armed op, nothing else).
  [[nodiscard]] bool recv_armed() const {
    for (const auto &s : outstanding_) {
      if (s.kind == OpKind::Recv) {
        return true;
      }
    }
    return false;
  }

  // A recv completion carrying `data` in a provided buffer. No-op unless a recv
  // is actually armed (a CQE cannot arrive for an op that was never queued).
  void deliver_recv(std::string_view data, int32_t res = -1) {
    if (!recv_armed()) {
      return;
    }
    const int32_t n = res >= 0 ? res : static_cast<int32_t>(data.size());
    auto lease = buffers_.fill(
        0, std::span<const std::byte>(
               reinterpret_cast<const std::byte *>(data.data()), data.size()));
    deliver(Cqe{conn_->core.tag(OpKind::Recv), n, IORING_CQE_F_BUFFER},
            std::move(lease));
  }

  void deliver_recv_eof() {
    if (recv_armed()) {
      deliver(Cqe{conn_->core.tag(OpKind::Recv), 0, 0});
    }
  }
  void deliver_recv_error(int32_t err) {
    if (recv_armed()) {
      deliver(Cqe{conn_->core.tag(OpKind::Recv), err, 0});
    }
  }
  // Complete the most recently queued outstanding send with `n` bytes.
  void deliver_send(int32_t n) {
    for (auto it = outstanding_.rbegin(); it != outstanding_.rend(); ++it) {
      if (it->kind == OpKind::Send) {
        deliver_op(*it, n);
        return;
      }
    }
  }

  // Drive a Closing connection to Closed: complete every still-outstanding
  // recv/send with -ECANCELED (the kernel cancels them when the fd is canceled)
  // and then every cancel SQE. Each counted submission gets exactly one CQE
  // (I2).
  void flush_pending() {
    // Snapshot: deliver_op() erases from outstanding_. Stop as soon as the
    // connection reaches Closed (no CQE may target a Closed slot, I3).
    std::vector<Outstanding> ops = outstanding_;
    for (const auto &s : ops) {
      if (conn_->core.state() == ConnCore::State::Closed) {
        return;
      }
      if (s.kind == OpKind::Recv || s.kind == OpKind::Send) {
        deliver_op(s, -ECANCELED);
      }
    }
    ops = outstanding_;
    for (const auto &s : ops) {
      if (conn_->core.state() == ConnCore::State::Closed) {
        return;
      }
      if (s.kind == OpKind::Cancel) {
        deliver_op(s, -ECANCELED);
      }
    }
  }

  // Deliver every ready exchange to the connection (as
  // Worker::drain_exchanges). After reap there is no connection: detach the
  // ingress so the pool can reap the exchange (E11).
  void drain_exchanges() {
    for (int i = 0; i < 16 && exchanges_->has_ready(); ++i) {
      exchanges_->drain(64, [this](Handle, Exchange &ex) {
        if (reaped_) {
          // The session's release() already detached the ingress; just let the
          // pool reap it (calling detach again would re-list it forever).
        } else if (!conn_->core.closing() || conn_->core.inflight() == 0) {
          conn_->on_exchange(*ctx_);
        } else if (!ex.ingress_detached()) {
          ex.detach(Side::Ingress, Errc::Shutdown);
        }
      });
    }
    reconcile();
  }

  // Fire the connection's currently armed deadline (advance the wheel far
  // enough that any scheduled node is due).
  void fire_timer() {
    timers_->advance(
        Clock::now() + std::chrono::hours(1), [this](TimerNode &n) {
          if (n.owner == conn_->core.handle() && !conn_->core.closing()) {
            conn_->on_timer(*ctx_, n.kind);
          }
        });
    reconcile();
  }

  void on_drain() {
    conn_->on_drain(*ctx_);
    reconcile();
  }

  void on_exchange() {
    conn_->on_exchange(*ctx_);
    reconcile();
  }

  // Submissions queued but not yet completed (test-side view).
  std::vector<Outstanding> &outstanding() { return outstanding_; }

  // Reap Closed connections (Worker::reap_closed): verify the close completed,
  // then destroy the slot (which runs ~ServerSession -> release(), returning
  // the exchange and the stage chunk, exactly as the worker does).
  void reap_closed() {
    for (const ConnRef r : ctx_->closed) {
      EXPECT_EQ(r.pool, PoolId::Http1);
      EXPECT_EQ(r.handle, conn_->core.handle());
    }
    ctx_->closed.clear();
    EXPECT_TRUE(closed()) << "reaped a connection that is not Closed";
    EXPECT_EQ(conn_->core.inflight(), 0U) << "inflight != 0 at reap";
    conn_.reset();
    reaped_ = true;
  }

  bool closed() const {
    return reaped_ || conn_->core.state() == ConnCore::State::Closed;
  }
  bool closing() const { return conn_->core.closing(); }

  // End-state invariants (INVARIANTS.md review checklist). Valid after reap.
  void expect_clean_end() {
    EXPECT_TRUE(reaped_) << "connection not reaped";
    // The session's release() detached the exchange; one drain reaps it (E11).
    drain_exchanges();
    EXPECT_EQ(get(metrics_.stale_events), 0U) << "stale_events != 0";
    EXPECT_EQ(exchanges_->live(), 0U) << "exchanges leaked";
    EXPECT_FALSE(exchanges_->has_ready()) << "ready list not drained";
    EXPECT_EQ(chunk_pool_.free_count(), chunk_pool_.capacity());
    EXPECT_EQ(stage_.free_count(), stage_.capacity());
  }

  Limits limits_{};
  FakeRing ring_;
  FakeBufferRing buffers_;
  WorkerMetrics metrics_{};
  ChunkPool stage_;
  ChunkPool chunk_pool_;
  std::unique_ptr<ExchangePool> exchanges_;
  std::unique_ptr<TimerWheel> timers_;
  std::unique_ptr<IoContext> ctx_;
  std::unique_ptr<Http1Conn> conn_;
  int peer_fd_{-1};
  bool reaped_{false};
};

// A minimal valid HTTP/1.1 request head.
inline std::string req(std::string_view method, std::string_view target,
                       std::string extra = {}) {
  std::string r = std::string(method) + " " + std::string(target) +
                  " HTTP/1.1\r\nHost: a\r\n";
  r += extra;
  r += "\r\n";
  return r;
}

} // namespace Coring5::Test
