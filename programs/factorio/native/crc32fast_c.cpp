// crc32fast's crc32_fast, which the game's Crc calls, as a C function for Kpp: the library
// (libraries/crc32fast, crc32fast.lib in the game's Release builds) is C++, so the function has
// C++'s name. overrides/src/Util/Crc.kpp defines crc32_fast on this one.
#include <crc32fast/Crc32_fast.h>

extern "C" uint32_t kppCrc32Fast(const void* data, size_t length, uint32_t previousCrc32) { return crc32_fast(data, length, previousCrc32); }
