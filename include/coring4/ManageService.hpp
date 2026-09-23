#pragma once

#include "coring4/Listener.hpp"
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <sys/types.h>
#include <variant>
#include <vector>

namespace Coring4::Manage {
struct AddListenerRequest {
  int port{0};
  Listener::Protocol protocol{Listener::Protocol::Tcp};
  Listener::IpVersion version{Listener::IpVersion::Ipv4};
};

using ManageRequestVariant = std::variant<AddListenerRequest>;

struct ManageRequest {
  ManageRequestVariant request;
  std::function<void(int, const std::string &)> error_callback;
};

struct WorkerChannel {
  int ev_fd{-1};
  std::queue<ManageRequest> queue;
  std::mutex mtx;

  void push(const ManageRequest &req) {
    {
      std::lock_guard lock(mtx);
      queue.push(req);
    }
    uint64_t v = 1;
    ::write(ev_fd, &v, sizeof(v));
  }

  std::vector<ManageRequest> pop_all() {
    std::lock_guard lock(mtx);
    std::vector<ManageRequest> res;
    while (!queue.empty()) {
      res.push_back(std::move(queue.front()));
      queue.pop();
    }
    return res;
  }
};

class ManageService {
public:
  void add_channel(const std::shared_ptr<WorkerChannel> &channel) {
    worker_channels.push_back(channel);
  }

  void send_request(ManageRequestVariant &&request) {
    ManageRequest req;
    req.request = request;
    for (auto &worker : worker_channels) {
      worker->push(req);
    }
  }

private:
  std::vector<std::shared_ptr<WorkerChannel>> worker_channels;
};
} // namespace Coring4::Manage
