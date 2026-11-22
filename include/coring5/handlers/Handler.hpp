#pragma once

// Request handlers (M4), one per route action kind. The protocol layer parses a
// request head, asks WorkerRouting (src: include/coring4/routing, being
// migrated into coring5 during M4) which action applies, and hands the request
// to the handler. Handlers produce a response through a bounded Relay
// (invariant I6).
//
// STATUS: interface only.

#include "coring5/base/Result.hpp"
#include "coring5/proto/http1/Parser.hpp"

namespace Coring5 {

enum class HandlerKind : uint8_t {
  Respond,
  Redirect,
  StaticFile,
  ReverseProxy
};

// TODO(M4):
//   class RequestHandler {
//   public:
//     virtual ~RequestHandler() = default;
//     virtual void start(IoContext&, Http1ServerSession&, const
//     Http1::RequestHead&) = 0; virtual void on_cqe(...) = 0;      //
//     upstream/file I/O completions virtual void cancel(IoContext&) = 0;
//   };
//
//   Respond      - canned status/body (also used for
//   400/404/413/431/502/503/504) Redirect     - 301/302/307/308 with Location
//   StaticFile   - see handlers/StaticFileRoot.hpp and http/Handlers.hpp;
//                  the future I/O driver owns queued reads and cancellation
//   ReverseProxy - see http/Handlers.hpp for the sans-IO handler. The egress
//                  adapter drives upstream selection, framing and I/O.

} // namespace Coring5
