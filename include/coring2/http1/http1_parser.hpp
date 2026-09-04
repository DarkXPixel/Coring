#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>
namespace Coring2 {
class Http1Parser {
public:
  enum class HttpMethod { GET, POST, PUT, DELETE, HEAD, OPTIONS, UNKNOWN };
  enum class ParseError {
    Incomplete,
    InvalidRequestLine,
    InvalidHeader,
    InvalidContentLenght,
  };

  struct HttpHeader {
    std::string_view name;
    std::string_view value;
  };

  enum class BodyType {
    None,
    ContentLength,
    Chunked,
  };

  struct HttpRequestHead {
    HttpMethod method{HttpMethod::UNKNOWN};
    std::string_view path;
    std::string_view version;
    std::array<HttpHeader, 32> headers;
    BodyType body_type{BodyType::None};

    std::size_t content_length{0};
    uint8_t header_counter{0};

    std::optional<std::string_view> get(std::string_view name) const {
      for (const auto &[h_name, h_value] : headers) {
        if (std::ranges::equal(h_name, name, [](char a, char b) {
              return std::tolower(a) == std::tolower(b);
            })) {
          return h_value;
        }
      }
      return std::nullopt;
    }
  };

  struct ParseResult {
    HttpRequestHead request;
    std::size_t header_size;
  };

  static std::expected<ParseResult, ParseError> parse(std::string_view buffer) {
    std::size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
      return std::unexpected(ParseError::Incomplete);
    }

    HttpRequestHead head;
    std::size_t cursor = 0;

    std::size_t line_end = buffer.find("\r\n", cursor);
    if (!parse_request_line(buffer.substr(cursor, line_end - cursor), head)) {
      return std::unexpected(ParseError::InvalidRequestLine);
    }

    cursor = line_end + 2;

    while (cursor < header_end) {
      line_end = buffer.find("\r\n", cursor);
      std::string_view header_line = buffer.substr(cursor, line_end - cursor);
      if (!parse_header(header_line, head)) {
        return std::unexpected(ParseError::InvalidHeader);
      }
      cursor = line_end + 2;
    }

    auto cl = head.get("Content-Length");
    auto te = head.get("Transfer-Encoding");

    if (cl && te) {
      return std::unexpected(ParseError::InvalidHeader);
    }

    if (cl) {
      head.body_type = BodyType::ContentLength;
      if (cl->empty()) {
        return std::unexpected(ParseError::InvalidContentLenght);
      }
      auto [ptr, ec] = std::from_chars(cl->data(), cl->data() + cl->size(),
                                       head.content_length);
      if (ec != std::errc{} || ptr != cl->data() + cl->size()) {
        return std::unexpected(ParseError::InvalidContentLenght);
      }
    } else if (auto te = head.get("Transfer-Encoding");
               te && te->contains("chunked")) {
      head.body_type = BodyType::Chunked;
    }
    return ParseResult{.request = head, .header_size = header_end + 4};
  }

private:
  static bool parse_request_line(std::string_view line, HttpRequestHead &head) {
    auto sp1 = line.find(' ');
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
      return false;
    }

    head.method = string_to_http_method(line.substr(0, sp1));
    head.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    head.version = line.substr(sp2 + 1);
    return !head.path.empty() && head.version.starts_with("HTTP/");
  }

  constexpr static HttpMethod string_to_http_method(std::string_view method) {
    if (method == "GET")
      return HttpMethod::GET;
    if (method == "POST")
      return HttpMethod::POST;
    if (method == "PUT")
      return HttpMethod::PUT;
    if (method == "DELETE")
      return HttpMethod::DELETE;
    if (method == "HEAD")
      return HttpMethod::HEAD;
    if (method == "OPTIONS")
      return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
  }

  static bool parse_header(std::string_view line, HttpRequestHead &head) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      return false;
    }

    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);

    auto trim = [](std::string_view v) {
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.remove_suffix(1);
      return v;
    };

    name = trim(name);
    value = trim(value);
    if (name.empty()) {
      return false;
    }

    head.headers[head.header_counter++] = {.name = name, .value = value};
    return true;
  }
};
} // namespace Coring2