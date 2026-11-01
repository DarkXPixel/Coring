#pragma once

// Request-path canonicalisation. Routing decisions MUST be made on the
// canonical form, otherwise "/admin", "/%61dmin", "/./admin" and "//admin"
// could be routed differently from what the file / upstream layer ends up
// using (a classic ACL bypass).
//
// Canonical form:
//   * starts with '/', no empty segments ("//"), no "." / ".." segments;
//   * %XX escapes of unreserved characters (ALPHA DIGIT - . _ ~) and of
//     non-ASCII bytes (>= 0x80) are decoded, so a UTF-8 character has exactly
//     one spelling; every other escape is kept with upper-case hex digits
//     ("%2f" == "%2F", and "%2F" is NOT a path separator);
//   * a trailing '/' is preserved ("/a/" != "/a").
// Rejected (-> 400): ".." above the root, malformed or NUL escapes, control
// characters, spaces and DEL.
//
// The output is never longer than the input.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace Coring4 {

// Longest accepted request path, bytes.
inline constexpr std::size_t kMaxTargetLen = 8192;

namespace PathDetail {

[[nodiscard]] constexpr int hex_value(unsigned char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] constexpr bool is_unreserved(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

[[nodiscard]] constexpr bool is_forbidden_raw(unsigned char c) noexcept {
  return c <= 0x20 || c == 0x7f;
}

// Escapes that must not stay escaped in canonical form.
[[nodiscard]] constexpr bool is_decoded_in_canonical(unsigned char c) noexcept {
  return is_unreserved(c) || c >= 0x80;
}

[[nodiscard]] constexpr bool is_dot_segment(const char *seg,
                                            std::size_t len) noexcept {
  return (len == 1 && seg[0] == '.') ||
         (len == 2 && seg[0] == '.' && seg[1] == '.');
}

} // namespace PathDetail

// Exact (byte-by-byte) canonical check. Used for config validation and as the
// fallback of the SWAR pre-filter below.
[[nodiscard]] inline bool is_canonical_path_exact(std::string_view p) noexcept {
  const std::size_t n = p.size();
  if (n == 0 || p[0] != '/') {
    return false;
  }
  std::size_t seg_start = 0; // index of the '/' that opened the current segment
  for (std::size_t i = 0; i < n; ++i) {
    const auto c = static_cast<unsigned char>(p[i]);
    if (c == '/') {
      if (i > 0) {
        const std::size_t len = i - seg_start - 1;
        if (len == 0 ||
            PathDetail::is_dot_segment(p.data() + seg_start + 1, len)) {
          return false; // "//", "." or ".."
        }
      }
      seg_start = i;
    } else if (c == '%') {
      if (i + 2 >= n) {
        return false;
      }
      const auto h1 = static_cast<unsigned char>(p[i + 1]);
      const auto h2 = static_cast<unsigned char>(p[i + 2]);
      const int hi = PathDetail::hex_value(h1);
      const int lo = PathDetail::hex_value(h2);
      // Only upper-case escapes of bytes that stay escaped are canonical.
      if (hi < 0 || lo < 0 || h1 >= 'a' || h2 >= 'a') {
        return false;
      }
      const auto v = static_cast<unsigned char>((hi << 4) | lo);
      if (v == 0 || PathDetail::is_decoded_in_canonical(v)) {
        return false;
      }
      i += 2;
    } else if (PathDetail::is_forbidden_raw(c)) {
      return false;
    }
  }
  const std::size_t len = n - seg_start - 1;
  return len == 0 || !PathDetail::is_dot_segment(p.data() + seg_start + 1, len);
}

namespace PathDetail {

inline constexpr uint64_t kOnes = 0x0101010101010101ull;
inline constexpr uint64_t kLow7 = 0x7f7f7f7f7f7f7f7full;
inline constexpr uint64_t kHigh = 0x8080808080808080ull;

// Bit 7 of every byte of `x` that is zero (exact, no false positives).
[[nodiscard]] inline uint64_t zero_bytes(uint64_t x) noexcept {
  return ~(((x & kLow7) + kLow7) | x | kLow7);
}

// Conservative 8-bytes-at-a-time test: true only if the path is certainly
// canonical. It rejects everything containing "//", "/.", '%', control
// characters, space or DEL; this covers every non-canonical input, and also
// some canonical ones ("/.well-known"), which then take the exact path.
[[nodiscard]] inline bool swar_surely_canonical(std::string_view p) noexcept {
  const std::size_t n = p.size();
  if (n == 0 || p[0] != '/') {
    return false;
  }
  const auto *s = reinterpret_cast<const unsigned char *>(p.data());
  uint64_t prev_slash = 0;
  std::size_t i = 0;
  while (i < n) {
    uint64_t w = kOnes * 0x61; // pad with 'a' (neutral)
    const std::size_t take = (n - i) >= 8 ? 8 : (n - i);
    std::memcpy(&w, s + i, take);
    i += take;

    const uint64_t slash = zero_bytes(w ^ (kOnes * '/'));
    const uint64_t dot = zero_bytes(w ^ (kOnes * '.'));
    if ((((slash << 8) | prev_slash) & (slash | dot)) != 0) {
      return false; // "//" or "/."
    }
    prev_slash = slash >> 56;

    const uint64_t pct = zero_bytes(w ^ (kOnes * '%'));
    const uint64_t del = zero_bytes(w ^ (kOnes * 0x7f));
    const uint64_t ctl = (w - kOnes * 0x21) & ~w & kHigh; // any byte <= 0x20
    if ((pct | del | ctl) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace PathDetail

// Hot-path check: true if `p` is already canonical and can be used as is.
[[nodiscard]] inline bool is_canonical_path(std::string_view p) noexcept {
  return PathDetail::swar_surely_canonical(p) || is_canonical_path_exact(p);
}

// Canonicalises `in` into `out`, which must have room for in.size() bytes.
// Returns the output length or nullopt if the path is invalid.
[[nodiscard]] inline std::optional<std::size_t>
normalize_path(std::string_view in, char *out) noexcept {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::size_t n = in.size();
  if (n == 0 || in[0] != '/') {
    return std::nullopt;
  }

  std::size_t pos = 0; // end of the canonical output so far
  std::size_t i = 0;
  while (i < n) {
    ++i; // skip the '/' that opens this segment
    std::size_t w = pos + 1;
    while (i < n && in[i] != '/') {
      const auto c = static_cast<unsigned char>(in[i]);
      if (c == '%') {
        if (i + 2 >= n) {
          return std::nullopt;
        }
        const int hi =
            PathDetail::hex_value(static_cast<unsigned char>(in[i + 1]));
        const int lo =
            PathDetail::hex_value(static_cast<unsigned char>(in[i + 2]));
        if (hi < 0 || lo < 0) {
          return std::nullopt;
        }
        const auto v = static_cast<unsigned char>((hi << 4) | lo);
        if (v == 0) {
          return std::nullopt;
        }
        if (PathDetail::is_decoded_in_canonical(v)) {
          out[w++] = static_cast<char>(v);
        } else {
          out[w++] = '%';
          out[w++] = kHex[hi];
          out[w++] = kHex[lo];
        }
        i += 3;
      } else if (PathDetail::is_forbidden_raw(c)) {
        return std::nullopt;
      } else {
        out[w++] = static_cast<char>(c);
        ++i;
      }
    }

    const bool is_last = i >= n;
    const std::size_t len = w - (pos + 1);
    out[pos] = '/';

    if (PathDetail::is_dot_segment(out + pos + 1, len)) {
      if (len == 2) { // ".."
        if (pos == 0) {
          return std::nullopt; // above the root
        }
        std::size_t k = pos;
        while (k > 0 && out[k - 1] != '/') {
          --k;
        }
        pos = k - 1; // slash that opened the previous segment
      }
      if (is_last) {
        pos += 1; // "/a/b/.." -> "/a/", "/a/." -> "/a/"
      }
    } else if (len == 0) {
      if (is_last) {
        pos += 1; // trailing slash
      } // else "//" collapses
    } else {
      pos = w;
    }
  }

  if (pos == 0) {
    out[0] = '/';
    pos = 1;
  }
  return pos;
}

// Final decoding step for the file-system / upstream layer. `in` is
// Match::rest (already canonical). Decodes the remaining %XX escapes, refusing
// NUL and '/' ("%2F"), so the result cannot introduce a path separator or a
// dot-segment. `out` must have room for in.size() bytes.
[[nodiscard]] inline std::optional<std::size_t>
decode_for_filesystem(std::string_view in, char *out) noexcept {
  const std::size_t n = in.size();
  std::size_t w = 0;
  for (std::size_t i = 0; i < n;) {
    const char c = in[i];
    if (c != '%') {
      out[w++] = c;
      ++i;
      continue;
    }
    if (i + 2 >= n) {
      return std::nullopt;
    }
    const int hi = PathDetail::hex_value(static_cast<unsigned char>(in[i + 1]));
    const int lo = PathDetail::hex_value(static_cast<unsigned char>(in[i + 2]));
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    const auto v = static_cast<unsigned char>((hi << 4) | lo);
    if (v == 0 || v == '/') {
      return std::nullopt;
    }
    out[w++] = static_cast<char>(v);
    i += 3;
  }
  return w;
}

} // namespace Coring4
