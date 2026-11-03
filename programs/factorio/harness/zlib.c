/* zlib's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. It runs:
   - zlib.h's and zconf.h's macros, the converted zlib's constants, aliases and functions (interface());
   - adler32, crc32 and their _combine over buffers of pseudo-random bytes, every length to 1,100;
   - deflate at every level 0 to 9, the five strategies, zlib, gzip and raw streams, small and
     large windows and memory levels, fed and drained in pieces, with Z_SYNC_FLUSH, Z_FULL_FLUSH and
     Z_BLOCK, deflateParams in the middle, a preset dictionary and deflateBound; each stream inflated
     back in pieces, checked, and printed as its length and CRC;
   - compress2, uncompress2 and compressBound;
   - the gz* file functions: a file written with gzprintf, gzputs, gzputc and gzwrite, and read
     back with gzgets, gzgetc, gzungetc, gzread and gzseek;
   - inflate of truncated and corrupted streams (the error codes and messages);
   - every file the list file names (one path per line): a .png's IDAT stream inflated (another
     encoder's deflate), any other file deflated at levels 1, 6 and 9 and inflated back.

     zlib LISTFILE > output.txt (harness.cmd zlib builds and compares both) */
#include "zlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long state = 0x9E3779B97F4A7C15ull;
static unsigned int nextRandom(void)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (unsigned int)(state >> 16);
}

/* Text-like bytes (compressible), or noise. */
static void fill(unsigned char* buffer, size_t length, int noise)
{
  static const char* words[] = {"entity", "inserter", "belt", " ", " ", "\n", "iron-plate", "copper", "=", "{", "}", "0", "1", "2"};
  size_t at = 0;
  while (at < length)
  {
    if (noise)
      buffer[at++] = (unsigned char)nextRandom();
    else
    {
      const char* word = words[nextRandom() % (sizeof(words) / sizeof(words[0]))];
      for (size_t k = 0; word[k] && at < length; ++k)
        buffer[at++] = (unsigned char)word[k];
    }
  }
}

static void checksums(void)
{
  unsigned char buffer[1100];
  uLong adler = adler32(0L, Z_NULL, 0), crc = crc32(0L, Z_NULL, 0), all = 0;
  fill(buffer, sizeof(buffer), 1);
  for (unsigned length = 0; length <= sizeof(buffer); ++length)
  {
    uLong a = adler32(1, buffer, length), c = crc32(0, buffer, length);
    all = crc32(all, (const Bytef*)&a, sizeof(a));
    all = crc32(all, (const Bytef*)&c, sizeof(c));
    uLong split = length / 3;
    uLong a1 = adler32(1, buffer, (uInt)split), a2 = adler32(1, buffer + split, (uInt)(length - split));
    uLong c1 = crc32(0, buffer, (uInt)split), c2 = crc32(0, buffer + split, (uInt)(length - split));
    if (adler32_combine(a1, a2, (z_off_t)(length - split)) != a || crc32_combine(c1, c2, (z_off_t)(length - split)) != c)
      printf("combine differs at %u\n", length);
  }
  adler = adler32_z(adler, buffer, sizeof(buffer));
  crc = crc32_z(crc, buffer, sizeof(buffer));
  printf("checksums: %08lx %08lx %08lx\n", adler, crc, all);
}

