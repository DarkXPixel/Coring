#pragma once

// Sans-IO handlers: Respond, Redirect, StaticFile and ReverseProxy.
// Respond/Redirect are built on the cold path. All handlers publish their head
// on an Exchange and produce body bytes through the credit protocol without
// allocating. StaticFile prepares its response from caller-supplied metadata.
//
// Headers are exposed as views for the session's head serializer
// (Exchange stores no head bytes yet). The serializer owns framing headers.
// Static handlers reject hop-by-hop fields and ReverseProxy strips them.

#include "coring5/base/Result.hpp"
#include "coring5/http/Exchange.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Coring5 {

struct StaticHeader {
  std::string name;
  std::string value;
};

enum class ApplyStatus : uint8_t {
  Done,    // head published and body fully pushed + finished
  Blocked, // wait for ResWritable, then call apply again with the same cursor
};

// Resume state for a single handler response. Default-constructed = start.
struct ApplyCursor {
  uint64_t sent{0};
  bool head_done{false};
};

inline constexpr std::size_t kMaxStaticHeaders = 32;
inline constexpr std::size_t kMaxStaticHeaderBytes = 8192;
inline constexpr std::size_t kMaxStaticBodyBytes = 1u << 20;
inline constexpr std::size_t kMaxLocationBytes = 8192;

[[nodiscard]] bool valid_static_header_name(std::string_view n) noexcept;
[[nodiscard]] bool valid_static_header_value(std::string_view v) noexcept;
// Forbidden: framing + hop-by-hop headers (case-insensitive).
[[nodiscard]] bool forbidden_static_header(std::string_view n) noexcept;

class Respond {
public:
  // Validates everything; status in [200,599] (no 1xx); 204/304 need empty
  // body.
  [[nodiscard]] static Result<Respond>
  make(uint16_t status, std::vector<StaticHeader> headers, std::string body);

  [[nodiscard]] uint16_t status() const noexcept { return status_; }
  [[nodiscard]] std::span<const StaticHeader> headers() const noexcept {
    return headers_;
  }
  [[nodiscard]] std::string_view body() const noexcept { return body_; }

  // Routing/Active -> publishes final head, pushes body, finishes.
  // Errors: WrongPhase/Gone/aborted stream -> Errc::Canceled.
  [[nodiscard]] Result<ApplyStatus> apply(Exchange &ex,
                                          ApplyCursor &cur) const noexcept;
  // Apply prevalidated configuration bytes without constructing an owning
  // handler on the data path. Fields follow Exchange::publish_final's lifetime.
  [[nodiscard]] static Result<ApplyStatus>
  apply(Exchange &ex, ApplyCursor &cur, uint16_t status, std::string_view body,
        std::span<const ResponseField> fields = {}) noexcept;

private:
  friend class Redirect;

  uint16_t status_{0};
  std::vector<StaticHeader> headers_;
  std::string body_;
};

class Redirect {
public:
  [[nodiscard]] static constexpr bool valid_status(uint16_t s) noexcept {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
  }
  // Location: non-empty, <= kMaxLocationBytes, only visible ASCII (no SP,
  // CTL, DEL, or >= 0x80: callers must percent-encode).
  [[nodiscard]] static bool valid_location(std::string_view loc) noexcept;

  [[nodiscard]] static Result<Redirect> make(uint16_t status,
                                             std::string_view location);

  [[nodiscard]] uint16_t status() const noexcept { return resp_.status(); }
  [[nodiscard]] std::string_view location() const noexcept {
    return resp_.headers_.front().value;
  }
  [[nodiscard]] const Respond &response() const noexcept { return resp_; }
  [[nodiscard]] std::span<const StaticHeader> headers() const noexcept {
    return resp_.headers();
  }
  [[nodiscard]] Result<ApplyStatus> apply(Exchange &ex,
                                          ApplyCursor &cur) const noexcept {
    return resp_.apply(ex, cur);
  }
  // location is a prevalidated Location field in responder-owned storage.
  [[nodiscard]] static Result<ApplyStatus>
  apply(Exchange &ex, ApplyCursor &cur, uint16_t status,
        const ResponseField &location) noexcept {
    return Respond::apply(ex, cur, status, {}, std::span{&location, 1});
  }

private:
  explicit Redirect(Respond r) : resp_(std::move(r)) {}
  Respond resp_;
};

// Metadata snapshot of the same opened regular file used to satisfy reads.
struct StaticFileMetadata {
  struct Timestamp {
    int64_t seconds{0};
    uint32_t nanoseconds{0};
  };

  uint64_t size{0};
  uint64_t inode{0};
  uint32_t device_major{0};
  uint32_t device_minor{0};
  Timestamp modified{};
  Timestamp changed{};
};

struct StaticFileConditions {
  std::optional<std::string_view> if_none_match{};
  std::optional<std::string_view> range{};
  // Any If-Range requires a strong validator, which this handler lacks.
  bool has_if_range{false};
  std::optional<std::string_view> if_modified_since{};
};

struct StaticFileStep {
  enum class Status : uint8_t { Done, Blocked, Read };

  Status status;
  uint64_t offset{0}; // Read: offset in the opened file
  uint32_t size{0};   // Read: maximum completion size, already claimed
};

class StaticFile {
public:
  // For GET/HEAD. A matching If-None-Match takes precedence over Range.
  // Supports one bytes range; ignores unknown units and multipart requests.
  [[nodiscard]] static StaticFile
  prepare(const StaticFileMetadata &file, bool head_only,
          StaticFileConditions conditions = {}) noexcept;

