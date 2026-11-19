#include "coring5/proto/http1conn/Http1Conn.hpp"

#include "coring5/proto/http1/Parser.hpp"
#include <cerrno>

namespace Coring5 {

namespace {

constexpr std::string_view kNotImplemented =
    "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\nConnection: "
    "close\r\n\r\n";
constexpr std::string_view kBadRequest =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: "
    "close\r\n\r\n";

} // namespace

void Http1Conn::start(IoContext &ctx) {
  core.arm_timer(ctx, DeadlineKind::HeaderRead, ctx.limits->header_timeout);
  arm_recv(ctx);
}

void Http1Conn::arm_recv(IoContext &ctx) {
  if (!core.counted(ctx.ring.recv_provided(core.fd(), ctx.buffers.bgid(),
                                           core.tag(OpKind::Recv)))) {
    core.close(ctx, Errc::Overloaded); // SQ exhausted: shed this connection
  }
}

void Http1Conn::respond(IoContext &ctx, std::string_view response,
                        Errc close_reason) {
  responding_ = true;
  response_ = response;
  sent_ = 0;
  close_reason_ = close_reason;
  // The response is the only thing left to do: allow it a write deadline.
  core.arm_timer(ctx, DeadlineKind::WriteStall, ctx.limits->idle_timeout);
  if (!core.counted(ctx.ring.send(core.fd(), response_.data(),
                                  static_cast<uint32_t>(response_.size()),
                                  core.tag(OpKind::Send)))) {
    core.close(ctx, Errc::Overloaded);
  }
}

void Http1Conn::on_cqe(IoContext &ctx, const Cqe &cqe,
                       BufferRing::Lease &lease) {
  switch (cqe.tag.kind()) {
  case OpKind::Recv: {
    if (cqe.res == -ENOBUFS) {
      inc(ctx.metrics.nobufs);
      core.close(ctx, Errc::NoBuffers);
      return;
    }
    if (cqe.res == 0) {
      core.close(ctx, Errc::Eof);
      return;
    }
    if (cqe.res < 0 || !lease.valid()) {
      core.close(ctx, Errc::Reset);
      return;
    }
    inc(ctx.metrics.bytes_in, static_cast<uint64_t>(cqe.res));

    const auto bytes = lease.bytes();
    Http1::RequestHead head; // ~4 KiB on the stack, never in the slot
    const auto r = Http1::parse_head(
        {reinterpret_cast<const char *>(bytes.data()), bytes.size()},
        *ctx.limits, head);
    // `lease` is not moved from: the buffer is recycled when we return (I10).
    switch (r.status) {
    case Http1::ParseStatus::Done:
      respond(ctx, kNotImplemented, Errc::Unsupported); // TODO(M4): handlers
      return;
    case Http1::ParseStatus::NeedMore: // documented limit: no head staging yet
    case Http1::ParseStatus::Error:
      respond(ctx, kBadRequest, Errc::ProtocolViolation);
      return;
    }
    return;
  }
  case OpKind::Send: {
    if (cqe.res <= 0) {
      core.close(ctx, Errc::Reset);
      return;
    }
    sent_ += static_cast<uint32_t>(cqe.res);
    inc(ctx.metrics.bytes_out, static_cast<uint64_t>(cqe.res));
    if (sent_ < response_.size()) {
      if (!core.counted(ctx.ring.send(
              core.fd(), response_.data() + sent_,
              static_cast<uint32_t>(response_.size() - sent_),
              core.tag(OpKind::Send)))) {
        core.close(ctx, Errc::Overloaded);
      }
      return;
    }
    core.close(ctx, close_reason_); // Connection: close
    return;
  }
  default:
    return;
  }
}

void Http1Conn::on_timer(IoContext &ctx, DeadlineKind kind) {
  switch (kind) {
  case DeadlineKind::HeaderRead:
  case DeadlineKind::WriteStall:
  case DeadlineKind::Idle:
    core.close(ctx, Errc::Timeout);
    return;
  default:
    return;
  }
}

void Http1Conn::on_drain(IoContext &ctx) {
  if (!responding_) {
    core.close(ctx, Errc::Shutdown);
  }
  // else: the Send completion closes the connection (state == Draining).
}

} // namespace Coring5
