#pragma once

#include <cstdint>
namespace Coring4 {
enum class PoolType : int8_t {
  NoSetted = -1,
  DefaultPool16KB = 0,
  Static = 123,
  Malloc = 127
};
}