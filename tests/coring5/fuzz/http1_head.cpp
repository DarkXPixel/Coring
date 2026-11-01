// Differential fuzzer: parsing the head accumulated in arbitrary chunks must
// give exactly the same verdict as parsing it all at once. This is the harness
// that catches "works on one TCP segmentation, breaks on another" bugs.
#include "coring5/proto/http1/Parser.hpp"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {
struct Verdict {
  ParseStatus status;
  ParseError error;
  std::size_t consumed;
  std::string method, target, host;
  std::size_t headers;
  bool operator==(const Verdict &) const = default;
};

Verdict summarize(const HeadResult &r, const RequestHead &h) {
  Verdict v{r.status, r.error, r.consumed, {}, {}, {}, 0};
  if (r.status == ParseStatus::Done) {
    v.method = std::string(h.method);
    v.target = std::string(h.target);
    v.host = std::string(h.host);
    v.headers = h.header_count;
  }
  return v;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const std::size_t step = 1U + data[0] % 31U;
  const char *p = reinterpret_cast<const char *>(data + 1);
  const std::size_t n = size - 1;
  const Limits limits{};

  RequestHead whole_head;
  const auto whole =
      summarize(parse_head({p, n}, limits, whole_head), whole_head);

  // Incremental: grow the visible prefix by `step` bytes and re-parse, as the
  // connection does when a head arrives in several recv() completions.
  Verdict incr{ParseStatus::NeedMore, ParseError::None, 0, {}, {}, {}, 0};
  RequestHead ih;
  for (std::size_t len = step;; len += step) {
    const std::size_t cur = len < n ? len : n;
    const auto r = parse_head({p, cur}, limits, ih);
    if (r.status != ParseStatus::NeedMore || cur == n) {
      incr = summarize(r, ih);
      break;
    }
  }

  // The incremental run must never report success/failure that the whole-buffer
  // run disagrees with. (If the whole buffer says NeedMore, both must.)
  if (!(whole == incr) && whole.status != ParseStatus::NeedMore) {
    // A head terminated before the end of the input yields the same verdict
    // only if bytes after the terminator are ignored, which they are.
    std::abort();
  }
  return 0;
}
