#include "coring5/config/Loader.hpp"
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto parsed = Coring5::parse_config(
      std::string_view(reinterpret_cast<const char *>(data), size));
  if (parsed) {
    (void)Coring5::validate_config(*parsed); // must never crash
  }
  return 0;
}
