#pragma once

// L4 TCP relay (M3): client <-> upstream with bounded queues in both
// directions (invariant I6). STATUS: interface only.
//
// Design notes:
//  * Two Connection objects (client side, upstream side) cross-referencing each
//    other ONLY through Handles; on a stale lookup the survivor treats the peer
//    as closed (this is the legitimate use of `gen` for peer links).
//  * Half-close: EOF on one side => flush the queue, shutdown(SHUT_WR) on the
//    other; fully closed when both directions are done.
//  * Backpressure: when a direction's queued bytes cross the high watermark,
//    stop re-arming recv for its source; resume below the low watermark. A
//    multishot recv that is already in flight may still deliver data after the
//    decision to pause: it must be accepted, not dropped.
//  * Strategies (selected by benchmark): copy through provided buffers, or
//    splice(sock->pipe->sock) via a per-worker pipe pool for plaintext.

namespace Coring5 {
// TODO(M3): class RelayQueue, class TcpRelayClient, class TcpRelayUpstream.
}