/* source deflated with these settings, in pieces; returns the stream (malloc'd), its length in *out. */
static unsigned char* deflateWith(const unsigned char* source, size_t length, int level, int windowBits, int memLevel, int strategy,
                                  int flushes, const unsigned char* dictionary, uInt dictionaryLength, size_t* out, int* rc)
{
  z_stream s;
  memset(&s, 0, sizeof(s));
  *rc = deflateInit2(&s, level, Z_DEFLATED, windowBits, memLevel, strategy);
  if (*rc != Z_OK)
  {
    *out = 0;
    return NULL;
  }
  if (dictionary)
    deflateSetDictionary(&s, dictionary, dictionaryLength);
  size_t capacity = deflateBound(&s, (uLong)length) + 64 + length / 4, written = 0;
  unsigned char* stream = (unsigned char*)malloc(capacity);
  size_t fed = 0;
  int pieces = 0;
  for (;;)
  {
    size_t piece = 1 + nextRandom() % 9000;
    if (piece > length - fed)
      piece = length - fed;
    s.next_in = (Bytef*)(source + fed);
    s.avail_in = (uInt)piece;
    fed += piece;
    int flush = fed == length ? Z_FINISH : Z_NO_FLUSH;
    if (flush == Z_NO_FLUSH && flushes)
    {
      unsigned int k = nextRandom() % 8;
      flush = k == 0 ? Z_SYNC_FLUSH : k == 1 ? Z_FULL_FLUSH : k == 2 ? Z_BLOCK : k == 3 ? Z_PARTIAL_FLUSH : Z_NO_FLUSH;
    }
    if (++pieces == 3 && flushes)
    {
      /* deflateParams compresses what is pending with the old level first: into a set output. */
      int params;
      do
      {
        if (written + 4096 > capacity)
        {
          capacity = capacity * 2 + 4096;
          stream = (unsigned char*)realloc(stream, capacity);
        }
        s.next_out = stream + written;
        s.avail_out = 4096;
        params = deflateParams(&s, (level + 4) % 10, strategy);
        written += 4096 - s.avail_out;
      } while (params == Z_BUF_ERROR);
    }
    do
    {
      if (written + 4096 > capacity)
      {
        capacity = capacity * 2 + 4096;
        stream = (unsigned char*)realloc(stream, capacity);
      }
      uInt room = (uInt)(1 + nextRandom() % 4096);
      s.next_out = stream + written;
      s.avail_out = room;
      *rc = deflate(&s, flush);
      written += room - s.avail_out;
    } while (s.avail_out == 0 || (flush == Z_FINISH && *rc != Z_STREAM_END));
    if (flush == Z_FINISH)
      break;
  }
  int pending = 0, bits = 0;
  deflatePending(&s, (unsigned*)&pending, &bits);
  int end = deflateEnd(&s);
  if (end != Z_OK)
    *rc = end;
  *out = written;
  return stream;
}

/* stream inflated in pieces; returns the bytes (malloc'd), their count in *out, the last code in *rc. */
static unsigned char* inflateWith(const unsigned char* stream, size_t length, int windowBits, const unsigned char* dictionary,
                                  uInt dictionaryLength, size_t expected, size_t* out, int* rc)
{
  z_stream s;
  memset(&s, 0, sizeof(s));
  *rc = inflateInit2(&s, windowBits);
  size_t capacity = expected + 16, written = 0, fed = 0;
  unsigned char* result = (unsigned char*)malloc(capacity);
  while (*rc == Z_OK || *rc == Z_BUF_ERROR || *rc == Z_NEED_DICT)
  {
    size_t piece = 1 + nextRandom() % 5000;
    if (piece > length - fed)
      piece = length - fed;
    s.next_in = (Bytef*)(stream + fed);
    s.avail_in = (uInt)piece;
    fed += piece;
    int progress = 0;
    do
    {
      if (written + 4096 > capacity)
      {
        capacity = capacity * 2 + 4096;
        result = (unsigned char*)realloc(result, capacity);
      }
      uInt room = (uInt)(1 + nextRandom() % 4096);
      s.next_out = result + written;
      s.avail_out = room;
      *rc = inflate(&s, Z_NO_FLUSH);
      written += room - s.avail_out;
      progress = room != s.avail_out;
      if (*rc == Z_NEED_DICT && dictionary)
      {
        *rc = inflateSetDictionary(&s, dictionary, dictionaryLength);
        progress = 1;
        if (*rc == Z_OK)
          s.avail_out = 0; /* (inflate again) */
      }
    } while (s.avail_out == 0 && *rc == Z_OK);
    if (*rc == Z_STREAM_END || (fed == length && !progress && *rc != Z_OK))
      break;
    if (fed == length && !progress)
      break;
  }
  if (*rc != Z_STREAM_END && s.msg)
    printf("  inflate: %d %s\n", *rc, s.msg);
  inflateEnd(&s);
  *out = written;
  return result;
}

