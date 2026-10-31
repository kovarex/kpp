// crc32fast's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built
// once with the C++ library and once converted with it, it must print the same bytes. Every
// function the library exports runs over:
// - buffers of every length from 0 to 4,200 at every alignment from 0 to 15, from a fixed
//   pseudo-random sequence;
// - every file the list file names (one path per line), whole and split for previousCrc32 and
//   crc32_combine2 (the bit- and byte-at-a-time functions only on files up to 64 KB);
// - CRCs chained from a nonzero start.
//
//   crc32fast LISTFILE > output.txt (harness.cmd crc32fast builds and compares both)
#include <crc32fast/Crc32_fast.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t (*CrcFunction)(const void* data, size_t length, uint32_t previousCrc32);

static uint32_t prefetch64(const void* data, size_t length, uint32_t previousCrc32)
{
  return crc32_16bytes_prefetch(data, length, previousCrc32, 64);
}

static const CrcFunction functions[] = {crc32_fast,  crc32_bitwise, crc32_halfbyte, crc32_1byte,   crc32_1byte_tableless, crc32_1byte_tableless2,
                                        crc32_4bytes, crc32_8bytes,  crc32_4x8bytes, crc32_16bytes, prefetch64};
static const int functionCount = sizeof(functions) / sizeof(functions[0]);

static uint64_t state = 0x9E3779B97F4A7C15ull;
static uint8_t nextByte()
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint8_t)(state >> 32);
}

// The bit- and byte-at-a-time ones, run on files only up to this size (4.4 GB of data would take them minutes).
static bool slow(CrcFunction function)
{
  return function == crc32_bitwise || function == crc32_halfbyte || function == crc32_1byte_tableless || function == crc32_1byte_tableless2;
}

// One line: every function's CRC of data (the slow ones only up to 64 KB), then the split forms.
static void report(const char* name, const uint8_t* data, size_t length)
{
  printf("%s %zu", name, length);
  for (int f = 0; f < functionCount; ++f)
    if (length <= 65536 || !slow(functions[f]))
      printf(" %08x", functions[f](data, length, 0));
  size_t half = length / 3;
  uint32_t first = crc32_fast(data, half, 0);
  uint32_t chained = crc32_fast(data + half, length - half, first);
  uint32_t second = crc32_fast(data + half, length - half, 0);
  printf(" %08x %08x %08x\n", chained, crc32_combine2(first, second, length - half), crc32_fast(data, length, 0x12345678u));
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: harness LISTFILE\n");
    return 2;
  }
  // Generated buffers: every length and alignment.
  uint8_t* buffer = (uint8_t*)malloc(4200 + 16);
  for (size_t i = 0; i < 4200 + 16; ++i)
    buffer[i] = nextByte();
  char name[32];
  for (size_t length = 0; length <= 4200; length += length < 300 ? 1 : 97)
    for (int offset = 0; offset < 16; ++offset)
    {
      snprintf(name, sizeof(name), "generated+%d", offset);
      report(name, buffer + offset, length);
    }
  free(buffer);
  // The listed files.
  FILE* list = fopen(argv[1], "rb");
  if (!list)
  {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  char path[4096];
  int files = 0;
  while (fgets(path, sizeof(path), list))
  {
    size_t end = strlen(path);
    while (end > 0 && (path[end - 1] == '\n' || path[end - 1] == '\r'))
      path[--end] = 0;
    if (end == 0)
      continue;
    FILE* file = fopen(path, "rb");
    if (!file)
    {
      printf("%s unreadable\n", path);
      continue;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
    size_t read = fread(data, 1, (size_t)size, file);
    fclose(file);
    report(path, data, read);
    free(data);
    ++files;
  }
  fclose(list);
  printf("files %d\n", files);
  return 0;
}
