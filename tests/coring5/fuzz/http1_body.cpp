// Body-framing fuzzer: ChunkedDecoder fed in arbitrary splits / credit sizes
// must agree with a one-shot run (outcome, reason, decoded bytes, consumed);
// ContentLengthFramer must never take more than declared or available.
#include "coring5/proto/http1/BodyFraming.hpp"
#include "coring5/proto/http1/ChunkedDecoder.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {
enum class Outcome : uint8_t { Done, Error, Incomplete };

struct Verdict {
  Outcome outcome{Outcome::Incomplete};
  ChunkedReason reason{ChunkedReason::None};
  Errc error{Errc::Ok};
  std::size_t consumed{0};
  std::string body;
  std::string trailers;
  bool operator==(const Verdict &) const = default;
};

Verdict run(const char *buf, std::size_t size, std::size_t step,
            std::size_t max_data) {
  ChunkedDecoder dec;
  std::array<char, Limits{}.max_head_bytes> capture;
  dec.capture_trailers(capture);
  Verdict v;
  std::size_t pos = 0;
  std::size_t arrived = step < size ? step : size;
  for (std::size_t iter = 0; iter < 4 * size + 16; ++iter) {
    const ChunkedStep s = dec.feed({buf + pos, arrived - pos}, max_data);
    if (s.consumed > arrived - pos) {
      std::abort();
    }
    pos += s.consumed;
    switch (s.event) {
    case ChunkedEvent::Data:
      if (s.data.empty() || s.data.size() > max_data || s.data.data() < buf ||
          s.data.data() + s.data.size() > buf + pos) {
        std::abort();
      }
      v.body.append(s.data.data(), s.data.size());
      break;
    case ChunkedEvent::NeedCredit:
      std::abort(); // max_data > 0 always
    case ChunkedEvent::NeedMore:
      if (pos != arrived) {
        std::abort();
      }
      if (arrived == size) {
        v.outcome = Outcome::Incomplete;
        v.error = dec.on_eof();
        v.consumed = pos;
        if (v.error != Errc::ProtocolViolation && v.error != Errc::TooLarge) {
          std::abort();
        }
        return v;
      }
      arrived = (size - arrived) < step ? size : arrived + step;
      break;
    case ChunkedEvent::Done: {
      if (!dec.done() || dec.on_eof() != Errc::Ok ||
          dec.body_bytes() != v.body.size()) {
        std::abort();
      }
      v.outcome = Outcome::Done;
      v.consumed = pos;
      const auto trailers = dec.trailer_block();
      if (trailers.size() < 2 || trailers.size() > pos ||
          std::memcmp(trailers.data(), buf + pos - trailers.size(), trailers.size()) != 0) {
        std::abort();
      }
      v.trailers.assign(trailers.begin(), trailers.end());
      return v;
    }
    case ChunkedEvent::Error:
      if (!dec.failed() || s.error == Errc::Ok ||
          s.reason == ChunkedReason::None) {
        std::abort();
      }
      // sticky
      if (dec.feed({buf + pos, arrived - pos}, max_data).event !=
          ChunkedEvent::Error) {
        std::abort();
      }
      v.outcome = Outcome::Error;
      v.reason = s.reason;
      v.error = s.error;
      v.consumed = pos;
      return v;
    }
  }
  std::abort(); // no progress
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 10) {
    return 0;
  }
  // ContentLengthFramer
  {
    uint64_t total = 0;
    std::memcpy(&total, data, 8);
    total %= 1U << 20;
    ContentLengthFramer f;
    f.start(total);
    uint64_t taken = 0;
    for (std::size_t i = 8; i < size; ++i) {
      const std::size_t avail = static_cast<std::size_t>(data[i]) * 7U;
      const uint64_t before = f.remaining();
      const std::size_t n = f.take(avail);
      if (n > avail || n > before || f.remaining() != before - n) {
        std::abort();
      }
      taken += n;
    }
    if (taken + f.remaining() != total || f.declared() != total ||
        f.done() != (f.remaining() == 0) ||
        (f.on_eof() == Errc::Ok) != f.done()) {
      std::abort();
    }
  }
  // ChunkedDecoder
  const std::size_t step = 1U + data[0] % 31U;
  const std::size_t credit = 1U + data[1] % 64U;
  const char *p = reinterpret_cast<const char *>(data + 2);
  const std::size_t n = size - 2;
  const Verdict whole = run(p, n, n, static_cast<std::size_t>(-1));
  const Verdict split = run(p, n, step, credit);
  if (whole != split) {
    std::abort();
  }
  return 0;
}
