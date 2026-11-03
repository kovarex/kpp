/* zstd's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. It runs:
   - zstd.h's macros, the converted zstd's constants and functions (interface());
   - every file the list file names (one path per line): compressed at levels -5, 1, 3, 9 and 19,
     each frame printed as its length and checksum, decompressed and compared;
   - streaming: ZSTD_compressStream2 fed and drained in pieces of pseudo-random sizes, with
     flushes, checksums, a window log, long-distance matching and the content size set, then
     ZSTD_decompressStream in pieces;
   - a dictionary trained from the first files (ZDICT_trainFromBuffer), used through a CDict and a
     DDict;
   - frames cut short and with a byte flipped (the error names).

     zstd LISTFILE > output.txt (harness.cmd zstd builds and compares both) */
#define ZSTD_STATIC_LINKING_ONLY
#define ZDICT_STATIC_LINKING_ONLY
#include <zstd/zstd.h>
#include <zstd/dictBuilder/zdict.h>
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

/* FNV-1a. */
static unsigned long long fnv1a(const void* data, size_t length)
{
  const unsigned char* bytes = (const unsigned char*)data;
  unsigned long long hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

static unsigned char* readFile(const char* path, size_t* length)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* bytes = (unsigned char*)malloc(size + 1);
  *length = fread(bytes, 1, size, f);
  fclose(f);
  return bytes;
}

static void file(const char* path, ZSTD_CCtx* cctx, ZSTD_DCtx* dctx)
{
  size_t length;
  unsigned char* bytes = readFile(path, &length);
  if (!bytes)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  static const int levels[] = {-5, 1, 3, 9, 19};
  size_t bound = ZSTD_compressBound(length);
  unsigned char* frame = (unsigned char*)malloc(bound);
  unsigned char* back = (unsigned char*)malloc(length + 1);
  printf("%s: %lu bytes", path, (unsigned long)length);
  for (int k = 0; k < 5; ++k)
  {
    size_t size = ZSTD_compressCCtx(cctx, frame, bound, bytes, length, levels[k]);
    if (ZSTD_isError(size))
    {
      printf(", %d: %s", levels[k], ZSTD_getErrorName(size));
      continue;
    }
    size_t got = ZSTD_decompressDCtx(dctx, back, length + 1, frame, size);
    printf(", %d: %lu %016llx %s", levels[k], (unsigned long)size, fnv1a(frame, size),
           !ZSTD_isError(got) && got == length && memcmp(back, bytes, length) == 0 ? "same" : "DIFFERENT");
  }
  printf("\n");
  free(frame);
  free(back);
  free(bytes);
}

static void streaming(const unsigned char* source, size_t length, int level, int round)
{
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, round % 2);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 17 + round % 5);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, round % 3 == 0);
  ZSTD_CCtx_setPledgedSrcSize(cctx, length);
  size_t capacity = ZSTD_compressBound(length) + 4096, written = 0, fed = 0;
  unsigned char* frame = (unsigned char*)malloc(capacity);
  for (;;)
  {
    size_t piece = 1 + nextRandom() % 40000;
    if (piece > length - fed)
      piece = length - fed;
    ZSTD_inBuffer in = {source + fed, piece, 0};
    fed += piece;
    ZSTD_EndDirective mode = fed == length ? ZSTD_e_end : nextRandom() % 5 == 0 ? ZSTD_e_flush : ZSTD_e_continue;
    size_t remaining;
    do
    {
      size_t room = 1 + nextRandom() % 9000;
      if (room > capacity - written)
        room = capacity - written;
      ZSTD_outBuffer out = {frame + written, room, 0};
      remaining = ZSTD_compressStream2(cctx, &out, &in, mode);
      written += out.pos;
      if (ZSTD_isError(remaining))
      {
        printf("stream %d: %s\n", round, ZSTD_getErrorName(remaining));
        break;
      }
    } while (in.pos < in.size || (mode != ZSTD_e_continue && remaining != 0));
    if (mode == ZSTD_e_end)
      break;
  }
  ZSTD_freeCCtx(cctx);

  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  unsigned char* back = (unsigned char*)malloc(length + 1);
  size_t got = 0, consumed = 0, rc = 1;
  while (consumed < written && rc != 0)
  {
    size_t piece = 1 + nextRandom() % 7000;
    if (piece > written - consumed)
      piece = written - consumed;
    ZSTD_inBuffer in = {frame + consumed, piece, 0};
    while (in.pos < in.size)
    {
      size_t room = 1 + nextRandom() % 9000;
      if (room > length + 1 - got)
        room = length + 1 - got;
      ZSTD_outBuffer out = {back + got, room, 0};
      rc = ZSTD_decompressStream(dctx, &out, &in);
      got += out.pos;
      if (ZSTD_isError(rc))
        break;
    }
    consumed += in.pos;
    if (ZSTD_isError(rc))
      break;
  }
  printf("stream %d level %d: %lu -> %lu %016llx, content size %llu, back %lu %s%s\n", round, level, (unsigned long)length, (unsigned long)written,
         fnv1a(frame, written), (unsigned long long)ZSTD_getFrameContentSize(frame, written), (unsigned long)got,
         got == length && memcmp(back, source, length) == 0 ? "same" : "DIFFERENT", ZSTD_isError(rc) ? ZSTD_getErrorName(rc) : "");
  ZSTD_freeDCtx(dctx);
  free(back);
  free(frame);
}

