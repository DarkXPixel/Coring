#include "coring5/base/Endpoint.hpp"
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto ep = Coring5::Endpoint::parse(
      std::string_view(reinterpret_cast<const char *>(data), size));
  if (ep) {
    (void)ep->to_string();
    (void)ep->port();
  }
  return 0;
}
