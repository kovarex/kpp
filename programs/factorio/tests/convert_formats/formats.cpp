// The converter's test of Factorio's own printf mismatches: a 64-bit argument read by a 32-bit
// conversion (`%u` of a size_t or of a 64-bit alias, `#%ull`), and an argument the format does not
// read. C++ passes the 64 bits and printf reads the low 32 on x64, and ignores the extra argument;
// what this file prints compiled as C++ (convert_formats.expected) is what its conversion must
// print. The values are above 2^32, so that the 32 bits read show.
#include <cstdint>
#include <cstdio>
#include <vector>

using UnitNumber = uint64_t;

// A container whose size_type is a std::vector's (Factorio's FlatSet, whose size TestLuaEvents
// prints with %u): size_t in C++, whatever width the library's sizes have.
template<class T>
class FlatSet
{
public:
  using container_type = std::vector<T>;
  using size_type = typename container_type::size_type;
  size_type size() const { return this->data.size(); }
  container_type data;
};

UnitNumber nextUnitNumber() { return (UnitNumber(7) << 32) + 42; }

// The game's toCStr (Util/ToStr.hpp): a String's text for %s. Its conversion is the value itself
// where the value's type prints what toStr writes (a number's digits), toStr(x) otherwise.
#define toCStr(x) toStr(x).c_str()
std::string toStr(uint64_t value)
{
  char text[32];
  snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
  return text;
}
struct Tick
{
  uint64_t value;
};
std::string toStr(const Tick& tick) { return "tick " + toStr(tick.value); }
const char* actionName(int action) { return action ? "Build" : "Nothing"; }

int main()
{
  char text[128];
  size_t index = (size_t(3) << 32) + 5;
  snprintf(text, sizeof(text), "index %u, next %u, unit #%ull, signed %d", index, index + 1, nextUnitNumber(), index);
  printf("%s\n", text);
  printf("no conversion\n", index);
  printf("one conversion %u\n", 9u, index);
  FlatSet<int> set;
  set.data = {1, 2, 3};
  std::vector<int> vector = {4, 5};
  printf("sizes %u %u\n", set.size(), vector.size());
  // A `*` width, the argument before the value (LuaHelper's `%*c`), negative aligning left
  int depth = 3;
  printf("%*c{\n", depth, ' ');
  printf("[%-*s]\n", 6, "ab");
  printf("[%*d]\n", -4, 7);
  // The # flag: C's 0x only for a value that is not 0, also aligned left (left as printf)
  unsigned zero = 0, one = 1;
  printf("[%#06x] [%#06x] [%#04X]\n", zero, 0x1fu, zero);
  printf("[%-#6x] [%-#06x]\n", one, one);
  // Values that print as the conversion reads them, written without a cast: a uint16_t for %u, a
  // uint8_t and an int16_t for %d (an int read by %u keeps its uint32_t); toCStr's String
  uint16_t player = 65535;
  uint8_t small = 200;
  int16_t delta = -4;
  int negative = -1;
  uint64_t updateTick = 123456789012ull;
  Tick tick{7};
  printf("[%s %u %s] %d %d %u %s\n", toCStr(updateTick), player, actionName(1), small, delta, negative, toCStr(tick));
  return 0;
}
