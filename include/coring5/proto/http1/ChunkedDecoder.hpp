#pragma once

// HTTP/1.1 chunked body decoder (M4.1c). Sans-IO, allocation-free, noexcept,
// fixed-size state. Bytes in, events out; strict: rejects, never normalises.
//
// `feed(in, max_data)` consumes from `in` until it can report one event:
//   NeedMore   all of `in` was consumed (framing bytes); feed more.
//   NeedCredit chunk data is next but max_data == 0; `consumed` bytes of `in`
//              were used, call again with credit.
//   Data       `data` is a zero-copy sub-span of `in` (<= max_data bytes).
//   Done       the body incl. trailers is complete; bytes after `consumed`
//              belong to the next pipelined request.
//   Error      sticky; `error`/`reason` say why.
// `consumed` is always counted from the start of `in` (it includes any framing
// bytes skipped before a Data run). The caller advances by `consumed`.
// Trailer contents are validated, counted and discarded.

#include "coring5/base/Result.hpp"
#include "coring5/config/Snapshot.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace Coring5::Http1 {

enum class ChunkedEvent : uint8_t { NeedMore, NeedCredit, Data, Done, Error };

enum class ChunkedReason : uint8_t {
  None,
  BadChunkSize,
  BodyTooLarge,
  BadChunkExt,
  ExtTooLarge,
  BadChunkCrlf,
  BadTrailer,
  ForbiddenTrailer,
  TrailersTooLarge,
  PrematureEof,
};

struct ChunkedStep {
  ChunkedEvent event{ChunkedEvent::NeedMore};
  Errc error{Errc::Ok};
  ChunkedReason reason{ChunkedReason::None};
  std::size_t consumed{0};
  std::span<const char> data{};
};

class ChunkedDecoder {
public:
  static constexpr uint32_t kDefaultMaxExtBytes = 1024;

  ChunkedDecoder() noexcept = default;
  explicit ChunkedDecoder(const Limits &limits,
                          uint32_t max_ext_bytes = kDefaultMaxExtBytes) noexcept
      : max_trailer_bytes_(limits.max_head_bytes),
        max_trailers_(limits.max_headers), max_ext_bytes_(max_ext_bytes) {}

  [[nodiscard]] ChunkedStep feed(std::span<const char> in,
                                 std::size_t max_data) noexcept;

  [[nodiscard]] bool done() const noexcept { return st_ == St::Done; }
  [[nodiscard]] bool failed() const noexcept { return st_ == St::Failed; }
  [[nodiscard]] ChunkedReason reason() const noexcept { return reason_; }
  [[nodiscard]] uint64_t body_bytes() const noexcept { return body_bytes_; }
  [[nodiscard]] uint32_t trailers_seen() const noexcept { return trailers_; }

  // Peer closed the transport: Ok only if the body is complete.
  [[nodiscard]] Errc on_eof() noexcept;

private:
  enum class St : uint8_t {
    Size,      // hex digits
    Ext,       // after ';' expecting name
    ExtName,   // token chars of name
    ExtValue0, // after '=' expecting value start
    ExtValue,  // token value
    ExtQuoted, // inside quoted-string
    ExtEscape, // after backslash in quoted-string
    ExtQEnd,   // after closing quote: ';' or CR
    SizeLf,    // expect LF after size line
    Data,      // chunk payload
    DataCr,
    DataLf,
    TrStart, // trailer line start
    TrName,
    TrValue,
    TrLf,  // LF ending a trailer field line
    EndLf, // LF ending the trailer block
    Done,
    Failed,
  };

  static constexpr std::size_t kNameCap = 24;

  ChunkedStep fail(Errc e, ChunkedReason r, std::size_t consumed) noexcept;
  bool finish_name() noexcept;

  St st_{St::Size};
  ChunkedReason reason_{ChunkedReason::None};
  uint8_t digits_{0};
  uint8_t name_len_{0};
  bool name_overflow_{false};
  uint64_t chunk_size_{0};
  uint64_t remaining_{0};
  uint64_t body_bytes_{0};
  uint32_t ext_bytes_{0};
  uint32_t trailer_bytes_{0};
  uint32_t trailers_{0};
  uint32_t max_trailer_bytes_{16 * 1024};
  uint32_t max_trailers_{100};
  uint32_t max_ext_bytes_{kDefaultMaxExtBytes};
  char name_[kNameCap]{};
};

} // namespace Coring5::Http1
