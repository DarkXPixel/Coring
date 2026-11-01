#pragma once

// Low-level primitives for the routing hot path: a fast seeded hash, SWAR byte
// search / ASCII lower-casing and the chained path hash.
//
// Paths are hashed *per segment* and the segment hashes are chained. The chain
// value after the k-th segment is also the hash of the k-segment prefix, so a
// single left-to-right pass yields the hash of every prefix of a request path
// (this is what makes longest-prefix matching O(path length)).
//
// The same functions are used when compiling a table and when matching, so
// both sides always agree.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace Coring4::RouteDetail {

static_assert(std::endian::native == std::endian::little,
              "routing SWAR helpers assume a little-endian target");

__extension__ typedef unsigned __int128 u128;

inline constexpr uint64_t kS0 = 0x2d358dccaa6c78a5ull;
inline constexpr uint64_t kS1 = 0x8bb84b93962eacc9ull;
inline constexpr uint64_t kS2 = 0x4b33a62ed433d4a3ull;
inline constexpr uint64_t kS3 = 0x4d5a2da51de1aa47ull;

inline constexpr uint64_t kPathDomain = 0xc2b2ae3d27d4eb4full;

inline constexpr std::size_t kMaxHostLen = 253;
// Longest label chain the matcher handles (real names have 2-6 labels). Also
// the width of the "which wildcard sizes exist" mask.
inline constexpr unsigned kMaxHostLabels = 64;

[[nodiscard]] inline uint64_t rd64(const unsigned char *p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

[[nodiscard]] inline uint64_t rd32(const unsigned char *p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

[[nodiscard]] inline uint64_t mum(uint64_t a, uint64_t b) noexcept {
  const u128 r = static_cast<u128>(a) * b;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}

// wyhash-style hash. Not cryptographic; the seed is random per table, which is
// what protects against hash-flooding of attacker-chosen paths / hosts.
[[nodiscard]] inline uint64_t hash_bytes(const char *data, std::size_t len,
                                         uint64_t seed) noexcept {
  const auto *p = reinterpret_cast<const unsigned char *>(data);
  seed ^= mum(seed ^ kS0, kS1);
  uint64_t a;
  uint64_t b;
  if (len <= 16) {
    if (len >= 4) {
      const std::size_t off = (len >> 3) << 2;
      a = (rd32(p) << 32) | rd32(p + off);
      b = (rd32(p + len - 4) << 32) | rd32(p + len - 4 - off);
    } else if (len > 0) {
      a = (static_cast<uint64_t>(p[0]) << 16) |
          (static_cast<uint64_t>(p[len >> 1]) << 8) | p[len - 1];
      b = 0;
    } else {
      a = 0;
      b = 0;
    }
  } else {
    std::size_t i = len;
    if (i > 48) {
      uint64_t see1 = seed;
      uint64_t see2 = seed;
      do {
        seed = mum(rd64(p) ^ kS1, rd64(p + 8) ^ seed);
        see1 = mum(rd64(p + 16) ^ kS2, rd64(p + 24) ^ see1);
        see2 = mum(rd64(p + 32) ^ kS3, rd64(p + 40) ^ see2);
        p += 48;
        i -= 48;
      } while (i > 48);
      seed ^= see1 ^ see2;
    }
    while (i > 16) {
      seed = mum(rd64(p) ^ kS1, rd64(p + 8) ^ seed);
      i -= 16;
      p += 16;
    }
    a = rd64(p + i - 16);
    b = rd64(p + i - 8);
  }
  a ^= kS1;
  b ^= seed;
  const u128 r = static_cast<u128>(a) * b;
  a = static_cast<uint64_t>(r);
  b = static_cast<uint64_t>(r >> 64);
  return mum(a ^ kS0 ^ len, b ^ kS1);
}

// Order dependent combination of an accumulator with a segment hash.
[[nodiscard]] inline uint64_t chain_step(uint64_t acc,
                                         uint64_t seg_hash) noexcept {
  return mum(acc ^ kS2, seg_hash ^ kS3);
}

// Returns a pointer to the first `c` in [p, p + n) or nullptr.
[[nodiscard]] inline const char *find_byte(const char *p, std::size_t n,
                                           char c) noexcept {
  constexpr uint64_t kOnes = 0x0101010101010101ull;
  constexpr uint64_t kHigh = 0x8080808080808080ull;
  const uint64_t pattern = kOnes * static_cast<unsigned char>(c);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, sizeof(w));
    w ^= pattern;
    // Exact for the lowest matching byte (false positives can only appear
    // above a true match), which is the one countr_zero picks.
    const uint64_t t = (w - kOnes) & ~w & kHigh;
    if (t != 0) {
      return p + i + (static_cast<std::size_t>(std::countr_zero(t)) >> 3);
    }
  }
  for (; i < n; ++i) {
    if (p[i] == c) {
      return p + i;
    }
  }
  return nullptr;
}

// ASCII lower-casing of eight bytes at once. Non-ASCII bytes are untouched.
[[nodiscard]] inline uint64_t lower8(uint64_t x) noexcept {
  constexpr uint64_t kOnes = 0x0101010101010101ull;
  const uint64_t heptets = x & (0x7f * kOnes);
  const uint64_t is_gt_z = heptets + ((0x7f - 'Z') * kOnes);
  const uint64_t is_ge_a = heptets + ((0x80 - 'A') * kOnes);
  const uint64_t is_ascii = ~x & (0x80 * kOnes);
  const uint64_t is_upper = is_ascii & (is_ge_a ^ is_gt_z);
  return x | (is_upper >> 2);
}

