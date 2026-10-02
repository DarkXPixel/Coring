#pragma once

#include <cstdint>
namespace Coring4 {
enum class Http1Stage : std::uint8_t {
  ReadingHeaders = 0,
  ParseHeaders,
  WaitUpstream
};
}