/* zstd.h's macros, which the converted zstd has as constants and functions of its scope (design.md
   16): each constant's value, size and signedness, and ZSTD_COMPRESSBOUND on arguments of several
   types, its result's size with them. */
static void constant(const char* name, long long value, size_t size, int isSigned)
{
  printf("%s = %lld, %u bytes, %s\n", name, value, (unsigned)size, isSigned ? "signed" : "unsigned");
}

static void interface(void)
{
  constant("ZSTD_VERSION_MAJOR", (long long)ZSTD_VERSION_MAJOR, sizeof(ZSTD_VERSION_MAJOR), ZSTD_VERSION_MAJOR * 0 - 1 < 0);
  constant("ZSTD_VERSION_MINOR", (long long)ZSTD_VERSION_MINOR, sizeof(ZSTD_VERSION_MINOR), ZSTD_VERSION_MINOR * 0 - 1 < 0);
  constant("ZSTD_VERSION_RELEASE", (long long)ZSTD_VERSION_RELEASE, sizeof(ZSTD_VERSION_RELEASE), ZSTD_VERSION_RELEASE * 0 - 1 < 0);
  constant("ZSTD_VERSION_NUMBER", (long long)ZSTD_VERSION_NUMBER, sizeof(ZSTD_VERSION_NUMBER), ZSTD_VERSION_NUMBER * 0 - 1 < 0);
  constant("ZSTD_CLEVEL_DEFAULT", (long long)ZSTD_CLEVEL_DEFAULT, sizeof(ZSTD_CLEVEL_DEFAULT), ZSTD_CLEVEL_DEFAULT * 0 - 1 < 0);
  constant("ZSTD_MAGICNUMBER", (long long)ZSTD_MAGICNUMBER, sizeof(ZSTD_MAGICNUMBER), ZSTD_MAGICNUMBER * 0 - 1 < 0);
  constant("ZSTD_MAGIC_DICTIONARY", (long long)ZSTD_MAGIC_DICTIONARY, sizeof(ZSTD_MAGIC_DICTIONARY), ZSTD_MAGIC_DICTIONARY * 0 - 1 < 0);
  constant("ZSTD_MAGIC_SKIPPABLE_START", (long long)ZSTD_MAGIC_SKIPPABLE_START, sizeof(ZSTD_MAGIC_SKIPPABLE_START), ZSTD_MAGIC_SKIPPABLE_START * 0 - 1 < 0);
  constant("ZSTD_MAGIC_SKIPPABLE_MASK", (long long)ZSTD_MAGIC_SKIPPABLE_MASK, sizeof(ZSTD_MAGIC_SKIPPABLE_MASK), ZSTD_MAGIC_SKIPPABLE_MASK * 0 - 1 < 0);
  constant("ZSTD_BLOCKSIZELOG_MAX", (long long)ZSTD_BLOCKSIZELOG_MAX, sizeof(ZSTD_BLOCKSIZELOG_MAX), ZSTD_BLOCKSIZELOG_MAX * 0 - 1 < 0);
  constant("ZSTD_BLOCKSIZE_MAX", (long long)ZSTD_BLOCKSIZE_MAX, sizeof(ZSTD_BLOCKSIZE_MAX), ZSTD_BLOCKSIZE_MAX * 0 - 1 < 0);
  constant("ZSTD_CONTENTSIZE_UNKNOWN", (long long)ZSTD_CONTENTSIZE_UNKNOWN, sizeof(ZSTD_CONTENTSIZE_UNKNOWN), ZSTD_CONTENTSIZE_UNKNOWN * 0 - 1 < 0);
  constant("ZSTD_CONTENTSIZE_ERROR", (long long)ZSTD_CONTENTSIZE_ERROR, sizeof(ZSTD_CONTENTSIZE_ERROR), ZSTD_CONTENTSIZE_ERROR * 0 - 1 < 0);
  constant("ZSTD_FRAMEHEADERSIZE_PREFIX", (long long)ZSTD_FRAMEHEADERSIZE_PREFIX, sizeof(ZSTD_FRAMEHEADERSIZE_PREFIX), ZSTD_FRAMEHEADERSIZE_PREFIX * 0 - 1 < 0);
  constant("ZSTD_FRAMEHEADERSIZE_MIN", (long long)ZSTD_FRAMEHEADERSIZE_MIN, sizeof(ZSTD_FRAMEHEADERSIZE_MIN), ZSTD_FRAMEHEADERSIZE_MIN * 0 - 1 < 0);
  constant("ZSTD_FRAMEHEADERSIZE_MAX", (long long)ZSTD_FRAMEHEADERSIZE_MAX, sizeof(ZSTD_FRAMEHEADERSIZE_MAX), ZSTD_FRAMEHEADERSIZE_MAX * 0 - 1 < 0);
  constant("ZSTD_SKIPPABLEHEADERSIZE", (long long)ZSTD_SKIPPABLEHEADERSIZE, sizeof(ZSTD_SKIPPABLEHEADERSIZE), ZSTD_SKIPPABLEHEADERSIZE * 0 - 1 < 0);
  constant("ZSTD_WINDOWLOG_MAX_32", (long long)ZSTD_WINDOWLOG_MAX_32, sizeof(ZSTD_WINDOWLOG_MAX_32), ZSTD_WINDOWLOG_MAX_32 * 0 - 1 < 0);
  constant("ZSTD_WINDOWLOG_MAX_64", (long long)ZSTD_WINDOWLOG_MAX_64, sizeof(ZSTD_WINDOWLOG_MAX_64), ZSTD_WINDOWLOG_MAX_64 * 0 - 1 < 0);
  constant("ZSTD_WINDOWLOG_MAX", (long long)ZSTD_WINDOWLOG_MAX, sizeof(ZSTD_WINDOWLOG_MAX), ZSTD_WINDOWLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_WINDOWLOG_MIN", (long long)ZSTD_WINDOWLOG_MIN, sizeof(ZSTD_WINDOWLOG_MIN), ZSTD_WINDOWLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_HASHLOG_MAX", (long long)ZSTD_HASHLOG_MAX, sizeof(ZSTD_HASHLOG_MAX), ZSTD_HASHLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_HASHLOG_MIN", (long long)ZSTD_HASHLOG_MIN, sizeof(ZSTD_HASHLOG_MIN), ZSTD_HASHLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_CHAINLOG_MAX_32", (long long)ZSTD_CHAINLOG_MAX_32, sizeof(ZSTD_CHAINLOG_MAX_32), ZSTD_CHAINLOG_MAX_32 * 0 - 1 < 0);
  constant("ZSTD_CHAINLOG_MAX_64", (long long)ZSTD_CHAINLOG_MAX_64, sizeof(ZSTD_CHAINLOG_MAX_64), ZSTD_CHAINLOG_MAX_64 * 0 - 1 < 0);
  constant("ZSTD_CHAINLOG_MAX", (long long)ZSTD_CHAINLOG_MAX, sizeof(ZSTD_CHAINLOG_MAX), ZSTD_CHAINLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_CHAINLOG_MIN", (long long)ZSTD_CHAINLOG_MIN, sizeof(ZSTD_CHAINLOG_MIN), ZSTD_CHAINLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_SEARCHLOG_MAX", (long long)ZSTD_SEARCHLOG_MAX, sizeof(ZSTD_SEARCHLOG_MAX), ZSTD_SEARCHLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_SEARCHLOG_MIN", (long long)ZSTD_SEARCHLOG_MIN, sizeof(ZSTD_SEARCHLOG_MIN), ZSTD_SEARCHLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_MINMATCH_MAX", (long long)ZSTD_MINMATCH_MAX, sizeof(ZSTD_MINMATCH_MAX), ZSTD_MINMATCH_MAX * 0 - 1 < 0);
  constant("ZSTD_MINMATCH_MIN", (long long)ZSTD_MINMATCH_MIN, sizeof(ZSTD_MINMATCH_MIN), ZSTD_MINMATCH_MIN * 0 - 1 < 0);
  constant("ZSTD_TARGETLENGTH_MAX", (long long)ZSTD_TARGETLENGTH_MAX, sizeof(ZSTD_TARGETLENGTH_MAX), ZSTD_TARGETLENGTH_MAX * 0 - 1 < 0);
  constant("ZSTD_TARGETLENGTH_MIN", (long long)ZSTD_TARGETLENGTH_MIN, sizeof(ZSTD_TARGETLENGTH_MIN), ZSTD_TARGETLENGTH_MIN * 0 - 1 < 0);
  constant("ZSTD_STRATEGY_MIN", (long long)ZSTD_STRATEGY_MIN, sizeof(ZSTD_STRATEGY_MIN), ZSTD_STRATEGY_MIN * 0 - 1 < 0);
  constant("ZSTD_STRATEGY_MAX", (long long)ZSTD_STRATEGY_MAX, sizeof(ZSTD_STRATEGY_MAX), ZSTD_STRATEGY_MAX * 0 - 1 < 0);
  constant("ZSTD_OVERLAPLOG_MIN", (long long)ZSTD_OVERLAPLOG_MIN, sizeof(ZSTD_OVERLAPLOG_MIN), ZSTD_OVERLAPLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_OVERLAPLOG_MAX", (long long)ZSTD_OVERLAPLOG_MAX, sizeof(ZSTD_OVERLAPLOG_MAX), ZSTD_OVERLAPLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_WINDOWLOG_LIMIT_DEFAULT", (long long)ZSTD_WINDOWLOG_LIMIT_DEFAULT, sizeof(ZSTD_WINDOWLOG_LIMIT_DEFAULT), ZSTD_WINDOWLOG_LIMIT_DEFAULT * 0 - 1 < 0);
  constant("ZSTD_LDM_HASHLOG_MIN", (long long)ZSTD_LDM_HASHLOG_MIN, sizeof(ZSTD_LDM_HASHLOG_MIN), ZSTD_LDM_HASHLOG_MIN * 0 - 1 < 0);
  constant("ZSTD_LDM_HASHLOG_MAX", (long long)ZSTD_LDM_HASHLOG_MAX, sizeof(ZSTD_LDM_HASHLOG_MAX), ZSTD_LDM_HASHLOG_MAX * 0 - 1 < 0);
  constant("ZSTD_LDM_MINMATCH_MIN", (long long)ZSTD_LDM_MINMATCH_MIN, sizeof(ZSTD_LDM_MINMATCH_MIN), ZSTD_LDM_MINMATCH_MIN * 0 - 1 < 0);
  constant("ZSTD_LDM_MINMATCH_MAX", (long long)ZSTD_LDM_MINMATCH_MAX, sizeof(ZSTD_LDM_MINMATCH_MAX), ZSTD_LDM_MINMATCH_MAX * 0 - 1 < 0);
  constant("ZSTD_LDM_BUCKETSIZELOG_MIN", (long long)ZSTD_LDM_BUCKETSIZELOG_MIN, sizeof(ZSTD_LDM_BUCKETSIZELOG_MIN), ZSTD_LDM_BUCKETSIZELOG_MIN * 0 - 1 < 0);
  constant("ZSTD_LDM_BUCKETSIZELOG_MAX", (long long)ZSTD_LDM_BUCKETSIZELOG_MAX, sizeof(ZSTD_LDM_BUCKETSIZELOG_MAX), ZSTD_LDM_BUCKETSIZELOG_MAX * 0 - 1 < 0);
  constant("ZSTD_LDM_HASHRATELOG_MIN", (long long)ZSTD_LDM_HASHRATELOG_MIN, sizeof(ZSTD_LDM_HASHRATELOG_MIN), ZSTD_LDM_HASHRATELOG_MIN * 0 - 1 < 0);
  constant("ZSTD_LDM_HASHRATELOG_MAX", (long long)ZSTD_LDM_HASHRATELOG_MAX, sizeof(ZSTD_LDM_HASHRATELOG_MAX), ZSTD_LDM_HASHRATELOG_MAX * 0 - 1 < 0);
  constant("ZSTD_HASHLOG3_MAX", (long long)ZSTD_HASHLOG3_MAX, sizeof(ZSTD_HASHLOG3_MAX), ZSTD_HASHLOG3_MAX * 0 - 1 < 0);
  constant("ZSTD_c_rsyncable", (long long)ZSTD_c_rsyncable, sizeof(ZSTD_c_rsyncable), ZSTD_c_rsyncable * 0 - 1 < 0);
  constant("ZSTD_c_format", (long long)ZSTD_c_format, sizeof(ZSTD_c_format), ZSTD_c_format * 0 - 1 < 0);
  constant("ZSTD_c_forceMaxWindow", (long long)ZSTD_c_forceMaxWindow, sizeof(ZSTD_c_forceMaxWindow), ZSTD_c_forceMaxWindow * 0 - 1 < 0);
  constant("ZSTD_c_forceAttachDict", (long long)ZSTD_c_forceAttachDict, sizeof(ZSTD_c_forceAttachDict), ZSTD_c_forceAttachDict * 0 - 1 < 0);
  constant("ZSTD_c_literalCompressionMode", (long long)ZSTD_c_literalCompressionMode, sizeof(ZSTD_c_literalCompressionMode), ZSTD_c_literalCompressionMode * 0 - 1 < 0);
  constant("ZSTD_d_format", (long long)ZSTD_d_format, sizeof(ZSTD_d_format), ZSTD_d_format * 0 - 1 < 0);
  static const size_t sizes[] = {0, 1, 100, 131071, 131072, 131073, 1048576};
  for (int k = 0; k < 7; ++k)
    printf("ZSTD_COMPRESSBOUND(%lu) = %lu, ZSTD_compressBound %lu\n", (unsigned long)sizes[k], (unsigned long)ZSTD_COMPRESSBOUND(sizes[k]),
           (unsigned long)ZSTD_compressBound(sizes[k]));
  int small = 1000;
  unsigned char byte = 200;
  long long negative = -5;
  printf("ZSTD_COMPRESSBOUND of an int %lld, %u bytes; of an unsigned char %lld, %u bytes; of a negative long long %lld, %u bytes\n",
         (long long)ZSTD_COMPRESSBOUND(small), (unsigned)sizeof(ZSTD_COMPRESSBOUND(small)), (long long)ZSTD_COMPRESSBOUND(byte),
         (unsigned)sizeof(ZSTD_COMPRESSBOUND(byte)), (long long)ZSTD_COMPRESSBOUND(negative), (unsigned)sizeof(ZSTD_COMPRESSBOUND(negative)));
}