static void roundTrip(const char* what, const unsigned char* source, size_t length, int level, int windowBits, int memLevel, int strategy,
                      int flushes, int dictionary)
{
  unsigned char dict[300];
  fill(dict, sizeof(dict), 0);
  size_t compressedLength, inflatedLength;
  int rc, back;
  unsigned char* stream = deflateWith(source, length, level, windowBits, memLevel, strategy, flushes, dictionary ? dict : NULL,
                                      sizeof(dict), &compressedLength, &rc);
  if (!stream)
  {
    printf("%s: deflateInit2 %d\n", what, rc);
    return;
  }
  unsigned char* inflated = inflateWith(stream, compressedLength, windowBits, dictionary ? dict : NULL, sizeof(dict), length,
                                        &inflatedLength, &back);
  printf("%s level %d window %d mem %d strategy %d: %lu -> %lu, crc %08lx, rc %d %d, back %s\n", what, level, windowBits, memLevel,
         strategy, (unsigned long)length, (unsigned long)compressedLength, crc32(0, stream, (uInt)compressedLength), rc, back,
         inflatedLength == length && memcmp(inflated, source, length) == 0 ? "same" : "DIFFERENT");
  free(stream);
  free(inflated);
}

static void streams(void)
{
  size_t length = 300000;
  unsigned char* text = (unsigned char*)malloc(length);
  unsigned char* noise = (unsigned char*)malloc(length);
  fill(text, length, 0);
  fill(noise, length, 1);
  for (int level = 0; level <= 9; ++level)
    for (int strategy = 0; strategy <= Z_FIXED; ++strategy)
      roundTrip("text", text, 1 + nextRandom() % length, level, 15, 8, strategy, level % 2, 0);
  static const int windows[] = {9, 12, 15, 31, -15, -9, 25};
  for (int w = 0; w < 7; ++w)
    for (int memLevel = 1; memLevel <= 9; memLevel += 4)
      roundTrip("window", text, 70000 + nextRandom() % 100000, 6, windows[w], memLevel, Z_DEFAULT_STRATEGY, 1, 0);
  roundTrip("noise", noise, length, 6, 15, 8, Z_DEFAULT_STRATEGY, 1, 0);
  roundTrip("noise", noise, length, 9, 31, 9, Z_FILTERED, 0, 0);
  roundTrip("dictionary", text, 5000, 9, 15, 8, Z_DEFAULT_STRATEGY, 0, 1);
  roundTrip("empty", text, 0, 6, 15, 8, Z_DEFAULT_STRATEGY, 0, 0);

  uLongf bound = compressBound(100000), packed = bound;
  Bytef* buffer = (Bytef*)malloc(bound);
  int rc = compress2(buffer, &packed, text, 100000, 7);
  Bytef* back = (Bytef*)malloc(100000);
  uLongf backLength = 100000, used = packed;
  int rc2 = uncompress2(back, &backLength, buffer, &used);
  printf("compress2: bound %lu, %lu bytes rc %d crc %08lx; uncompress2 rc %d %lu of %lu used, %s\n", bound, packed, rc, crc32(0, buffer, packed), rc2,
         backLength, used, memcmp(back, text, 100000) == 0 ? "same" : "DIFFERENT");

  /* Truncated and corrupted streams. */
  size_t cut;
  for (int k = 0; k < 6; ++k)
  {
    size_t streamLength, outLength;
    int drc, irc;
    unsigned char* stream = deflateWith(text, 20000, 6, k % 2 ? 31 : 15, 8, Z_DEFAULT_STRATEGY, 0, NULL, 0, &streamLength, &drc);
    cut = k < 3 ? streamLength - 1 - nextRandom() % 50 : streamLength;
    if (k >= 3)
    {
      /* (one nextRandom a statement: C leaves the order of an expression's operands open) */
      size_t at = nextRandom() % streamLength;
      stream[at] ^= (unsigned char)(1 + nextRandom() % 255);
    }
    unsigned char* out = inflateWith(stream, cut, k % 2 ? 31 : 15, NULL, 0, 20000, &outLength, &irc);
    printf("damaged %d: %lu of %lu bytes, inflate %d, %lu out, crc %08lx\n", k, (unsigned long)cut, (unsigned long)streamLength, irc,
           (unsigned long)outLength, crc32(0, out, (uInt)outLength));
    free(stream);
    free(out);
  }
  free(buffer);
  free(back);
  free(text);
  free(noise);
}

