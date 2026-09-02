#pragma once
#include "coring2/utility.hpp"
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Coring2 {
class Http1ResponseBuilder {
private:
  uint16_t status_code_{200};
  std::string_view status_message_{"OK"};
  std::vector<std::pair<std::string_view, std::string_view>> headers_;
  std::string_view body_;
  bool auto_content_length_{true};

public:
  Http1ResponseBuilder() = default;

  Http1ResponseBuilder &status(uint16_t code) {
    status_code_ = code;
    status_message_ = get_status_message(code);
    return *this;
  }

  Http1ResponseBuilder &status(HttpStatus status) {
    return this->status(static_cast<uint16_t>(status));
  }

  Http1ResponseBuilder &status(uint16_t code, std::string_view message) {
    status_code_ = code;
    status_message_ = message;
    return *this;
  }

  Http1ResponseBuilder &header(std::string_view name, std::string_view value) {
    headers_.emplace_back(name, value);
    return *this;
  }

  Http1ResponseBuilder &content_type(std::string_view type) {
    return header("Content-Type", type);
  }

  Http1ResponseBuilder &keep_alive(bool enable = true) {
    return header("Connection", enable ? "keep-alive" : "close");
  }

  Http1ResponseBuilder &body(std::string_view body_data) {
    body_ = body_data;
    return *this;
  }

  [[nodiscard]] std::string build() const {
    std::string response;
    std::size_t estimated_size = 64 + body_.size();
    for (const auto &[k, v] : headers_) {
      estimated_size += k.size() + 4;
    }

    response.reserve(estimated_size);
    std::format_to(std::back_inserter(response), "HTTP/1.1 {} {}\r\n",
                   status_code_, status_message_);
    for (const auto &[name, value] : headers_) {
      std::format_to(std::back_inserter(response), "{}: {}\r\n", name, value);
    }

    if (auto_content_length_) {
      std::format_to(std::back_inserter(response), "Content-Length: {}\r\n",
                     body_.size());
    }

    response.append("\r\n");
    if (!body_.empty()) {
      response.append(body_.data(), body_.size());
    }
    return response;
  }
};
} // namespace Coring2