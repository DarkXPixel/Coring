#pragma once

// Listening-socket creation. Runs on the CONTROL thread only (invariant I5:
// everything fallible happens before a worker is involved). The resulting fd
// is handed to a worker through the mailbox.

#include "coring5/base/Endpoint.hpp"
#include "coring5/base/Result.hpp"
#include "coring5/io/FdOwner.hpp"

namespace Coring5 {

struct ListenOptions {
  int backlog{4096};
  bool reuse_port{true}; // one socket per worker, kernel load-balances accepts
  bool reuse_addr{true};
  bool tcp_nodelay{true}; // inherited by accepted sockets
  bool v6only{false};
};

// Non-blocking, close-on-exec TCP listener.
[[nodiscard]] Result<FdOwner> create_tcp_listener(const Endpoint &ep,
                                                  const ListenOptions &opts);

// Address the socket is actually bound to (resolves port 0).
[[nodiscard]] Result<Endpoint> local_endpoint(int fd);

} // namespace Coring5
