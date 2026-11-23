#pragma once

#include "coring5/http/Exchange.hpp"

namespace Coring5 {

// A stale peer link is normal. Detach each missing side once so a later
// drain can reap it. Anonymous responders belong to the ingress session.
template <class Deliver>
void deliver_exchange_ingress(Exchange &ex, Deliver &&deliver) {
  if (deliver(ex.ingress().conn)) {
    return;
  }
  if (!ex.ingress_detached()) {
    ex.detach(Side::Ingress, Errc::Shutdown);
  }
  if (ex.responder_state() == Exchange::ResponderState::Attached &&
      !ex.responder().conn.handle.valid()) {
    ex.detach(Side::Responder, Errc::Shutdown);
  }
}

} // namespace Coring5
