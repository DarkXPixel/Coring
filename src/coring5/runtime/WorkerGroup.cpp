#include "coring5/runtime/WorkerGroup.hpp"

#include <exception>
#include <future>

namespace Coring5 {

WorkerGroup::~WorkerGroup() { shutdown(Millis{1000}); }

std::expected<void, std::string> WorkerGroup::start(const RuntimeConfig &rt) {
  if (running_) {
    return std::unexpected("workers already started");
  }
  const std::size_t n = rt.workers;
  metrics_ = std::make_unique<WorkerMetrics[]>(n);
  mailboxes_.clear();
  for (std::size_t i = 0; i < n; ++i) {
    mailboxes_.push_back(std::make_unique<Mailbox>());
    if (!mailboxes_.back()->valid()) {
      mailboxes_.clear();
      return std::unexpected("eventfd creation failed");
    }
  }

  // Two-phase start: every worker reports init() through a promise, then waits
  // on `go` so nobody runs until ALL initialised successfully.
  std::promise<bool> go;
  std::shared_future<bool> go_future = go.get_future().share();
  std::vector<std::future<std::string>> results;
  results.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    std::promise<std::string> init_result;
    results.push_back(init_result.get_future());
    WorkerInit wi{.id = WorkerId{static_cast<uint16_t>(i)},
                  .runtime = rt,
                  .cpu = rt.cpus.empty() ? -1 : rt.cpus[i],
                  .mailbox = mailboxes_[i].get(),
                  .metrics = &metrics_[i]};
    threads_.emplace_back(
        [wi, go_future, p = std::move(init_result)]() mutable {
          Worker w(wi); // constructed on its own thread
          auto r = w.init();
          p.set_value(r ? std::string{} : r.error());
          if (r && go_future.get()) {
            w.run();
          }
        });
  }

  std::string first_error;
  for (auto &f : results) {
    std::string e = f.get();
    if (!e.empty() && first_error.empty()) {
      first_error = std::move(e);
    }
  }
  go.set_value(first_error.empty());
  if (!first_error.empty()) {
    for (auto &t : threads_) {
      t.join();
    }
    threads_.clear();
    mailboxes_.clear();
    metrics_.reset();
    return std::unexpected(std::move(first_error));
  }
  running_ = true;
  return {};
}

void WorkerGroup::shutdown(Millis drain_timeout) {
  if (!running_) {
    return;
  }
  for (auto &mb : mailboxes_) {
    mb->post(Shutdown{drain_timeout});
  }
  for (auto &t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
  running_ = false;
}

} // namespace Coring5
