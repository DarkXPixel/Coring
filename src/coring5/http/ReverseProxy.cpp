#include "coring5/http/Handlers.hpp"

#include <algorithm>
#include <cstring>

namespace Coring5 {

namespace {

using BodyStatus = ProxyBodyStep::Status;

bool idempotent(std::string_view method) noexcept {
  return method == "GET" || method == "HEAD" || method == "PUT" ||
         method == "DELETE" || method == "OPTIONS" || method == "TRACE";
}

bool transient(Errc why) noexcept {
  return why == Errc::UpstreamUnavailable || why == Errc::Reset ||
         why == Errc::Eof || why == Errc::Timeout;
}

bool attached(const Exchange &ex) noexcept {
  return ex.responder_state() == Exchange::ResponderState::Attached;
}

void detach(Exchange &ex, Errc why) noexcept {
  if (attached(ex)) {
    ex.detach(Side::Responder, why);
  }
}

} // namespace

Result<> ReverseProxy::start(Exchange &ex, SideRef responder,
                             std::string_view method) noexcept {
  if (method == "CONNECT") {
    return unexpected_error(Errc::Unsupported);
  }
  if (!ex.attach_responder(responder)) {
    return unexpected_error(Errc::Canceled);
  }
  idempotent_ = idempotent(method);
  return {};
}

Result<ProxyBodyStep>
ReverseProxy::prepare_request_body(Exchange &ex,
                                   std::span<std::byte> buffer) noexcept {
  const auto &req = ex.stream(StreamId::Req);
  if (!attached(ex) ||
      (ex.phase() != Phase::Active && !ex.tunneling())) {
    return unexpected_error(Errc::Canceled);
  }
  if (req.state() == StreamState::Aborted) {
    return unexpected_error(req.aborted_with());
  }
  if (ex.wait_readable(StreamId::Req)) {
    return ProxyBodyStep{BodyStatus::Blocked};
  }
  const auto front = req.front();
  if (front.empty()) {
    return ProxyBodyStep{BodyStatus::Done};
  }
  if (buffer.empty() || request_staged_ != 0) {
    return unexpected_error(Errc::InvalidArgument);
  }
  const auto size = std::min(front.size(), buffer.size());
  std::memcpy(buffer.data(), front.data(), size);
  request_staged_ = static_cast<uint32_t>(size);
  return ProxyBodyStep{BodyStatus::Ready, request_staged_};
}

Result<> ReverseProxy::on_request_sent(Exchange &ex, uint32_t bytes) noexcept {
  const auto &req = ex.stream(StreamId::Req);
  if (!attached(ex) ||
      (ex.phase() != Phase::Active && !ex.tunneling()) ||
      req.state() == StreamState::Aborted) {
    return unexpected_error(Errc::Canceled);
  }
  if (request_staged_ == 0 || bytes > request_staged_) {
    return unexpected_error(Errc::InvalidArgument);
  }
  request_staged_ = 0;
  ex.consume(StreamId::Req, bytes);
  return {};
}

HeadStatus ReverseProxy::on_interim(Exchange &ex, uint16_t status) noexcept {
  if (!attached(ex)) {
    return HeadStatus::WrongPhase;
  }
  const auto result = ex.publish_interim(status);
  if (result == HeadStatus::Ok || result == HeadStatus::Dropped) {
    saw_interim_ = true;
  }
  return result;
}

Result<ProxyBodyStep>
ReverseProxy::apply(Exchange &ex, const ApplyCursor &cur,
                    uint32_t read_capacity) const noexcept {
  const auto &res = ex.stream(StreamId::Res);
  if (ex.ingress_detached() || res.state() == StreamState::Aborted) {
    detach(ex, Errc::Canceled);
    return unexpected_error(res.state() == StreamState::Aborted
                                ? res.aborted_with()
                                : Errc::Canceled);
  }
  if (!cur.head_done || !ex.head_published()) {
    return unexpected_error(Errc::InvalidArgument);
  }
  if (res.state() == StreamState::Finished) {
    detach(ex, Errc::Canceled);
    return ProxyBodyStep{BodyStatus::Done};
  }
  if (!attached(ex) || read_capacity == 0) {
    return unexpected_error(Errc::InvalidArgument);
  }
  if (res.want() != 0 || res.claimed() != 0) {
    return ProxyBodyStep{BodyStatus::Blocked};
  }
  const auto credit = ex.claim(StreamId::Res, read_capacity);
  if (credit.status == ClaimStatus::Blocked) {
    return ProxyBodyStep{BodyStatus::Blocked};
  }
  if (credit.status != ClaimStatus::Granted) {
    detach(ex, Errc::Internal);
    return unexpected_error(Errc::Internal);
  }
  return ProxyBodyStep{BodyStatus::Ready, credit.granted};
}

Result<> ReverseProxy::on_read(Exchange &ex, ApplyCursor &cur,
                               Result<std::span<const std::byte>> completion,
                               bool end,
                               std::span<const ResponseField> trailers) const noexcept {
  const auto &res = ex.stream(StreamId::Res);
  if (ex.ingress_detached() || res.state() == StreamState::Aborted) {
    detach(ex, Errc::Canceled);
    return unexpected_error(Errc::Canceled);
  }
  if (!attached(ex) || !cur.head_done || res.claimed() == 0) {
    return unexpected_error(Errc::InvalidArgument);
  }
  if (!completion || completion->size() > res.claimed()) {
    const Error error =
        completion ? Error{Errc::Internal, 0} : completion.error();
    detach(ex, error.code);
    return std::unexpected(error);
  }
  if (ex.push(StreamId::Res, *completion) != WriteStatus::Ok) {
    detach(ex, res.aborted_with());
    return unexpected_error(res.aborted_with());
  }
  cur.sent += completion->size();
  if (end) {
    const auto finished = ex.finish_response(trailers);
    detach(ex,
           finished == WriteStatus::Ok ? Errc::Canceled : res.aborted_with());
    if (finished != WriteStatus::Ok) {
      return unexpected_error(res.aborted_with());
    }
  }
  return {};
}

bool ReverseProxy::on_failure(Exchange &ex, Errc why,
                              ProxyRetry retry) noexcept {
  request_staged_ = 0;
  if (!attached(ex)) {
    return false;
  }
  if (ex.phase() == Phase::Active && !ex.ingress_detached()) {
    if (retries_left_ > 0 && idempotent_ && !saw_interim_ && transient(why) &&
        retry == ProxyRetry::IfUnsent &&
        ex.stream(StreamId::Req).state() != StreamState::Aborted &&
        ex.consumed(StreamId::Req) == 0 &&
        ex.stream(StreamId::Res).state() == StreamState::Open) {
      --retries_left_;
      return true;
    }
    if (ex.stream(StreamId::Res).state() == StreamState::Open) {
      (void)ex.respond_synthetic(status_for(why));
    }
  }
  detach(ex, why);
  return false;
}

} // namespace Coring5