static void gzipFiles(const char* directory)
{
  char path[4096];
  snprintf(path, sizeof(path), "%s\\zlib-harness.gz", directory);
  gzFile out = gzopen(path, "wb9");
  gzsetparams(out, 6, Z_DEFAULT_STRATEGY);
  for (int k = 0; k < 200; ++k)
    gzprintf(out, "line %d: %s %u\n", k, k % 3 ? "belt" : "inserter", nextRandom());
  gzputs(out, "a line by gzputs\n");
  gzputc(out, 'x');
  unsigned char block[5000];
  fill(block, sizeof(block), 0);
  gzwrite(out, block, sizeof(block));
  long written = gztell(out);
  int closed = gzclose(out);
  gzFile in = gzopen(path, "rb");
  char line[256];
  unsigned long linesHash = 0;
  int lines = 0;
  for (int k = 0; k < 201 && gzgets(in, line, sizeof(line)); ++k)
  {
    linesHash = crc32(linesHash, (const Bytef*)line, (uInt)strlen(line));
    ++lines;
  }
  int errnum = 0;
  const char* message = gzerror(in, &errnum);
  printf("gz read: %d lines, error %d %s\n", lines, errnum, message);
  int c = gzgetc(in);
  gzungetc(c, in);
  int again = gzgetc(in);
  unsigned char back[5000];
  int read = gzread(in, back, sizeof(back));
  long at = gztell(in);
  long rewound = gzseek(in, 100, SEEK_SET);
  int afterSeek = gzgetc(in);
  int eof = gzeof(in);
  gzclose(in);
  remove(path);
  printf("gz: %ld written, close %d, %d lines %08lx, getc %d %d, read %d %s, at %ld, seek %ld getc %d, eof %d\n", written, closed, lines, linesHash,
         c, again, read, memcmp(back, block, sizeof(block)) == 0 ? "same" : "DIFFERENT", at, rewound, afterSeek, eof);
}

