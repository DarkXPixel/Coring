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

constexpr bool is_token(std::string_view v) {
  if (v.empty()) {
    return false;
  }
  for (const char c : v) {
    if (!is_tchar(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// Request-target bytes: visible ASCII only. Controls, SP and DEL are rejected
// earlier; bytes >= 0x80 are only legal inside header values.
constexpr bool is_target_char(unsigned char c) { return c > 0x20 && c < 0x7F; }

} // namespace

bool Header::named(std::string_view key) const noexcept { return iequals(name, key); }

std::string_view RequestHead::find(std::string_view lower_name) const noexcept {
  for (std::size_t i = 0; i < header_count; ++i) {
    if (headers[i].named(lower_name)) {
      return headers[i].value;
    }
  }
  return {};
}

namespace {
ParseError request_line(std::string_view line, const Limits &limits,
                        RequestHead &out) {
  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 =
      sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
      line.find(' ', sp2 + 1) != std::string_view::npos) {
    return ParseError::BadRequestLine;
  }
  out.method = line.substr(0, sp1);
  out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = line.substr(sp2 + 1);

  if (out.method.empty() || !std::ranges::all_of(out.method, [](char c) {
        return is_tchar(static_cast<unsigned char>(c));
      })) {
    return ParseError::BadMethod;
  }
  if (out.target.empty()) {
    return ParseError::BadRequestLine;
  }
  if (out.target.size() > limits.max_uri_bytes) {
    return ParseError::UriTooLong;
  }
  if (!std::ranges::all_of(out.target, [](char c) {
        return is_target_char(static_cast<unsigned char>(c));
      })) {
    return ParseError::BadRequestLine;
  }
  if (version == "HTTP/1.1") {
    out.minor_version = 1;
  } else if (version == "HTTP/1.0") {
    out.minor_version = 0;
  } else {
    return ParseError::BadVersion;
  }

  return ParseError::None;
}

ParseError status_line(std::string_view line, RequestHead &out,
                       uint16_t &status) {
  if (line.size() < 13 ||
      (!line.starts_with("HTTP/1.1 ") && !line.starts_with("HTTP/1.0 ")) ||
      line[12] != ' ') {
    return ParseError::BadVersion;
  }
  const auto [end, ec] =
      std::from_chars(line.data() + 9, line.data() + 12, status);
  if (ec != std::errc{} || end != line.data() + 12 || status < 100 ||
      status > 599) {
    return ParseError::BadRequestLine;
  }
  out.minor_version = static_cast<uint8_t>(line[7] - '0');
  return ParseError::None;
}
HeadResult parse(std::span<const char> in, const Limits &limits,
                      RequestHead &out, uint16_t *status) {
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
    // A CR must be the first half of CRLF. The check looks only at bytes inside
    // the scanned window, so the verdict never depends on segmentation: a CR
    // that ends the window is judged once its successor arrives.
    if (c == '\r' && i + 1 < scan && buf[i + 1] != '\n') {
      return fail(ParseError::BareCr);
    }
    if (c == '\0' || c == 0x7F ||
        (c < 0x20 && c != '\r' && c != '\n' && c != '\t')) {
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

  const auto error = status ? status_line(line, out, *status)
                            : request_line(line, limits, out);
  if (error != ParseError::None) return fail(error);
  // ---- headers ----
  const std::size_t max_headers =
      std::min<std::size_t>(limits.max_headers, kMaxHeadersHard);
  bool seen_host = false;
  bool saw_close = false;
  bool saw_keep_alive = false;
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
      // Two Host headers are a routing-confusion vector between the proxy and
      // the upstream: reject even when the values are identical.
      if (seen_host) {
        return fail(ParseError::DuplicateHost);
      }
      seen_host = true;
      out.host = value;
    } else if (iequals(name, "connection")) {
      // RFC 9110 7.6.1: a comma-separated list of tokens, possibly spread over
      // several header lines. Empty list elements are tolerated.
      std::string_view rest = value;
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view tok = trim_ows(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{}
                                               : rest.substr(comma + 1);
        if (tok.empty()) {
          continue;
        }
        if (!is_token(tok)) {
          return fail(ParseError::BadHeaderValue);
        }
        if (out.connection_token_count >= kMaxConnectionTokens) {
          return fail(ParseError::TooManyHeaders);
        }
        out.connection_tokens[out.connection_token_count++] = tok;
        if (iequals(tok, "close")) {
          saw_close = true;
        } else if (iequals(tok, "keep-alive")) {
          saw_keep_alive = true;
        } else if (iequals(tok, "upgrade")) {
          out.upgrade = true;
        }
      }
    }
  }

  if (out.chunked && out.has_content_length) {
    return fail(ParseError::ConflictingLength); // classic smuggling vector
  }
  if (out.chunked && out.minor_version == 0) {
    return fail(ParseError::BadTransferEncoding); // TE is HTTP/1.1 only
  }
  if (!status && out.minor_version == 1 && out.host.empty()) {
    return fail(ParseError::MissingHost); // RFC 9112 3.2: exactly one Host
  }
  // `close` always wins; HTTP/1.0 persists only on an explicit keep-alive.
  out.keep_alive = !saw_close && (out.minor_version == 1 || saw_keep_alive);
  return HeadResult{ParseStatus::Done, ParseError::None, head_len};
}

} // namespace

HeadResult parse_head(std::span<const char> in, const Limits &limits,
                      RequestHead &out) {
  return parse(in, limits, out, nullptr);
}
HeadResult parse_response_head(std::span<const char> in, const Limits &limits,
                               RequestHead &out, uint16_t &status) {
  return parse(in, limits, out, &status);
}

} // namespace Coring5::Http1
