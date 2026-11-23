#pragma once

// Exchange: protocol-neutral request/response pairing (docs/EXCHANGE.md s.4).
// Phase + ReqStream (ingress -> responder) + ResStream (responder -> ingress)
// + attachment flags + pending events. Sans-IO, no timers (sessions own
// deadlines and call detach(self, Timeout)). Events are raised AFTER the state
// change and put the Exchange on the ReadyList (once).
//
// Header fields can borrow responder-owned bytes until ingress serializes them.

#include "coring5/base/Result.hpp"
#include "coring5/base/Types.hpp"
#include "coring5/http/BodyStream.hpp"
#include "coring5/http/ReadyList.hpp"
#include <cstdint>
#include <span>
#include <string_view>

namespace Coring5 {

enum class Side : uint8_t { Ingress, Responder };
enum class StreamId : uint8_t { Req, Res };
enum class Phase : uint8_t { Routing, Active, Responding, Closing };
enum class HeadStatus : uint8_t { Ok, WrongPhase, BadStatus, Gone, Dropped };

struct SideRef {
  ConnRef conn{};
  uint32_t stream{0};
};

struct RequestMeta {
  bool head_only{false}, has_body{false}, declared_known{false};
  uint64_t declared{0};
  bool accepts_interim{true}, client_accepts_trailers{false};
};

struct ResponseMeta {
  uint16_t status{0};
  bool no_content{false}, synthesized{false}, declared_known{false};
  uint64_t declared{0};
};

struct ResponseField {
  std::string_view name;
  std::string_view value;
};

[[nodiscard]] constexpr bool content_forbidden(uint16_t status,
                                               bool head_only) noexcept {
  return head_only || status < 200 || status == 204 || status == 304;
}

[[nodiscard]] constexpr uint16_t status_for(Errc e) noexcept {
  switch (e) {
  case Errc::Timeout:
    return 504;
  case Errc::UpstreamUnavailable:
  case Errc::Overloaded:
  case Errc::Shutdown:
    return 503;
  default:
    return 502;
  }
}

namespace ExEvent {
inline constexpr uint16_t kHeadReady = 1u << 0;
inline constexpr uint16_t kResponderStart = 1u << 1;
inline constexpr uint16_t kInterimReady = 1u << 2;
inline constexpr uint16_t kResponseHeadReady = 1u << 3;
inline constexpr uint16_t kReqReadable = 1u << 4;
inline constexpr uint16_t kResWritable = 1u << 5;
inline constexpr uint16_t kReqWritable = 1u << 6;
inline constexpr uint16_t kResReadable = 1u << 7;
inline constexpr uint16_t kReqAborted = 1u << 8;
inline constexpr uint16_t kResAborted = 1u << 9;
inline constexpr uint16_t kPeerDetached = 1u << 10;
inline constexpr uint16_t kIngressMask =
    kInterimReady | kResponseHeadReady | kResReadable | kReqWritable |
    kReqAborted | kResAborted | kPeerDetached;
inline constexpr uint16_t kResponderMask =
    kHeadReady | kResponderStart | kReqReadable | kResWritable | kReqAborted |
    kResAborted | kPeerDetached;
} // namespace ExEvent

class Exchange {
public:
  enum class ResponderState : uint8_t { None, Attached, Detached };

  Exchange(ChunkPool &pool, StreamLimits limits, SideRef ingress,
           RequestMeta meta) noexcept;
  Exchange(const Exchange &) = delete;
  Exchange &operator=(const Exchange &) = delete;
  Exchange(Exchange &&) = delete;
  Exchange &operator=(Exchange &&) = delete;

  // E2: Routing -> Active.
  [[nodiscard]] bool attach_responder(SideRef responder) noexcept;
  // E2b/E7: synthesized final head + empty body; Routing or Active.
  [[nodiscard]] HeadStatus respond_synthetic(uint16_t status) noexcept;
  // E3: 1xx (not 101); 1-slot, a newer one replaces a pending one.
  [[nodiscard]] HeadStatus publish_interim(uint16_t status) noexcept;
  // The adapter validates a 101 handshake before switching both streams to
  // unframed bytes. Only an HTTP request with no body can switch.
  [[nodiscard]] HeadStatus
  publish_tunnel(std::span<const ResponseField> fields) noexcept;
  [[nodiscard]] bool tunneling() const noexcept { return res_meta_.status == 101; }
  // E4: final head; opens the ResStream gate (R1).
  // The responder keeps fields and their bytes alive through synchronous
  // ingress serialization or until ingress detaches. Framing and hop-by-hop
  // fields belong to the session.
  [[nodiscard]] HeadStatus
  publish_final(const ResponseMeta &meta,
                std::span<const ResponseField> fields = {}) noexcept;
  [[nodiscard]] std::span<const ResponseField> take_response_fields() noexcept {
    const auto fields = response_fields_;
    response_fields_ = {};
    return fields;
  }
  // Pending interim status (0 = none); clears it.
  [[nodiscard]] uint16_t take_interim() noexcept {
    const uint16_t s = interim_;
    interim_ = 0;
    return s;
  }