static unsigned long readBig(const unsigned char* p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static void file(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* bytes = (unsigned char*)malloc(size + 1);
  size_t got = fread(bytes, 1, size, f);
  fclose(f);
  size_t length = strlen(path);
  if (length > 4 && strcmp(path + length - 4, ".png") == 0 && got > 8)
  {
    /* The IDAT chunks' data, one zlib stream. */
    unsigned char* idat = (unsigned char*)malloc(got);
    size_t idatLength = 0, at = 8;
    unsigned long width = 0, height = 0;
    while (at + 12 <= got)
    {
      unsigned long chunk = readBig(bytes + at);
      if (chunk > got - at - 12)
        break;
      if (memcmp(bytes + at + 4, "IHDR", 4) == 0 && chunk >= 8)
      {
        width = readBig(bytes + at + 8);
        height = readBig(bytes + at + 12);
      }
      if (memcmp(bytes + at + 4, "IDAT", 4) == 0)
      {
        memcpy(idat + idatLength, bytes + at + 8, chunk);
        idatLength += chunk;
      }
      at += chunk + 12;
    }
    size_t outLength;
    int rc;
    unsigned char* out = inflateWith(idat, idatLength, 15, NULL, 0, width * height * 4 + height + 1024, &outLength, &rc);
    printf("%s: %lux%lu, %lu IDAT bytes, inflate %d, %lu bytes, adler %08lx\n", path, width, height, (unsigned long)idatLength, rc,
           (unsigned long)outLength, adler32(1, out, (uInt)outLength));
    free(out);
    free(idat);
  }
  else
  {
    unsigned long sizes[3], crcs[3];
    int same = 1;
    for (int k = 0; k < 3; ++k)
    {
      static const int levels[] = {1, 6, 9};
      size_t streamLength, outLength;
      int drc, irc;
      unsigned char* stream = deflateWith(bytes, got, levels[k], 15, 8, Z_DEFAULT_STRATEGY, 0, NULL, 0, &streamLength, &drc);
      unsigned char* out = inflateWith(stream, streamLength, 15, NULL, 0, got, &outLength, &irc);
      same = same && outLength == got && memcmp(out, bytes, got) == 0 && irc == Z_STREAM_END;
      sizes[k] = (unsigned long)streamLength;
      crcs[k] = crc32(0, stream, (uInt)streamLength);
      free(stream);
      free(out);
    }
    printf("%s: %lu bytes, crc %08lx -> %lu %08lx, %lu %08lx, %lu %08lx, back %s\n", path, (unsigned long)got, crc32(0, bytes, (uInt)got), sizes[0],
           crcs[0], sizes[1], crcs[1], sizes[2], crcs[2], same ? "same" : "DIFFERENT");
  }
  free(bytes);
}

/* zlib.h's and zconf.h's macros, which the converted zlib has as constants, aliases and functions of
   its scope (design.md 16): each constant's value, size and signedness, ZLIB_VERSION and its size,
   the aliases' sizes, and Z_NULL as a pointer. */
static void constant(const char* name, long long value, size_t size, int isSigned)
{
  printf("%s = %lld, %u bytes, %s\n", name, value, (unsigned)size, isSigned ? "signed" : "unsigned");
}

static void interface(void)
{
  constant("ZLIB_VERNUM", (long long)ZLIB_VERNUM, sizeof(ZLIB_VERNUM), ZLIB_VERNUM * 0 - 1 < 0);
  constant("ZLIB_VER_MAJOR", (long long)ZLIB_VER_MAJOR, sizeof(ZLIB_VER_MAJOR), ZLIB_VER_MAJOR * 0 - 1 < 0);
  constant("ZLIB_VER_MINOR", (long long)ZLIB_VER_MINOR, sizeof(ZLIB_VER_MINOR), ZLIB_VER_MINOR * 0 - 1 < 0);
  constant("ZLIB_VER_REVISION", (long long)ZLIB_VER_REVISION, sizeof(ZLIB_VER_REVISION), ZLIB_VER_REVISION * 0 - 1 < 0);
  constant("ZLIB_VER_SUBREVISION", (long long)ZLIB_VER_SUBREVISION, sizeof(ZLIB_VER_SUBREVISION), ZLIB_VER_SUBREVISION * 0 - 1 < 0);
  constant("Z_NO_FLUSH", (long long)Z_NO_FLUSH, sizeof(Z_NO_FLUSH), Z_NO_FLUSH * 0 - 1 < 0);
  constant("Z_PARTIAL_FLUSH", (long long)Z_PARTIAL_FLUSH, sizeof(Z_PARTIAL_FLUSH), Z_PARTIAL_FLUSH * 0 - 1 < 0);
  constant("Z_SYNC_FLUSH", (long long)Z_SYNC_FLUSH, sizeof(Z_SYNC_FLUSH), Z_SYNC_FLUSH * 0 - 1 < 0);
  constant("Z_FULL_FLUSH", (long long)Z_FULL_FLUSH, sizeof(Z_FULL_FLUSH), Z_FULL_FLUSH * 0 - 1 < 0);
  constant("Z_FINISH", (long long)Z_FINISH, sizeof(Z_FINISH), Z_FINISH * 0 - 1 < 0);
  constant("Z_BLOCK", (long long)Z_BLOCK, sizeof(Z_BLOCK), Z_BLOCK * 0 - 1 < 0);
  constant("Z_TREES", (long long)Z_TREES, sizeof(Z_TREES), Z_TREES * 0 - 1 < 0);
  constant("Z_OK", (long long)Z_OK, sizeof(Z_OK), Z_OK * 0 - 1 < 0);
  constant("Z_STREAM_END", (long long)Z_STREAM_END, sizeof(Z_STREAM_END), Z_STREAM_END * 0 - 1 < 0);
  constant("Z_NEED_DICT", (long long)Z_NEED_DICT, sizeof(Z_NEED_DICT), Z_NEED_DICT * 0 - 1 < 0);
  constant("Z_ERRNO", (long long)Z_ERRNO, sizeof(Z_ERRNO), Z_ERRNO * 0 - 1 < 0);
  constant("Z_STREAM_ERROR", (long long)Z_STREAM_ERROR, sizeof(Z_STREAM_ERROR), Z_STREAM_ERROR * 0 - 1 < 0);
  constant("Z_DATA_ERROR", (long long)Z_DATA_ERROR, sizeof(Z_DATA_ERROR), Z_DATA_ERROR * 0 - 1 < 0);
  constant("Z_MEM_ERROR", (long long)Z_MEM_ERROR, sizeof(Z_MEM_ERROR), Z_MEM_ERROR * 0 - 1 < 0);
  constant("Z_BUF_ERROR", (long long)Z_BUF_ERROR, sizeof(Z_BUF_ERROR), Z_BUF_ERROR * 0 - 1 < 0);
  constant("Z_VERSION_ERROR", (long long)Z_VERSION_ERROR, sizeof(Z_VERSION_ERROR), Z_VERSION_ERROR * 0 - 1 < 0);
  constant("Z_NO_COMPRESSION", (long long)Z_NO_COMPRESSION, sizeof(Z_NO_COMPRESSION), Z_NO_COMPRESSION * 0 - 1 < 0);
  constant("Z_BEST_SPEED", (long long)Z_BEST_SPEED, sizeof(Z_BEST_SPEED), Z_BEST_SPEED * 0 - 1 < 0);
  constant("Z_BEST_COMPRESSION", (long long)Z_BEST_COMPRESSION, sizeof(Z_BEST_COMPRESSION), Z_BEST_COMPRESSION * 0 - 1 < 0);
  constant("Z_DEFAULT_COMPRESSION", (long long)Z_DEFAULT_COMPRESSION, sizeof(Z_DEFAULT_COMPRESSION), Z_DEFAULT_COMPRESSION * 0 - 1 < 0);
  constant("Z_FILTERED", (long long)Z_FILTERED, sizeof(Z_FILTERED), Z_FILTERED * 0 - 1 < 0);
  constant("Z_HUFFMAN_ONLY", (long long)Z_HUFFMAN_ONLY, sizeof(Z_HUFFMAN_ONLY), Z_HUFFMAN_ONLY * 0 - 1 < 0);
  constant("Z_RLE", (long long)Z_RLE, sizeof(Z_RLE), Z_RLE * 0 - 1 < 0);
  constant("Z_FIXED", (long long)Z_FIXED, sizeof(Z_FIXED), Z_FIXED * 0 - 1 < 0);
  constant("Z_DEFAULT_STRATEGY", (long long)Z_DEFAULT_STRATEGY, sizeof(Z_DEFAULT_STRATEGY), Z_DEFAULT_STRATEGY * 0 - 1 < 0);
  constant("Z_BINARY", (long long)Z_BINARY, sizeof(Z_BINARY), Z_BINARY * 0 - 1 < 0);
  constant("Z_TEXT", (long long)Z_TEXT, sizeof(Z_TEXT), Z_TEXT * 0 - 1 < 0);
  constant("Z_ASCII", (long long)Z_ASCII, sizeof(Z_ASCII), Z_ASCII * 0 - 1 < 0);
  constant("Z_UNKNOWN", (long long)Z_UNKNOWN, sizeof(Z_UNKNOWN), Z_UNKNOWN * 0 - 1 < 0);
  constant("Z_DEFLATED", (long long)Z_DEFLATED, sizeof(Z_DEFLATED), Z_DEFLATED * 0 - 1 < 0);
  constant("MAX_MEM_LEVEL", (long long)MAX_MEM_LEVEL, sizeof(MAX_MEM_LEVEL), MAX_MEM_LEVEL * 0 - 1 < 0);
  constant("MAX_WBITS", (long long)MAX_WBITS, sizeof(MAX_WBITS), MAX_WBITS * 0 - 1 < 0);
  printf("ZLIB_VERSION %s, %u bytes\n", ZLIB_VERSION, (unsigned)sizeof(ZLIB_VERSION));
  printf("z_off_t %u bytes, z_off64_t %u bytes, uLong %u bytes, uInt %u bytes, Bytef %u bytes\n", (unsigned)sizeof(z_off_t), (unsigned)sizeof(z_off64_t),
         (unsigned)sizeof(uLong), (unsigned)sizeof(uInt), (unsigned)sizeof(Bytef));
  void* none = Z_NULL;
  printf("Z_NULL %s\n", none == 0 ? "null" : "not null");
}

int main(int argc, char** argv)
{
  interface();
  printf("zlib %s, flags %lx\n", zlibVersion(), zlibCompileFlags());
  checksums();
  streams();
  if (argc < 2)
    return 0;
  char directory[4096];
  snprintf(directory, sizeof(directory), "%s", argv[1]);
  char* slash = strrchr(directory, '\\');
  if (slash)
    *slash = 0;
  else
    strcpy(directory, ".");
  gzipFiles(directory);
  FILE* list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  char line[4096];
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n > 0)
      file(line);
  }
  fclose(list);
  return 0;
}
