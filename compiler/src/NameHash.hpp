#pragma once
// The hash of the tables keyed by names (IR names, type names, declarations' texts): the standard library's
// hash of a string takes it a byte at a time (MSVC's FNV-1a), and codegen's keys are long, readable names
// (`@"FlatMap<...>::operator()(...)"`, hundreds of bytes). It was 6.6% of the front end's and codegen's time on
// one thread, 15% of module assembly's (docs/codegen-speed.md, section 8).
// NameHash takes sixteen bytes a step with one 64 x 64 -> 128 bit multiplication. Its value is this process's
// own: it orders nothing that is written and places no function in a module (those keep std::hash and FNV,
// whose values the output depends on).
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#if defined(_MSC_VER) && !defined(__SIZEOF_INT128__)
#include <intrin.h>
#endif

struct NameHash
{
  using is_transparent = void; // (a std::string key found by a string_view, without a copy)

  size_t operator()(std::string_view text) const noexcept { return size_t(hash(text.data(), text.size())); }

  static uint64_t hash(const char* p, size_t n) noexcept
  {
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xC2B2AE3D27D4EB4Full, k2 = 0x165667B19E3779F9ull;
    uint64_t seed = k0 ^ (uint64_t(n) * k2), a, b;
    if (n > 16)
    {
      size_t left = n;
      do
      {
        seed = mix(read8(p) ^ k1, read8(p + 8) ^ seed);
        p += 16;
        left -= 16;
      } while (left > 16);
      a = read8(p + left - 16); // (the last sixteen bytes, over what the steps took when fewer are left)
      b = read8(p + left - 8);
    }
    else if (n >= 8)
    {
      a = read8(p);
      b = read8(p + n - 8);
    }
    else if (n >= 4)
    {
      a = read4(p);
      b = read4(p + n - 4);
    }
    else if (n > 0)
    {
      a = (uint64_t(uint8_t(p[0])) << 16) | (uint64_t(uint8_t(p[n >> 1])) << 8) | uint8_t(p[n - 1]);
      b = 0;
    }
    else
      a = b = 0;
    return mix(k1 ^ uint64_t(n), mix(a ^ k1, b ^ seed));
  }

private:
  static uint64_t read8(const char* p) noexcept
  {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
  }
  static uint64_t read4(const char* p) noexcept
  {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
  }
  // The two halves of a * b folded together.
  static uint64_t mix(uint64_t a, uint64_t b) noexcept
  {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 product = (unsigned __int128)a * b;
    return uint64_t(product) ^ uint64_t(product >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    uint64_t high;
    uint64_t low = _umul128(a, b, &high);
    return low ^ high;
#else
    // (no 128-bit product: four 32 x 32 ones)
    uint64_t a0 = uint32_t(a), a1 = a >> 32, b0 = uint32_t(b), b1 = b >> 32;
    uint64_t low = a0 * b0, mid1 = a1 * b0, mid2 = a0 * b1, high = a1 * b1;
    uint64_t carry = ((low >> 32) + uint32_t(mid1) + uint32_t(mid2)) >> 32;
    low += (mid1 << 32) + (mid2 << 32);
    high += (mid1 >> 32) + (mid2 >> 32) + carry;
    return low ^ high;
#endif
  }
};

// The tables keyed by a name: std's, with NameHash, and equal_to<> so that a string_view finds a std::string key.
template<typename Value>
using NameKeyMap = std::unordered_map<std::string, Value, NameHash, std::equal_to<>>;
using NameKeySet = std::unordered_set<std::string, NameHash, std::equal_to<>>;
template<typename Value>
using NameViewKeyMap = std::unordered_map<std::string_view, Value, NameHash, std::equal_to<>>;
using NameViewKeySet = std::unordered_set<std::string_view, NameHash, std::equal_to<>>;
