#include "coring5/proto/http1/ResponseWriter.hpp"
#include <algorithm>
#include <cstring>

namespace Coring5::Http1 {

namespace {
constexpr bool is_tchar(unsigned char c) noexcept {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z')) {
    return true;
  }
  switch (c) {
  case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
  case '+': case '-': case '.': case '^': case '_': case '`': case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

// Appends to out[pos..); false on overflow.
bool append(std::span<char> out, std::size_t &pos, std::string_view s) noexcept {
  if (s.size() > out.size() - pos) {
    return false;
  }
  if (!s.empty()) {
    std::memcpy(out.data() + pos, s.data(), s.size());
  }
  pos += s.size();
  return true;
}

std::size_t dec(char *end, uint64_t v) noexcept {
  std::size_t n = 0;
  do {
    *--end = static_cast<char>('0' + v % 10);
    v /= 10;
    ++n;
  } while (v != 0);
  return n;
}
} // namespace

std::string_view reason_phrase(uint16_t s) noexcept {
  switch (s) {
  case 100: return "Continue";
  case 101: return "Switching Protocols";
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 206: return "Partial Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 303: return "See Other";
  case 304: return "Not Modified";
  case 307: return "Temporary Redirect";
  case 308: return "Permanent Redirect";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 408: return "Request Timeout";
  case 411: return "Length Required";
  case 413: return "Content Too Large";
  case 414: return "URI Too Long";
  case 416: return "Range Not Satisfiable";
  case 417: return "Expectation Failed";
  case 421: return "Misdirected Request";
  case 426: return "Upgrade Required";
  case 429: return "Too Many Requests";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 505: return "HTTP Version Not Supported";
  default: return "";
  }
}

bool is_valid_field_name(std::string_view n) noexcept {
  return !n.empty() && std::ranges::all_of(n, [](char c) {
           return is_tchar(static_cast<unsigned char>(c));
         });
}

bool is_valid_field_value(std::string_view v) noexcept {
  return std::ranges::all_of(v, [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c == '\t' || (c >= 0x20 && c != 0x7F);
  });
}

Errc ResponseWriter::put(std::string_view s) noexcept {
  if (err_ != Errc::Ok) {
    return err_;
  }
  if (!append(out_, len_, s)) {
    return fail(Errc::TooLarge);
  }
  return Errc::Ok;
}

Errc ResponseWriter::status_line(uint16_t status, uint8_t minor,
                                 std::string_view reason) noexcept {
  if (err_ != Errc::Ok) {
    return err_;
  }
  if (status < 100 || status > 599 || minor > 1 || len_ != 0 ||
      !is_valid_field_value(reason)) {
    return fail(Errc::InvalidArgument);
  }
  if (reason.empty()) {
    reason = reason_phrase(status);
  }
  const char code[3] = {static_cast<char>('0' + status / 100),
                        static_cast<char>('0' + status / 10 % 10),
                        static_cast<char>('0' + status % 10)};
  put(minor == 1 ? "HTTP/1.1 " : "HTTP/1.0 ");
  put({code, 3});
  put(" ");
  put(reason);
  return put("\r\n");
}

Errc ResponseWriter::header(std::string_view name,
                            std::string_view value) noexcept {
  if (err_ != Errc::Ok) {
    return err_;
  }
  if (len_ == 0 || !is_valid_field_name(name) || !is_valid_field_value(value)) {
    return fail(Errc::InvalidArgument);
  }
  put(name);
  put(": ");
  put(value);
  return put("\r\n");
}

Errc ResponseWriter::content_length(uint64_t n) noexcept {
  char tmp[20];
  const std::size_t k = dec(tmp + sizeof tmp, n);
  return header("Content-Length", {tmp + sizeof tmp - k, k});
}

Errc ResponseWriter::end_head() noexcept {
  if (len_ == 0 && err_ == Errc::Ok) {
    return fail(Errc::InvalidArgument);
  }
  return put("\r\n");
}

std::size_t encode_chunk_header(std::span<char> out, uint64_t n) noexcept {
  if (n == 0) {
    return 0;
  }
  char tmp[16];
  std::size_t k = 0;
  for (uint64_t v = n; v != 0; v >>= 4) {
    tmp[k++] = "0123456789abcdef"[v & 0xF];
  }
  if (out.size() < k + 2) {
    return 0;
  }
  for (std::size_t i = 0; i < k; ++i) {
    out[i] = tmp[k - 1 - i];
  }
  out[k] = '\r';
  out[k + 1] = '\n';
  return k + 2;
}

Result<std::size_t> write_last_chunk(std::span<char> out,
                                     std::span<const FieldView> trailers) noexcept {
  std::size_t pos = 0;
  bool ok = append(out, pos, "0\r\n");
  for (const FieldView &f : trailers) {
    if (!is_valid_field_name(f.name) || !is_valid_field_value(f.value)) {
      return std::unexpected(Error{Errc::InvalidArgument, 0});
    }
    ok = ok && append(out, pos, f.name) && append(out, pos, ": ") &&
         append(out, pos, f.value) && append(out, pos, "\r\n");
  }
  ok = ok && append(out, pos, "\r\n");
  if (!ok) {
    return std::unexpected(Error{Errc::TooLarge, 0});
  }
  return pos;
}

} // namespace Coring5::Http1
