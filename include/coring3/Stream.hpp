#pragma once

#include "coring3/SessionStorage.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <variant>
namespace Coring3 {
enum class StreamState : uint8_t {
  Idle,
  ReadingHeaders,
  WaitingUpstreamConnection,
  ProxyingBody,
  FlushingToClient,
  Closed
};

struct StringSplice {
  uint16_t offset{0};
  uint16_t length{0};

  [[nodiscard]] bool empty() const noexcept { return length == 0; }
};

struct HeaderSplice {
  StringSplice name;
  StringSplice value;
};

class HttpRequestMeta {
  StringSplice method_;
  StringSplice path_;
  int64_t content_length_{-1};

  uint16_t arena_used_{0};
  uint8_t headers_count_{0};
  uint8_t http_version_{1};

  bool keep_alive_{true};
  uint8_t reserved_flags_{0};

  std::array<HeaderSplice, 32> headers_;
  alignas(64) std::array<std::byte, 8192> arena_buf_;

  // alignas(8) std::array<HttpHeader, 32> headers_;
  // std::string_view method_;
  // std::string_view path_;
  // int64_t content_length_{0};
  // uint16_t arena_used_{0};

  // uint8_t headers_count_{0};
  // uint8_t http_version_{1};

  // bool keep_alive_{true};
  // alignas(64) std::array<std::byte, 8196> arena_buf_;

public:
  HttpRequestMeta() = default;

  void reset() noexcept {
    arena_used_ = 0;
    headers_count_ = 0;
    method_ = {};
    path_ = {};
    content_length_ = -1;
    http_version_ = 1;
    keep_alive_ = true;
  }

  bool set_method(std::string_view m) noexcept {
    auto slice = push_to_arena(m);
    if (!slice) {
      return false;
    }
    method_ = *slice;
    return true;
  }

  bool set_path(std::string_view p) noexcept {
    auto slice = push_to_arena(p);
    if (!slice) {
      return false;
    }
    path_ = *slice;
    return true;
  }

  bool add_header(std::string_view name, std::string_view value) noexcept {
    if (headers_count_ >= headers_.size()) {
      return false;
    }

    auto name_slice = push_to_arena(name);
    auto value_slice = push_to_arena(value);

    if (!name_slice || !value_slice) {
      return false;
    }

    headers_[headers_count_++] =
        HeaderSplice{.name = *name_slice, .value = *value_slice};
    return true;
  }

private:
  std::optional<StringSplice> push_to_arena(std::string_view sv) noexcept {
    if (sv.empty()) {
      return StringSplice{.offset = 0, .length = 0};
    }

    if (arena_used_ + sv.size() > arena_buf_.size()) {
      return std::nullopt;
    }

    auto off = arena_used_;
    auto len = static_cast<uint16_t>(sv.size());
    std::memcpy(arena_buf_.data() + off, sv.data(), len);
    arena_used_ += len;
    return StringSplice{.offset = off, .length = len};
  }

  [[nodiscard]] std::string_view get_view(StringSplice slice) const noexcept {
    if (slice.empty()) {
      return {};
    }
    return {reinterpret_cast<const char *>(arena_buf_.data() + slice.offset),
            slice.length};
  }

  void parse_well_known_header(std::string_view name,
                               std::string_view value) noexcept {}

  static bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if ((a[i] | 0x20) != (b[i] | 0x20)) {
        return false;
      }
    }
    return true;
  }
};

struct HttpTransactionStream {
  int64_t bytes_proxied{0};
  SessionHandle self_handle;
  SessionHandle client_conn_handle;
  SessionHandle upstream_conn_handle;

  uint32_t client_stream_id{0};
  uint16_t status_code{0};
  StreamState state{StreamState::Idle}; // 1byte
  bool is_chunked{false};

  alignas(64) HttpRequestMeta request_meta;
  void reset() noexcept {
    bytes_proxied = 0;
    self_handle = SessionHandle{};
    client_conn_handle = SessionHandle{};
    upstream_conn_handle = SessionHandle{};
    client_stream_id = 0;
    status_code = 0;
    state = StreamState::Idle;
    is_chunked = false;
    request_meta.reset();
  }
};

using StreamVariant = std::variant<EmptySession, HttpTransactionStream>;
} // namespace Coring3