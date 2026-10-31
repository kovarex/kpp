// The converter's test of Factorio's own printf mismatches: a 64-bit argument read by a 32-bit
// conversion (`%u` of a size_t or of a 64-bit alias, `#%ull`), and an argument the format does not
// read. C++ passes the 64 bits and printf reads the low 32 on x64, and ignores the extra argument;
// what this file prints compiled as C++ (convert_formats.expected) is what its conversion must
// print. The values are above 2^32, so that the 32 bits read show.
#include <cstdint>
#include <cstdio>

using UnitNumber = uint64_t;

UnitNumber nextUnitNumber() { return (UnitNumber(7) << 32) + 42; }

int main()
{
  char text[128];
  size_t index = (size_t(3) << 32) + 5;
  snprintf(text, sizeof(text), "index %u, next %u, unit #%ull, signed %d", index, index + 1, nextUnitNumber(), index);
  printf("%s\n", text);
  printf("no conversion\n", index);
  printf("one conversion %u\n", 9u, index);
  return 0;
}