int main(int argc, char** argv)
{
  interface();
  printf("zstd %s (%u), levels %d to %d\n", ZSTD_versionString(), ZSTD_versionNumber(), ZSTD_minCLevel(), ZSTD_maxCLevel());
  /* Synthetic text, compressible, and noise. */
  size_t length = 1500000;
  unsigned char* text = (unsigned char*)malloc(length);
  static const char* words[] = {"entity", " ", "inserter", "\n", "transport-belt", "=", "{", "}", "0.5", "iron-plate", "  "};
  for (size_t at = 0; at < length;)
  {
    const char* w = words[nextRandom() % 11];
    for (size_t k = 0; w[k] && at < length; ++k)
      text[at++] = (unsigned char)w[k];
  }
  for (int round = 0; round < 6; ++round)
  {
    static const int levels[] = {1, 3, -3, 7, 12, 19};
    /* (one nextRandom a statement: C leaves the order of a call's arguments open) */
    size_t skip = nextRandom() % 1000;
    size_t size = length / 2 + nextRandom() % (length / 2 - 1000);
    streaming(text + skip, size, levels[round], round);
  }

  /* Damaged frames. */
  {
    size_t bound = ZSTD_compressBound(100000);
    unsigned char* frame = (unsigned char*)malloc(bound);
    unsigned char* back = (unsigned char*)malloc(100001);
    size_t size = ZSTD_compress(frame, bound, text, 100000, 5);
    for (int k = 0; k < 6; ++k)
    {
      unsigned char* copy = (unsigned char*)malloc(size);
      memcpy(copy, frame, size);
      size_t cut = k < 3 ? size - 1 - nextRandom() % 100 : size;
      if (k >= 3)
      {
        size_t at = 4 + nextRandom() % (size - 4);
        copy[at] ^= (unsigned char)(1 + nextRandom() % 255);
      }
      size_t got = ZSTD_decompress(back, 100001, copy, cut);
      printf("damaged %d: %lu of %lu: %s\n", k, (unsigned long)cut, (unsigned long)size,
             ZSTD_isError(got) ? ZSTD_getErrorName(got) : got == 100000 && memcmp(back, text, 100000) == 0 ? "same" : "decoded, different");
      free(copy);
    }
    free(frame);
    free(back);
  }
  if (argc < 2)
    return 0;

  FILE* list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  char line[4096];
  char** paths = NULL;
  size_t count = 0;
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n == 0)
      continue;
    paths = (char**)realloc(paths, (count + 1) * sizeof(char*));
    paths[count] = (char*)malloc(n + 1);
    memcpy(paths[count], line, n + 1);
    ++count;
  }
  fclose(list);

  /* A dictionary from the first 400 files. */
  size_t sampleCount = count < 400 ? count : 400, total = 0;
  size_t* sampleSizes = (size_t*)malloc(sampleCount * sizeof(size_t) + 1);
  unsigned char* samples = NULL;
  for (size_t k = 0; k < sampleCount; ++k)
  {
    size_t n;
    unsigned char* bytes = readFile(paths[k], &n);
    if (!bytes)
      n = 0;
    if (n > 20000)
      n = 20000;
    samples = (unsigned char*)realloc(samples, total + n + 1);
    if (n)
      memcpy(samples + total, bytes, n);
    total += n;
    sampleSizes[k] = n;
    free(bytes);
  }
  unsigned char dictionary[16384];
  size_t dictionarySize = ZDICT_trainFromBuffer(dictionary, sizeof(dictionary), samples, sampleSizes, (unsigned)sampleCount);
  if (ZDICT_isError(dictionarySize))
    printf("dictionary: %s\n", ZDICT_getErrorName(dictionarySize));
  else
  {
    printf("dictionary: %lu bytes, id %u, %016llx\n", (unsigned long)dictionarySize, ZDICT_getDictID(dictionary, dictionarySize),
           fnv1a(dictionary, dictionarySize));
    ZSTD_CDict* cdict = ZSTD_createCDict(dictionary, dictionarySize, 9);
    ZSTD_DDict* ddict = ZSTD_createDDict(dictionary, dictionarySize);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    size_t at = 0, packed = 0, all = 0, same = 0;
    for (size_t k = 400; k < count && k < 1400; ++k)
    {
      size_t n;
      unsigned char* bytes = readFile(paths[k], &n);
      if (!bytes)
        continue;
      size_t bound = ZSTD_compressBound(n);
      unsigned char* frame = (unsigned char*)malloc(bound);
      unsigned char* back = (unsigned char*)malloc(n + 1);
      size_t size = ZSTD_compress_usingCDict(cctx, frame, bound, bytes, n, cdict);
      size_t got = ZSTD_decompress_usingDDict(dctx, back, n + 1, frame, size, ddict);
      all += n;
      packed += size;
      at ^= (size_t)fnv1a(frame, size);
      same += !ZSTD_isError(got) && got == n && memcmp(back, bytes, n) == 0;
      free(frame);
      free(back);
      free(bytes);
    }
    printf("with the dictionary: %lu -> %lu bytes, %lu same, %016llx\n", (unsigned long)all, (unsigned long)packed, (unsigned long)same,
           (unsigned long long)at);
    ZSTD_freeCDict(cdict);
    ZSTD_freeDDict(ddict);
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
  }
  free(samples);
  free(sampleSizes);

  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  for (size_t k = 0; k < count; ++k)
  {
    file(paths[k], cctx, dctx);
    free(paths[k]);
  }
  free(paths);
  ZSTD_freeCCtx(cctx);
  ZSTD_freeDCtx(dctx);
  return 0;
}
