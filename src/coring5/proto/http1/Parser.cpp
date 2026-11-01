#include "coring5/proto/http1/Parser.hpp"

#include <algorithm>
#include <charconv>

namespace Coring5::Http1 {

namespace {

constexpr bool is_tchar(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z')) {
    return true;
  }
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

constexpr char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim_ows(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
    v.remove_prefix(1);
  }
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
    v.remove_suffix(1);
  }
  return v;
}

HeadResult fail(ParseError e) { return HeadResult{ParseStatus::Error, e, 0}; }

} // namespace

std::string_view RequestHead::find(std::string_view lower_name) const noexcept {
  for (std::size_t i = 0; i < header_count; ++i) {
    if (iequals(headers[i].name, lower_name)) {
      return headers[i].value;
    }
  }
  return {};
}

HeadResult parse_head(std::span<const char> in, const Limits &limits,
                      RequestHead &out) {
  const std::string_view buf(in.data(), in.size());
  out = RequestHead{};

  // Reject early on anything that can never become valid, even before the head
  // is complete: bare LF, NUL, other control characters. Only head bytes are
  // scanned (never a body following the terminator), so the verdict does not
  // depend on how the stream was segmented.
  const std::size_t term = buf.find("\r\n\r\n");
  const std::size_t scan = std::min<std::size_t>(
      term == std::string_view::npos ? buf.size() : term + 4,
      limits.max_head_bytes);
  for (std::size_t i = 0; i < scan; ++i) {
    const auto c = static_cast<unsigned char>(buf[i]);
    if (c == '\n' && (i == 0 || buf[i - 1] != '\r')) {
      return fail(ParseError::BareLf);
    }
    if (c == '\0' || (c < 0x20 && c != '\r' && c != '\n' && c != '\t')) {
      return fail(ParseError::BadHeader);
    }
  }

  const std::size_t end = buf.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (buf.size() >= limits.max_head_bytes) {
      return fail(ParseError::HeadTooLarge);
    }
    return HeadResult{};
  }
  const std::size_t head_len = end + 4;
  if (head_len > limits.max_head_bytes) {
    return fail(ParseError::HeadTooLarge);
  }

  std::string_view head = buf.substr(0, end + 2); // keep the last CRLF
  // ---- request line ----
  const std::size_t rl_end = head.find("\r\n");
  const std::string_view line = head.substr(0, rl_end);
  head.remove_prefix(rl_end + 2);

  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 =
      sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
      line.find(' ', sp2 + 1) != std::string_view::npos) {
    return fail(ParseError::BadRequestLine);
  }
  out.method = line.substr(0, sp1);
  out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = line.substr(sp2 + 1);

  if (out.method.empty() || !std::ranges::all_of(out.method, [](char c) {
        return is_tchar(static_cast<unsigned char>(c));
      })) {
    return fail(ParseError::BadMethod);
  }
  if (out.target.empty()) {
    return fail(ParseError::BadRequestLine);
  }
  if (out.target.size() > limits.max_uri_bytes) {
    return fail(ParseError::UriTooLong);
  }
  if (version == "HTTP/1.1") {
    out.minor_version = 1;
  } else if (version == "HTTP/1.0") {
    out.minor_version = 0;
    out.keep_alive = false;
  } else {
    return fail(ParseError::BadVersion);
  }

  // ---- headers ----
  const std::size_t max_headers =
      std::min<std::size_t>(limits.max_headers, kMaxHeadersHard);
  while (!head.empty()) {
    const std::size_t eol = head.find("\r\n");
    const std::string_view h = head.substr(0, eol);
    head.remove_prefix(eol + 2);

    if (h.empty()) {
      break;
    }
    if (h.front() == ' ' || h.front() == '\t') {
      return fail(ParseError::ObsFold);
    }
    const std::size_t colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return fail(ParseError::BadHeader);
    }
    const std::string_view name = h.substr(0, colon);
    if (!std::ranges::all_of(name, [](char c) {
          return is_tchar(static_cast<unsigned char>(c));
        })) {
      return fail(ParseError::BadHeaderName); // includes whitespace before ':'
    }
    const std::string_view value = trim_ows(h.substr(colon + 1));

    if (out.header_count >= max_headers) {
      return fail(ParseError::TooManyHeaders);
    }
    out.headers[out.header_count++] = Header{name, value};

    if (iequals(name, "content-length")) {
      uint64_t n = 0;
      const auto [p, ec] =
          std::from_chars(value.data(), value.data() + value.size(), n);
      if (value.empty() || ec != std::errc{} ||
          p != value.data() + value.size()) {
        return fail(ParseError::BadContentLength);
      }
      if (out.has_content_length && out.content_length != n) {
        return fail(ParseError::ConflictingLength);
      }
      out.has_content_length = true;
      out.content_length = n;
    } else if (iequals(name, "transfer-encoding")) {
      if (!iequals(value, "chunked") || out.chunked) {
        return fail(ParseError::BadTransferEncoding);
      }
      out.chunked = true;
    } else if (iequals(name, "host")) {
      out.host = value;
    } else if (iequals(name, "connection")) {
      if (iequals(value, "close")) {
        out.keep_alive = false;
      } else if (iequals(value, "keep-alive")) {
        out.keep_alive = true;
      }
    }
  }

  if (out.chunked && out.has_content_length) {
    return fail(ParseError::ConflictingLength); // classic smuggling vector
  }
  // TODO(M4): body framing for requests (chunked decoder state machine).
  return HeadResult{ParseStatus::Done, ParseError::None, head_len};
}

} // namespace Coring5::Http1
