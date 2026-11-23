#pragma once

// HTTP/1.1 response serializer (sans-IO, allocation-free, noexcept). Writes
// into a caller-provided bounded buffer; nothing is allocated or copied
// beyond that. Strict: names must be tokens, values must not contain CTLs
// (CR/LF/NUL => rejected, never sanitised), so response splitting is
// impossible. Overflow => Errc::TooLarge and the writer is poisoned.
//
//   ResponseWriter w(buf);
//   w.status_line(200, 1);              // "HTTP/1.1 200 OK\r\n"
//   w.header("Content-Type", "text/plain");
//   w.content_length(5)  /  w.chunked();
//   w.end_head();                       // "\r\n"
//   n = w.size(); send buf[0..n)
//
// Chunked body: encode_chunk_header(out, n) -> "<hex>\r\n"; the caller sends
// the payload (zero copy, separate iovec) followed by chunk_crlf(); the
// terminator is `write_last_chunk(out, trailers)`.

#include "coring5/base/Result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Coring5::Http1 {

[[nodiscard]] std::string_view reason_phrase(uint16_t status) noexcept;

[[nodiscard]] bool is_valid_field_name(std::string_view n) noexcept;
[[nodiscard]] bool is_valid_field_value(std::string_view v) noexcept;

struct FieldView {
  std::string_view name;
  std::string_view value;
};

class ResponseWriter {
public:
  explicit ResponseWriter(std::span<char> out) noexcept : out_(out) {}

  // `minor` is 0 or 1; status in [100, 599].
  Errc status_line(uint16_t status, uint8_t minor = 1,
                   std::string_view reason = {}) noexcept;
  Errc request_line(std::string_view method, std::string_view target, uint8_t minor) noexcept;
  Errc header(std::string_view name, std::string_view value) noexcept;
  Errc content_length(uint64_t n) noexcept;
  Errc chunked() noexcept { return header("Transfer-Encoding", "chunked"); }
  Errc end_head() noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return len_; }
  [[nodiscard]] Errc error() const noexcept { return err_; }
  [[nodiscard]] std::string_view view() const noexcept {
    return {out_.data(), len_};
  }

private:
  Errc put(std::string_view s) noexcept;
  Errc fail(Errc e) noexcept {
    if (err_ == Errc::Ok) {
      err_ = e;
    }
    return err_;
  }

  std::span<char> out_;
  std::size_t len_{0};
  Errc err_{Errc::Ok};
};

// Max bytes of a chunk-size line: 16 hex digits + CRLF.
inline constexpr std::size_t kChunkHeaderMax = 18;
inline constexpr std::string_view kChunkCrlf = "\r\n";

// Writes "<lowercase hex>\r\n" into `out`; returns bytes written, 0 if `n`==0
// (use write_last_chunk) or `out` is too small.
[[nodiscard]] std::size_t encode_chunk_header(std::span<char> out,
                                              uint64_t n) noexcept;

// Writes "0\r\n" + trailer fields + "\r\n". Returns bytes written or error.
[[nodiscard]] Result<std::size_t>
write_last_chunk(std::span<char> out,
                 std::span<const FieldView> trailers = {}) noexcept;

} // namespace Coring5::Http1
