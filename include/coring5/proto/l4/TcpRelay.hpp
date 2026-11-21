#pragma once

// L4 TCP relay (M3). The implementation lives in:
//   proto/l4/L4Conn.hpp       connection type: relay of two byte directions
//   mem/ChunkQueue.hpp        bounded per-direction byte queue (chunk list)
//   mem/ChunkPool.hpp         fixed-size chunk allocator behind the queues
//   net/UpstreamDialer.hpp    protocol-neutral dial: pick, connect, retry
//   upstream/UpstreamPool.hpp backend selection, passive health, panic mode
//
// Design decisions (see docs/adr/0009-l4-relay-single-slot.md). They apply to
// L4 only; HTTP/TLS/static upstream handling is a separate M4 ADR.
//  * ONE pool slot owns both sockets (client = core.fd(), upstream =
//    core.peer_fd()); ConnCore cancels the two fds separately. No peer handles.
//  * Data is copied out of the shared provided-buffer ring into the relay queue
//    at once (invariant I10), so a slow reader can never starve the ring.
//  * A direction pauses its source at queue_high_watermark and resumes at
//    queue_low_watermark (I6). Queue memory is bounded by the worker's chunk
//    pool (runtime.relay_memory_mib).
//  * Half-close: EOF on a source -> flush its queue -> IORING_OP_SHUTDOWN
//    (SHUT_WR) on the sink; close when both directions are done.
//  * Splice (sock -> pipe -> sock) is NOT implemented: the roadmap makes it
//    benchmark-gated and no benchmark exists yet.
