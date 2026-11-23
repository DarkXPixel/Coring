#include "coring5/runtime/Worker.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>

namespace Coring5 {

class WorkerRingTest : public ::testing::Test {
protected:
  void SetUp() override {
    // init() blocks signals on the issuing thread, just as in WorkerGroup.
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &old_mask_), 0);
    RuntimeConfig runtime;
    runtime.buffer_count = 4;
    runtime.relay_memory_mib = 1;
    runtime.http1_stage_effective =
        Limits{}.max_head_bytes + runtime.buffer_size;
    runtime.pool_capacity[pool_index(PoolId::Http1)] = 1;
    worker = std::make_unique<Worker>(
        WorkerInit{{}, runtime, -1, &mailbox, &metrics});
    auto initialized = worker->init();
    ASSERT_TRUE(initialized) << initialized.error();
    ring = worker->ring_.get();
    ring->after_batch = [this] {
      if (get(metrics.loop_iterations) >= 12) {
        throw std::logic_error(
            "worker did not drain within the scripted iterations");
      }
      for (const auto ref : worker->ctx_->closed) {
        worker->connections_->visit(ref.pool, [&](auto &pool) {
          const auto *conn = pool.get(ref.handle);
          ASSERT_NE(conn, nullptr);
          EXPECT_EQ(conn->core.state(), ConnCore::State::Closed);
          EXPECT_EQ(conn->core.inflight(), 0U);
        });
        ++closed_count;
      }
    };

    mailbox_tag = queued(OpKind::Mailbox, mailbox.event_fd()).tag;
    auto snapshot = std::make_shared<ConfigSnapshot>();
    auto &spec = snapshot->listeners.emplace_back();
    spec.id = ListenerId{1};
    spec.protocol = ProtocolKind::Http1;
    spec.route = "default";
    auto &routes = snapshot->routes[spec.route];
    routes.hosts.emplace("", 0);
    routes.vhosts.emplace_back().exact.emplace("/", RespondRoute{200, "hello"});
    // FakeRing records fd numbers only. Give the worker actual owned fds
    // so the tests observe exactly when it releases them.
    listener = ::dup(mailbox.event_fd());
    ASSERT_GE(listener, 0);
    ApplyConfig apply{snapshot, {}, {}, {}};
    apply.add.push_back({spec.id, FdOwner(listener)});
    mailbox.post(std::move(apply));
    notify_mailbox();
    step();

    accept_tag = queued(OpKind::Accept, listener).tag;
    client = ::dup(mailbox.event_fd());
    ASSERT_GE(client, 0);
    ring->complete(accept_tag, client, IORING_CQE_F_MORE);
    step();
    ASSERT_EQ(get(metrics.conns_live), 1U);
    const auto recv = queued(OpKind::Recv, client);
    constexpr std::string_view request =
        "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
    ASSERT_NE(worker->buffers_.get(), nullptr);
    (void)worker->buffers_->fill(0, std::as_bytes(std::span(request)));
    ring->complete(recv.tag, static_cast<int32_t>(request.size()),
                   IORING_CQE_F_BUFFER);
    step();
    send = queued(OpKind::Send, client);
    ASSERT_GT(send.len, 1U);
    ASSERT_EQ(worker->exchanges_->live(), 1U);
    ASSERT_GT(worker->chunks_.peak_in_use(), 0U);
    ASSERT_GT(worker->stage_.in_use(), 0U);
  }

  void TearDown() override {
    worker.reset();
    EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr), 0);
  }

  FakeSubmission queued(OpKind kind, int fd) const {
    for (const auto &op : ring->pending) {
      if (op.kind == kind && op.fd == fd) {
        return op;
      }
    }
    throw std::logic_error("expected worker submission missing");
  }

  void step() {
    ASSERT_FALSE(worker->done_);
    worker->run_once();
  }
  void notify_mailbox() {
    ring->complete(mailbox_tag, POLLIN, IORING_CQE_F_MORE);
  }

  void shutdown(Millis timeout) {
    mailbox.post(Shutdown{timeout});
    notify_mailbox();
    step();
    listener_cancel = queued(OpKind::Cancel, listener).tag;
  }

  void expect_clean() {
    // Let the production loop process the final batch and any re-listed
    // exchange.
    worker->run();
    EXPECT_TRUE(worker->done_);
    EXPECT_EQ(closed_count, 1U);
    EXPECT_EQ(worker->connections_->live(), 0U);
    EXPECT_EQ(worker->listeners_->live(), 0U);
    EXPECT_EQ(worker->exchanges_->live(), 0U);
    EXPECT_FALSE(worker->exchanges_->has_ready());
    EXPECT_EQ(worker->buffers_->free_count(), worker->buffers_->count());
    EXPECT_EQ(worker->stage_.free_count(), worker->stage_.capacity());
    EXPECT_EQ(worker->chunks_.free_count(), worker->chunks_.capacity());
    EXPECT_EQ(get(metrics.stale_events), 0U);
    for (const int fd : {listener, client}) {
      EXPECT_EQ(fcntl(fd, F_GETFD), -1);
      EXPECT_EQ(errno, EBADF);
    }
  }

  sigset_t old_mask_{};
  Mailbox mailbox;
  WorkerMetrics metrics{};
  std::unique_ptr<Worker> worker;
  FakeRing *ring{};
  int listener{-1}, client{-1};
  OpTag mailbox_tag{}, accept_tag{}, listener_cancel{};
  FakeSubmission send{};
  unsigned closed_count{0};
};

TEST_F(WorkerRingTest, GracefulDrainFinishesShortSend) {
  shutdown(Millis{60000});
  ring->complete(listener_cancel, 0);
  ring->complete(send.tag, 1);
  step();
  EXPECT_NE(fcntl(listener, F_GETFD), -1); // final accept CQE outstanding
  EXPECT_NE(fcntl(client, F_GETFD), -1);
  const auto rest = queued(OpKind::Send, client);
  ASSERT_EQ(rest.len, send.len - 1);
  ring->complete(accept_tag, -ECANCELED);
  ring->complete(rest.tag, static_cast<int32_t>(rest.len));
  expect_clean();
  EXPECT_EQ(get(metrics.bytes_out), send.len);
}

TEST_F(WorkerRingTest, ExpiredDrainWaitsForCancelledSend) {
  shutdown(Millis{0});
  const auto cancel = queued(OpKind::Cancel, client);
  ring->complete(accept_tag, -ECANCELED);
  ring->complete(cancel.tag, 0);
  step();
  EXPECT_NE(fcntl(listener, F_GETFD), -1); // listener cancel still outstanding
  EXPECT_NE(fcntl(client, F_GETFD), -1);   // send still outstanding
  EXPECT_EQ(get(metrics.conns_live), 1U);
  ring->complete(send.tag, -ECANCELED);
  ring->complete(listener_cancel, 0);
  expect_clean();
  EXPECT_EQ(get(metrics.bytes_out), 0U);
}

} // namespace Coring5