inline void lower_copy(char *dst, const char *src, std::size_t n) noexcept {
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, src + i, sizeof(w));
    w = lower8(w);
    std::memcpy(dst + i, &w, sizeof(w));
  }
  for (; i < n; ++i) {
    const char c = src[i];
    dst[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
}

// ---------------------------------------------------------------------------
// Host names
// ---------------------------------------------------------------------------

// Removes ":port" and one trailing dot. Understands "[v6]:port". Returns an
// empty view for malformed input.
[[nodiscard]] inline std::string_view
strip_host_port(std::string_view h) noexcept {
  if (h.empty()) {
    return h;
  }
  if (h.front() == '[') {
    const std::size_t e = h.find(']');
    if (e == std::string_view::npos) {
      return {};
    }
    return {h.data(), e + 1};
  }
  const std::size_t c = h.rfind(':');
  if (c != std::string_view::npos) {
    h = {h.data(), c};
  }
  if (!h.empty() && h.back() == '.') {
    h.remove_suffix(1);
  }
  return h;
}

namespace host_detail {
inline constexpr auto kAllowed = [] {
  std::array<bool, 256> t{};
  for (int c = 'a'; c <= 'z'; ++c) {
    t[static_cast<std::size_t>(c)] = true;
  }
  for (int c = '0'; c <= '9'; ++c) {
    t[static_cast<std::size_t>(c)] = true;
  }
  t[static_cast<unsigned char>('-')] = true;
  t[static_cast<unsigned char>('_')] = true;
  t[static_cast<unsigned char>('.')] = true;
  return t;
}();
} // namespace host_detail

// Validates a lower-case host without port: LDH labels separated by single
// dots (no empty labels), or a bracketed IP literal "[...]".
[[nodiscard]] inline bool valid_host(std::string_view h) noexcept {
  if (h.empty() || h.size() > kMaxHostLen) {
    return false;
  }
  if (h.front() == '[') {
    if (h.size() < 3 || h.back() != ']') {
      return false;
    }
    for (std::size_t i = 1; i + 1 < h.size(); ++i) {
      const char c = h[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' ||
            c == '.')) {
        return false;
      }
    }
    return true;
  }
  if (h.front() == '.' || h.back() == '.') {
    return false;
  }
  char prev = 0;
  for (const char ch : h) {
    if (!host_detail::kAllowed[static_cast<unsigned char>(ch)]) {
      return false;
    }
    if (ch == '.' && prev == '.') {
      return false;
    }
    prev = ch;
  }
  return true;
}

// Bit for "a wildcard whose suffix has `labels` labels exists" (labels >= 1).
// Label counts above 64 share the last bit; probes are always verified.
[[nodiscard]] constexpr uint64_t label_bit(unsigned labels) noexcept {
  return 1ull << (std::min(labels, 64u) - 1);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Bit used in the "which depths / steps exist" masks. Values >= 63 share the
// last bit (the mask is only an accelerator, every probe is verified).
[[nodiscard]] constexpr uint64_t depth_bit(unsigned depth) noexcept {
  return 1ull << std::min(depth, 63u);
}

// Incremental chained hash of a canonical path ("/a/b", "/a/b/", "/").
// A trailing '/' counts as one extra, empty segment ("/a" != "/a/") and "/"
// is the empty chain.
class PathCursor {
public:
  PathCursor(std::string_view path, uint64_t seed, uint64_t salt) noexcept
      : s_(path.data()), n_(path.size()), seed_(seed), h_(salt ^ kPathDomain) {}

  // Consumes the next non-empty segment. On success `end` is the length of
  // the prefix that ends with it.
  bool next(std::size_t &end) noexcept {
    if (i_ >= n_) {
      return false;
    }
    const char *e = find_byte(s_ + i_, n_ - i_, '/');
    const std::size_t j = e != nullptr ? static_cast<std::size_t>(e - s_) : n_;
    h_ = chain_step(h_, hash_bytes(s_ + i_, j - i_, seed_));
    ++steps_;
    end = j;
    i_ = e != nullptr ? j + 1 : n_ + 1;
    return true;
  }

  // Hash after the segments consumed so far (the prefix hash).
  [[nodiscard]] uint64_t hash() const noexcept { return h_; }
  [[nodiscard]] unsigned depth() const noexcept { return steps_; }

  // True once no non-empty segment remains (only a trailing '/' may be left).
  [[nodiscard]] bool exhausted() const noexcept { return i_ >= n_; }

  // Consumes the trailing empty segment, if any. Call once, after next()
  // returned false. Afterwards hash()/depth() describe the whole path.
  void finish() noexcept {
    if (n_ > 1 && s_[n_ - 1] == '/') {
      h_ = chain_step(h_, hash_bytes(s_, 0, seed_));
      ++steps_;
    }
  }

private:
  const char *s_;
  std::size_t n_;
  std::size_t i_{1};
  uint64_t seed_;
  uint64_t h_;
  unsigned steps_{0};
};

} // namespace Coring4::RouteDetail
