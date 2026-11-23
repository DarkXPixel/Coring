#pragma once

#include "coring5/handlers/StaticFileRoot.hpp"
#include "coring5/net/UpstreamDialer.hpp"
#include "coring5/proto/http1/ChunkedDecoder.hpp"
#include "coring5/proto/http1/Parser.hpp"

namespace Coring5 {

// Stable storage for kernel buffers and for fields borrowed by the serializer.
class ResponderBuffer {
public:
  explicit ResponderBuffer(ChunkPool &pool)
      : pool_(pool), slot_(pool.alloc()) {}
  ~ResponderBuffer() {
    if (slot_ != ChunkPool::kNil) {
      pool_.free(slot_);
    }
  }
  ResponderBuffer(const ResponderBuffer &) = delete;
  ResponderBuffer &operator=(const ResponderBuffer &) = delete;
  std::span<char> bytes() const noexcept {
    if (slot_ == ChunkPool::kNil) {
      return {};
    }
    return {reinterpret_cast<char *>(pool_.data(slot_)), pool_.chunk_size()};
  }

private:
  ChunkPool &pool_;
  uint32_t slot_;
};

class FileResponder {
public:
  explicit FileResponder(IoContext &ctx) : buffer_(*ctx.stage) {}
  void start(IoContext &, ConnCore &, Exchange &, const Http1::RequestHead &,
             const StaticRoute &);
  void pump(IoContext &, ConnCore &, Exchange &);
  void complete(IoContext &, ConnCore &, Exchange &, const Cqe &);

private:
  void open_next(IoContext &, ConnCore &, Exchange &);
  void fail_lookup(ConnCore &, Exchange &, uint16_t status);

  ResponderBuffer buffer_;
  FdOwner directory_;
  std::shared_ptr<const StaticFileOptions> options_;
  std::optional<StaticFileLookup> lookup_;
  open_how how_ = StaticFileRoot::open_options();
  struct statx stat_{};
  StaticFileConditions conditions_{};
  std::optional<StaticFile> file_;
  ApplyCursor cursor_;
  std::array<ResponseField, 4> fields_{};
  bool reading_{};
};

class ProxyResponder {
public:
  ProxyResponder(IoContext &ctx, UpstreamId id)
      : proxy_(id), limits_(*ctx.limits), tx_(*ctx.stage), head_(*ctx.stage),
        body_(*ctx.chunks), decoder_(limits_) {}
  void start(IoContext &, ConnCore &, Exchange &, const Http1::RequestHead &);
  void pump(IoContext &, ConnCore &, Exchange &);
  void complete(IoContext &, ConnCore &, Exchange &, const Cqe &);
  void on_timeout(IoContext &ctx, ConnCore &core, Exchange &ex) {
    fail(ctx, core, ex, Errc::Timeout);
  }
  bool retiring() const noexcept {
    return (finished_ && pending()) || (cursor_.head_done && !tunnel_ && sending_);
  }
  bool pending() const noexcept { return sending_ || receiving_ || dialing_; }

private:
  void dial(IoContext &, ConnCore &, Exchange &, BackendRef first = {});
  void fail(IoContext &, ConnCore &, Exchange &, Errc);
  void upload(IoContext &, ConnCore &, Exchange &);
  void receive(IoContext &, ConnCore &, Exchange &);
  Result<> parse_heads(Exchange &, ChunkPool &stages);
  Result<> consume_body(IoContext &, Exchange &, uint32_t credit);
  [[nodiscard]] Result<std::span<const ResponseField>> response_trailers() noexcept;
  ReverseProxy proxy_;
  Limits limits_;
  UpstreamDialer dialer_;
  BackendLease backend_;
  // tx_ starts with the serialized head, then holds one upload frame until
  // every byte is acknowledged. Keep head_ stable for borrowed response fields.
  ResponderBuffer tx_, head_, body_;
  std::array<ResponseField, Http1::kMaxHeadersHard> fields_{};
  std::array<ResponseField, Http1::kMaxHeadersHard> trailer_fields_{};
  std::optional<ResponderBuffer> trailer_buffer_;
  std::array<std::string_view, Http1::kMaxConnectionTokens> connection_tokens_{};
  std::size_t connection_token_count_{};
  ApplyCursor cursor_;
  Http1::ChunkedDecoder decoder_;
  TimerNode read_timer_, write_timer_;
  TimePoint retry_deadline_{};
  // Unconsumed wire bytes in the head tail or the current body read. A
  // blocked Exchange keeps this bounded suffix until credit returns.
  std::span<const char> staged_{};
  std::span<const char> unsent_{};
  uint32_t head_used_{}, payload_{};
  uint64_t remaining_{};
  bool waiting_continue_{}, reuse_allowed_{}, reusable_{};
  bool upgrade_{}, tunnel_{}, forward_trailers_{};
  bool request_chunked_{}, response_chunked_{}, length_known_{};
  bool sending_{}, receiving_{}, dialing_{}, wrote_{}, head_sent_{},
      upload_done_{}, finished_{};
};
} // namespace Coring5