  [[nodiscard]] uint16_t status() const noexcept { return status_; }
  // Header values for the session serializer. Content-Length is in ResponseMeta.
  [[nodiscard]] std::string_view etag() const noexcept {
    return {etag_.data(), etag_size_};
  }
  [[nodiscard]] std::string_view last_modified() const noexcept {
    return last_modified_.front() == '\0' ? std::string_view{}
                                         : std::string_view(last_modified_.data(), 29);
  }
  [[nodiscard]] static constexpr std::string_view accept_ranges() noexcept {
    return "bytes";
  }
  [[nodiscard]] std::string_view content_range() const noexcept {
    return {content_range_.data(), content_range_size_};
  }

  // Call again after ResWritable (Blocked) or on_read (Read). The caller owns
  // the file and the I/O buffer and keeps both alive until read completion.
  // read_capacity is the size of that buffer and must be nonzero for a body.
  [[nodiscard]] Result<StaticFileStep>
  apply(Exchange &ex, ApplyCursor &cur, uint32_t read_capacity,
        std::span<const ResponseField> fields = {}) const noexcept;
  // Completes one Read. Report read and submission failures here to abort
  // the stream and return credit. A short, positive read makes progress.
  // EOF before the declared length aborts the stream.
  [[nodiscard]] Result<>
  on_read(Exchange &ex, ApplyCursor &cur,
          Result<std::span<const std::byte>> completion) const noexcept;

private:
  StaticFile() = default;

  uint64_t offset_{0};
  uint64_t length_{0};
  // Quoted weak tag: four 64-bit and four 32-bit hex fields, plus separators.
  std::array<char, 112> etag_{};
  // "bytes " plus three uint64_t decimal fields and two separators.
  std::array<char, 68> content_range_{};
  std::array<char, 30> last_modified_{};
  std::size_t etag_size_{0};
  std::size_t content_range_size_{0};
  uint16_t status_{200};
  bool head_only_{false};
};

using ProxyHeader = ResponseField;

struct ProxyForwarding {
  std::string_view client_address; // numeric address of the downstream peer
  std::string_view original_host;  // validated request authority, if present
  bool tls{false};
};

struct ProxyBodyStep {
  enum class Status : uint8_t { Done, Blocked, Ready };

  Status status;
  uint32_t size{0}; // Ready: request bytes staged or response credit granted
};

enum class ProxyRetry : uint8_t { IfUnsent, Disallow };

// One instance per Exchange, driven by the egress adapter. The adapter owns
// upstream selection, HTTP framing, I/O buffers and deadlines across attempts.
class ReverseProxy {
public:
  explicit ReverseProxy(UpstreamId upstream, uint8_t retries = 1) noexcept
      : upstream_(upstream), retries_left_(retries) {}

  [[nodiscard]] UpstreamId upstream() const noexcept { return upstream_; }

  // Disjoint, caller-owned input and output. Bound the input head at decode
  // time. Views borrow input bytes and trusted forwarding metadata, which
  // replaces incoming Forwarded and X-Forwarded-* fields. The adapter derives
  // wire framing from Exchange metadata.
  [[nodiscard]] static Result<std::span<const ProxyHeader>>
  request_headers(std::span<const ProxyHeader> input,
                  ProxyForwarding forwarding,
                  std::span<ProxyHeader> output) noexcept;

  [[nodiscard]] Result<> start(Exchange &ex, SideRef responder,
                               std::string_view method) noexcept;

  // Stage bytes in an adapter-owned send buffer, so cancellation can return
  // Exchange chunks even with a send in flight. On a short completion only
  // the sent bytes are consumed. Blocked waits for ReqReadable.
  [[nodiscard]] Result<ProxyBodyStep>
  prepare_request_body(Exchange &ex, std::span<std::byte> buffer) noexcept;
  [[nodiscard]] Result<> on_request_sent(Exchange &ex, uint32_t bytes) noexcept;

  [[nodiscard]] HeadStatus on_interim(Exchange &ex, uint16_t status) noexcept;
  // Keep the backing bytes and filtered views alive until ingress
  // serialization. A final head stops any outstanding request upload.
  [[nodiscard]] Result<std::span<const ProxyHeader>>
  on_response_head(Exchange &ex, ApplyCursor &cur, ResponseMeta meta,
                   std::span<const ProxyHeader> input,
                   std::span<ProxyHeader> output, bool tunnel = false) noexcept;

  // After on_response_head, claim body credit before reading or decoding
  // upstream data. Blocked waits for ResWritable or a pending read completion.
  [[nodiscard]] Result<ProxyBodyStep>
  apply(Exchange &ex, const ApplyCursor &cur,
        uint32_t read_capacity) const noexcept;
  // Complete one granted read. An empty non-final result releases credit
  // after a framing-only read. end means the decoder saw the message end.
  [[nodiscard]] Result<> on_read(Exchange &ex, ApplyCursor &cur,
                                 Result<std::span<const std::byte>> completion,
                                 bool end,
                                 std::span<const ResponseField> trailers = {}) const noexcept;

  // true authorizes another attempt with the same responder and Exchange.
  // Use IfUnsent only if no upstream request bytes were written, all old
  // I/O is settled, and the adapter's overall deadline allows another try.
  // A terminal failure synthesizes a head or aborts the open response.
  [[nodiscard]] bool on_failure(Exchange &ex, Errc why,
                                ProxyRetry retry) noexcept;

private:
  UpstreamId upstream_;
  uint32_t request_staged_{0};
  uint8_t retries_left_;
  bool idempotent_{false};
  bool saw_interim_{false};
};

} // namespace Coring5
