#include "coring5/proto/http1/ChunkedDecoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace Coring5::Http1 {
namespace {

constexpr bool is_tchar(unsigned char c) noexcept {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
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

constexpr int hex_val(unsigned char c) noexcept {
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

constexpr bool is_ctl(unsigned char c) noexcept {
  return c < 0x20 || c == 0x7f;
}

constexpr char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool is_forbidden(std::string_view n) noexcept {
  constexpr std::array<std::string_view, 10> kNames{"content-length",
                                                    "transfer-encoding",
                                                    "trailer",
                                                    "connection",
                                                    "keep-alive",
                                                    "proxy-connection",
                                                    "te",
                                                    "upgrade",
                                                    "host",
                                                    "expect"};
  return std::ranges::find(kNames, n) != kNames.end();
}

} // namespace

ChunkedStep ChunkedDecoder::fail(Errc e, ChunkedReason r,
                                 std::size_t consumed) noexcept {
  st_ = St::Failed;
  reason_ = r;
  return {ChunkedEvent::Error, e, r, consumed, {}};
}

bool ChunkedDecoder::finish_name() noexcept {
  if (name_overflow_) {
    return true; // longer than any forbidden name
  }
  char buf[kNameCap];
  for (uint8_t i = 0; i < name_len_; ++i) {
    buf[i] = lower(name_[i]);
  }
  return !is_forbidden(std::string_view(buf, name_len_));
}

Errc ChunkedDecoder::on_eof() noexcept {
  if (st_ == St::Done) {
    return Errc::Ok;
  }
  if (st_ != St::Failed) {
    st_ = St::Failed;
    reason_ = ChunkedReason::PrematureEof;
  }
  return Errc::ProtocolViolation;
}

ChunkedStep ChunkedDecoder::feed(std::span<const char> in,
                                 std::size_t max_data) noexcept {
  if (st_ == St::Done) {
    return {ChunkedEvent::Done, Errc::Ok, ChunkedReason::None, 0, {}};
  }
  if (st_ == St::Failed) {
    const Errc e = (reason_ == ChunkedReason::BodyTooLarge ||
                    reason_ == ChunkedReason::ExtTooLarge ||
                    reason_ == ChunkedReason::TrailersTooLarge)
                       ? Errc::TooLarge
                       : Errc::ProtocolViolation;
    return {ChunkedEvent::Error, e, reason_, 0, {}};
  }

  const auto bad = [&](ChunkedReason r, std::size_t i) {
    const bool big = r == ChunkedReason::BodyTooLarge ||
                     r == ChunkedReason::ExtTooLarge ||
                     r == ChunkedReason::TrailersTooLarge;
    return fail(big ? Errc::TooLarge : Errc::ProtocolViolation, r, i + 1);
  };

  std::size_t i = 0;
  while (i < in.size()) {
    if (st_ == St::Data) {
      if (max_data == 0) {
        return {ChunkedEvent::NeedCredit, Errc::Ok, ChunkedReason::None, i, {}};
      }
      const uint64_t avail = in.size() - i;
      const auto n = static_cast<std::size_t>(
          std::min<uint64_t>({remaining_, avail, max_data}));
      ChunkedStep s{ChunkedEvent::Data, Errc::Ok, ChunkedReason::None, i + n,
                    in.subspan(i, n)};
      remaining_ -= n;
      body_bytes_ += n;
      if (remaining_ == 0) {
        st_ = St::DataCr;
      }
      return s;
    }

    const auto c = static_cast<unsigned char>(in[i]);
    const bool in_ext = st_ >= St::Ext && st_ <= St::ExtQEnd;
    if (in_ext && ++ext_bytes_ > max_ext_bytes_) {
      return bad(ChunkedReason::ExtTooLarge, i);
    }
    if (st_ >= St::TrStart && st_ <= St::EndLf) {
      if (++trailer_bytes_ > max_trailer_bytes_) {
        return bad(ChunkedReason::TrailersTooLarge, i);
      }
      if (!trailer_storage_.empty()) {
        if (trailer_bytes_ > trailer_storage_.size()) {
          return bad(ChunkedReason::TrailersTooLarge, i);
        }
        trailer_storage_[trailer_bytes_ - 1] = in[i];
      }
    }

    switch (st_) {
    case St::Size: {
      const int h = hex_val(c);
      if (h >= 0) {
        if (digits_ >= 16) {
          return bad(ChunkedReason::BodyTooLarge, i);
        }
        ++digits_;
        chunk_size_ = (chunk_size_ << 4) | static_cast<uint64_t>(h);
      } else if (digits_ == 0) {
        return bad(ChunkedReason::BadChunkSize, i);
      } else if (c == ';') {
        ext_bytes_ = 1;
        if (ext_bytes_ > max_ext_bytes_) {
          return bad(ChunkedReason::ExtTooLarge, i);
        }
        st_ = St::Ext;
      } else if (c == '\r') {
        st_ = St::SizeLf;
      } else {
        return bad(ChunkedReason::BadChunkSize, i);
      }
      break;
    }
    case St::Ext:
      if (!is_tchar(c)) {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      st_ = St::ExtName;
      break;
    case St::ExtName:
      if (c == '=') {
        st_ = St::ExtValue0;
      } else if (c == ';') {
        st_ = St::Ext;
      } else if (c == '\r') {
        st_ = St::SizeLf;
      } else if (!is_tchar(c)) {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      break;
    case St::ExtValue0:
      if (c == '"') {
        st_ = St::ExtQuoted;
      } else if (is_tchar(c)) {
        st_ = St::ExtValue;
      } else {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      break;
    case St::ExtValue:
      if (c == ';') {
        st_ = St::Ext;
      } else if (c == '\r') {
        st_ = St::SizeLf;
      } else if (!is_tchar(c)) {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      break;
    case St::ExtQuoted:
      if (c == '\\') {
        st_ = St::ExtEscape;
      } else if (c == '"') {
        st_ = St::ExtQEnd;
      } else if (is_ctl(c) && c != '\t') {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      break;
    case St::ExtEscape:
      if (is_ctl(c) && c != '\t') {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      st_ = St::ExtQuoted;
      break;
    case St::ExtQEnd:
      if (c == ';') {
        st_ = St::Ext;
      } else if (c == '\r') {
        st_ = St::SizeLf;
      } else {
        return bad(ChunkedReason::BadChunkExt, i);
      }
      break;
    case St::SizeLf:
      if (c != '\n') {
        return bad(ChunkedReason::BadChunkCrlf, i);
      }
      if (chunk_size_ > std::numeric_limits<uint64_t>::max() - body_bytes_) {
        return bad(ChunkedReason::BodyTooLarge, i);
      }
      if (chunk_size_ == 0) {
        st_ = St::TrStart;
        trailer_bytes_ = 0;
        trailers_ = 0;
      } else {
        remaining_ = chunk_size_;
        st_ = St::Data;
      }
      break;
    case St::DataCr:
      if (c != '\r') {
        return bad(ChunkedReason::BadChunkCrlf, i);
      }
      st_ = St::DataLf;
      break;
    case St::DataLf:
      if (c != '\n') {
        return bad(ChunkedReason::BadChunkCrlf, i);
      }
      st_ = St::Size;
      digits_ = 0;
      chunk_size_ = 0;
      ext_bytes_ = 0;
      break;
    case St::TrStart:
      if (c == '\r') {
        st_ = St::EndLf;
      } else if (is_tchar(c)) {
        if (++trailers_ > max_trailers_) {
          return bad(ChunkedReason::TrailersTooLarge, i);
        }
        name_len_ = 0;
        name_overflow_ = false;
        name_[name_len_++] = static_cast<char>(c);
        st_ = St::TrName;
      } else {
        return bad(ChunkedReason::BadTrailer, i);
      }
      break;
    case St::TrName:
      if (c == ':') {
        if (!finish_name()) {
          return bad(ChunkedReason::ForbiddenTrailer, i);
        }
        st_ = St::TrValue;
      } else if (is_tchar(c)) {
        if (name_len_ < kNameCap) {
          name_[name_len_++] = static_cast<char>(c);
        } else {
          name_overflow_ = true;
        }
      } else {
        return bad(ChunkedReason::BadTrailer, i);
      }
      break;
    case St::TrValue:
      if (c == '\r') {
        st_ = St::TrLf;
      } else if (is_ctl(c) && c != '\t') {
        return bad(ChunkedReason::BadTrailer, i);
      }
      break;
    case St::TrLf:
      if (c != '\n') {
        return bad(ChunkedReason::BadTrailer, i);
      }
      st_ = St::TrStart;
      break;
    case St::EndLf:
      if (c != '\n') {
        return bad(ChunkedReason::BadChunkCrlf, i);
      }
      st_ = St::Done;
      return {ChunkedEvent::Done, Errc::Ok, ChunkedReason::None, i + 1, {}};
    case St::Data:
    case St::Done:
    case St::Failed:
      break; // handled above
    }
    ++i;
  }
  return {ChunkedEvent::NeedMore, Errc::Ok, ChunkedReason::None, in.size(), {}};
}

} // namespace Coring5::Http1
