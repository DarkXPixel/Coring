#include "coring5/proto/http3/Http3Conn.hpp"

namespace Coring5 {

void Http3Conn::start(IoContext &ctx) {
  // No UDP path exists yet (M10): nothing is in flight, so this finishes
  // immediately and the worker reaps the slot at the end of the iteration.
  core.close(ctx, Errc::Unsupported);
}

void Http3Conn::on_cqe(IoContext &, const Cqe &, IoBufferRing::Lease &) {}

void Http3Conn::on_timer(IoContext &ctx, DeadlineKind) {
  core.close(ctx, Errc::Timeout);
}

void Http3Conn::on_drain(IoContext &ctx) { core.close(ctx, Errc::Shutdown); }

} // namespace Coring5
