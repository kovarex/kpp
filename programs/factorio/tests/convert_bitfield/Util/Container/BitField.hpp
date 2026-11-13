#pragma once

#include <Util/Container/SmallBitset.hpp>
#include <Util/BitFieldBase.hpp>

#include <numeric>
#include <type_traits>

/* The following python code is used to generate the macros:
n = 31

def map_macro(name, pattern, separator):
   print("#define " + name + "1(X) " + pattern.format("X"))
   for i in range(1, n):
       print("#define " + name + str(i + 1) + "(X, ...) " + name + "1(X)" + separator + "_BITFIELD_EXPAND(" + name + str(i) + "(__VA_ARGS__))")
   print("#define " + name + "(...) _BITFIELD_EXPAND(_BITFIELD_GET_MACRO(__VA_ARGS__, " + ", ".join(name + str(i + 1) for i in reversed(range(n))) + ", __DUMMY__)(__VA_ARGS__))")

print("#define _BITFIELD_EXPAND(X) X // MSVC fun")
print("#define _BITFIELD_GET_MACRO(" + ", ".join("_" + str(i + 1) for i in range(n)) + ", Name, ...) Name")
print("#define _BITFIELD_COUNT(...) _BITFIELD_EXPAND(_BITFIELD_GET_MACRO(__VA_ARGS__, " + ", ".join(str(i) for i in reversed(range(n + 1))) + "))")
map_macro("_BITFIELD_ALIASES", "static constexpr Enum {0} = Enum::{0};", " ")
 */
#define _BITFIELD_EXPAND(X) X // MSVC fun
#define _BITFIELD_GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, Name, ...) Name
#define _BITFIELD_COUNT(...) _BITFIELD_EXPAND(_BITFIELD_GET_MACRO(__VA_ARGS__, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))

/* use this to align the backslashes automatically
import sys
lines = [line.rstrip('\n\\ ') for line in sys.stdin]
length = max([len(line) for line in lines])
lines = [line + ' ' * (length - len(line) + 1) + '\\' for line in lines]
print('\n'.join(lines))
*/

#define BITFIELD(Name, ...)                                                                       \
  class Name : public BitFieldBase                                                                \
  {                                                                                               \
  public:                                                                                         \
    enum class Enum                                                                               \
    {                                                                                             \
      __VA_ARGS__                                                                                 \
    };                                                                                            \
    constexpr static size_t value_count = _BITFIELD_COUNT(__VA_ARGS__);                           \
    using UnderlyingType = SmallBitset<value_count>::UnderlyingType;                              \
                                                                                                  \
    constexpr Name() = default;                                                                   \
    constexpr Name(std::initializer_list<Enum> values)                                            \
      : bits(Name::toBits(values))                                                                \
    {}                                                                                            \
                                                                                                  \
    constexpr Name(Enum value)                                                                    \
      : bits(static_cast<uint64_t>(1ULL << static_cast<std::size_t>(value))) {}                   \
                                                                                                  \
private:                                                                                          \
    constexpr Name(SmallBitset<value_count> bits)                                                 \
      : bits(bits)                                                                                \
    {}                                                                                            \
                                                                                                  \
public:                                                                                           \
    Name& add(Enum value) { this->bits.set(static_cast<std::size_t>(value)); return *this; }      \
    Name& remove(Enum value) { this->bits.reset(static_cast<std::size_t>(value)); return *this; } \
    Name& flip(Enum value) { this->bits.flip(static_cast<std::size_t>(value)); return *this; }    \
    Name& set(Enum value, bool boolValue) { boolValue ? this->add(value) : this->remove(value); return *this; } \
                                                                                                  \
    Name& set() { this->bits.set(); return *this; }                                               \
    Name& reset() { this->bits.reset(); return *this; }                                           \
    Name& flip() { this->bits.flip(); return *this; }                                             \
                                                                                                  \
    constexpr bool none() const { return this->bits.none(); }                                     \
    constexpr bool any() const { return this->bits.any(); }                                       \
    constexpr bool all() const { return this->bits.all(); }                                       \
    template<Enum value>                                                                          \
    constexpr bool contains() const { return bits.test<static_cast<std::size_t>(value)>(); }      \
    constexpr bool checkedContains(Enum value) const                                              \
      { return bits.test(static_cast<std::size_t>(value)); }                                      \
                                                                                                  \
    constexpr bool operator==(Name other) const { return this->bits == other.bits; }              \
    constexpr bool operator!=(Name other) const { return this->bits != other.bits; }              \
    constexpr Name operator|(Enum value) const { return (*this) | Name(value); }                  \
    constexpr Name operator|(const Name other) const { return Name(this->bits | other.bits); }    \
    constexpr Name operator&(Enum value) const { return (*this) & Name(value); }                  \
    constexpr Name operator&(const Name other) const { return Name(this->bits & other.bits); }    \
    Name& operator&=(const Name& other) { this->bits &= other.bits; return *this; }               \
    Name& operator|=(const Name& other) { this->bits |= other.bits; return *this; }               \
                                                                                                  \
    constexpr SmallBitset<value_count>::UnderlyingType getBits() const { return this->bits.getBits(); }  \
    static Name fromBits(uint64_t bits)                                                           \
      { Name value; value.bits = SmallBitset<value_count>(bits); return value; }                  \
                                                                                                  \
    using enum Enum;                                                                              \
  private:                                                                                        \
    constexpr static uint64_t toBits(std::initializer_list<Enum> values)                          \
    {                                                                                             \
      uint64_t acc = 0;                                                                           \
      for (auto value : values)                                                                   \
        acc |= static_cast<uint64_t>(1ULL << static_cast<std::size_t>(value));                    \
      return acc;                                                                                 \
    }                                                                                             \
                                                                                                  \
    SmallBitset<value_count> bits;                                                                \
  }
