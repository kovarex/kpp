// The C++ library's own: a vector and an algorithm (libstdc++ for Linux, libc++ for FreeBSD, MSVC's).
#include <numeric>
#include <vector>
#include "numbers.h"

extern "C" int cpp_sum(int count)
{
  std::vector<int> values(count);
  std::iota(values.begin(), values.end(), 1);
  return std::accumulate(values.begin(), values.end(), 0);
}