  // E6: responder stops reading the request after the head was published.
  bool cancel_request_body() noexcept;
  // E7-E10 via R3. Idempotent; any party may detach any side (forced detach).
  void detach(Side who, Errc why) noexcept;

  // ---- stream wrappers (events raised after the state change) -----------
  // Res writes before publish_final violate R1: assert in debug, abort(Res,
  // Internal) in release.
  [[nodiscard]] ClaimResult claim(StreamId s, uint32_t n) noexcept;
  [[nodiscard]] WriteStatus push(StreamId s,
                                 std::span<const std::byte> d) noexcept;
  [[nodiscard]] WriteStatus finish(StreamId s,
                                   bool has_trailers = false) noexcept;
  // Borrowed responder fields, like the head. The consumer sees them only
  // after draining the body.
  [[nodiscard]] WriteStatus
  finish_response(std::span<const ResponseField> trailers) noexcept;
  [[nodiscard]] std::span<const ResponseField> response_trailers() const noexcept {
    return res_.trailers_available() ? response_trailers_
                                     : std::span<const ResponseField>{};
  }
  void consume(StreamId s, uint32_t n) noexcept;
  [[nodiscard]] bool wait_readable(StreamId s) noexcept;
  void abort(StreamId s, Errc why) noexcept;
  [[nodiscard]] std::span<const std::byte> front(StreamId s) const noexcept {
    return stream(s).front();
  }
  [[nodiscard]] const BodyStream &stream(StreamId s) const noexcept {
    return s == StreamId::Req ? req_ : res_;
  }
  // Bytes consumed so far (progress indicator for session deadlines).
  [[nodiscard]] uint64_t consumed(StreamId s) const noexcept {
    const BodyStream &b = stream(s);
    return b.pushed() - b.queued();
  }

  // ---- events ------------------------------------------------------------
  [[nodiscard]] uint16_t take_events(Side who) noexcept {
    uint16_t &e = who == Side::Ingress ? ing_events_ : resp_events_;
    const uint16_t r = e;
    e = 0;
    return r;
  }
  [[nodiscard]] uint16_t pending_events(Side who) const noexcept {
    return who == Side::Ingress ? ing_events_ : resp_events_;
  }
  [[nodiscard]] bool listed() const noexcept { return listed_; }

  // ---- queries -----------------------------------------------------------
  [[nodiscard]] Phase phase() const noexcept { return phase_; }
  [[nodiscard]] const RequestMeta &request_meta() const noexcept {
    return req_meta_;
  }
  [[nodiscard]] const ResponseMeta &response_meta() const noexcept {
    return res_meta_;
  }
  [[nodiscard]] bool head_published() const noexcept { return head_published_; }
  [[nodiscard]] const SideRef &ingress() const noexcept { return ingress_; }
  [[nodiscard]] const SideRef &responder() const noexcept { return responder_; }
  [[nodiscard]] ResponderState responder_state() const noexcept {
    return responder_state_;
  }
  [[nodiscard]] bool ingress_detached() const noexcept {
    return ingress_detached_;
  }
  // E11 precondition: Closing, ingress detached, responder not attached.
  [[nodiscard]] bool reapable() const noexcept {
    return phase_ == Phase::Closing && ingress_detached_ &&
           responder_state_ != ResponderState::Attached;
  }

private:
  friend class ExchangePool;
  void bind(Handle self, ReadyList &ready) noexcept;
  void harvest() noexcept;
  void raise(uint16_t e) noexcept;
  [[nodiscard]] bool gate(StreamId s) noexcept;
  [[nodiscard]] HeadStatus
  publish(ResponseMeta meta, std::span<const ResponseField> fields = {}) noexcept;
  [[nodiscard]] BodyStream &mut(StreamId s) noexcept {
    return s == StreamId::Req ? req_ : res_;
  }

  BodyStream req_;
  BodyStream res_;
  ReadyList *ready_{nullptr};
  Handle self_{};
  SideRef ingress_;
  SideRef responder_{};
  RequestMeta req_meta_;
  ResponseMeta res_meta_{};
  std::span<const ResponseField> response_fields_;
  std::span<const ResponseField> response_trailers_;
  uint16_t ing_events_{0};
  uint16_t resp_events_{0};
  uint16_t interim_{0};
  Phase phase_{Phase::Routing};
  ResponderState responder_state_{ResponderState::None};
  bool ingress_detached_{false};
  bool head_published_{false};
  bool listed_{false};
  bool req_abort_noted_{false};
  bool res_abort_noted_{false};
};

} // namespace Coring5
