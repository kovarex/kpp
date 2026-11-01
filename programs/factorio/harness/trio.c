/* trio's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. It runs:
   - trio_snprintf over every conversion with flags, widths, precisions and length modifiers, on
     integers at their limits and on doubles: zeros, infinities, NaNs, subnormals, powers of ten
     and pseudo-random bit patterns, in %e, %f, %g and %a, and the formats Lua's string.format and
     tostring use;
   - trio_sprintf, trio_vsnprintf, trio_snprintfcat, trio_asprintf, trio_vasprintf and trio_cprintf
     (into a callback), with truncation and the counts returned;
   - trio_sscanf of integers, doubles, strings, scansets, %n and failures, and of every printed
     double read back to its bits;
   - every file the list file names (one path per line): its numbers read with trio_sscanf and
     printed again as Lua prints them, hashed per file.

     trio LISTFILE > output.txt (harness.cmd trio builds and compares both) */
#include <trio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>

static unsigned long long state = 0x9E3779B97F4A7C15ull;
static unsigned long long nextRandom(void)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

/* FNV-1a, continued from hash. */
static unsigned long long fnv1a(unsigned long long hash, const void* data, size_t length)
{
  const unsigned char* bytes = (const unsigned char*)data;
  for (size_t i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}
#define FNV_START 0xCBF29CE484222325ull

static double fromBits(unsigned long long bits)
{
  double d;
  memcpy(&d, &bits, sizeof d);
  return d;
}

static unsigned long long toBits(double d)
{
  unsigned long long bits;
  memcpy(&bits, &d, sizeof bits);
  return bits;
}

static void integers(void)
{
  static const char* formats[] = {
    "%d", "%i", "%5d", "%-5d|", "%05d", "%+d", "% d", "%.3d", "%8.3d", "%-+8.3d|", "%x", "%X", "%#x",
    "%#X", "%o", "%#o", "%u", "%08x", "%#010x", "%.0d", "%+.0d", "%c", "%-3c|", "%hd", "%hu", "%hhd",
    "%hhu", "%*d", "%-*d|", "%.*d"};
  static const int values[] = {0, 1, -1, 7, -7, 42, 255, 256, 65535, 65536, 1000000, -1000000,
                               INT_MAX, INT_MIN, 0x7F, 0x80};
  char buffer[256];
  for (size_t f = 0; f < sizeof formats / sizeof *formats; ++f)
  {
    printf("%-10s", formats[f]);
    for (size_t v = 0; v < sizeof values / sizeof *values; ++v)
    {
      int n;
      if (strchr(formats[f], '*'))
        n = trio_snprintf(buffer, sizeof buffer, formats[f], 6, values[v]);
      else if (formats[f][1] == 'c' || formats[f][2] == 'c' || formats[f][3] == 'c')
        n = trio_snprintf(buffer, sizeof buffer, formats[f], 'A' + (values[v] & 15));
      else
        n = trio_snprintf(buffer, sizeof buffer, formats[f], values[v]);
      printf(" [%s]%d", buffer, n);
    }
    printf("\n");
  }

  static const char* longFormats[] = {"%ld", "%lu", "%lx", "%lld", "%llu", "%llx", "%#llo", "%+lld",
                                      "%20lld|", "%-20llu|", "%.25lld", "%zu", "%zd", "%jd", "%td",
                                      "%I64d", "%I64x"};
  static const long long longValues[] = {0, 1, -1, 4294967295ll, 4294967296ll, -4294967296ll,
                                         LLONG_MAX, LLONG_MIN, 1234567890123456789ll};
  for (size_t f = 0; f < sizeof longFormats / sizeof *longFormats; ++f)
  {
    printf("%-10s", longFormats[f]);
    for (size_t v = 0; v < sizeof longValues / sizeof *longValues; ++v)
    {
      int n;
      if (longFormats[f][1] == 'l' && longFormats[f][2] != 'l')
        n = trio_snprintf(buffer, sizeof buffer, longFormats[f], (long)longValues[v]);
      else if (longFormats[f][1] == 'z' || longFormats[f][1] == 't')
        n = trio_snprintf(buffer, sizeof buffer, longFormats[f], (size_t)longValues[v]);
      else
        n = trio_snprintf(buffer, sizeof buffer, longFormats[f], longValues[v]);
      printf(" [%s]%d", buffer, n);
    }
    printf("\n");
  }
}

static void strings(void)
{
  static const char* formats[] = {"%s", "%10s|", "%-10s|", "%.3s", "%10.3s|", "%-10.0s|", "%.*s",
                                   "%%%s%%", "<%s>%c<%s>"};
  static const char* values[] = {"", "a", "hello", "hello, world", "tab\there", "\xC3\xA9t\xC3\xA9"};
  char buffer[256];
  for (size_t f = 0; f < sizeof formats / sizeof *formats; ++f)
  {
    printf("%-10s", formats[f]);
    for (size_t v = 0; v < sizeof values / sizeof *values; ++v)
    {
      int n;
      if (strchr(formats[f], '*'))
        n = trio_snprintf(buffer, sizeof buffer, formats[f], 4, values[v]);
      else if (strchr(formats[f], 'c'))
        n = trio_snprintf(buffer, sizeof buffer, formats[f], values[v], '#', values[v]);
      else
        n = trio_snprintf(buffer, sizeof buffer, formats[f], values[v]);
      printf(" [%s]%d", buffer, n);
    }
    printf("\n");
  }
  int n = trio_snprintf(buffer, sizeof buffer, "%s|%p|%5p", (char*)NULL, (void*)NULL, (void*)NULL);
  printf("null [%s]%d\n", buffer, n);
}

static const char* doubleFormats[] = {
  "%f", "%.0f", "%.1f", "%.3f", "%.10f", "%#.0f", "%12.4f|", "%-12.4f|", "%+f", "% f", "%012.3f",
  "%e", "%.0e", "%.3e", "%#.0e", "%E", "%+.10e", "%15.2e|", "%g", "%.0g", "%.1g", "%.3g", "%.10g",
  "%#g", "%#.3g", "%G", "%.14g", "%.17g", "%a", "%A", "%.3a", "%.0a", "%Lf", "%Lg", "%.20g",
  "%.30f"};

static void formatDouble(double value)
{
  char buffer[512];
  for (size_t f = 0; f < sizeof doubleFormats / sizeof *doubleFormats; ++f)
  {
    int n;
    if (doubleFormats[f][1] == 'L' || doubleFormats[f][2] == 'L')
      n = trio_snprintf(buffer, sizeof buffer, doubleFormats[f], (long double)value);
    else
      n = trio_snprintf(buffer, sizeof buffer, doubleFormats[f], value);
    printf(" [%s]%d", buffer, n);
  }
  printf("\n");
}

static void doubles(void)
{
  static const unsigned long long bits[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0x3FB999999999999Aull,
    0x400921FB54442D18ull, 0x4005BF0A8B145769ull, 0xC08F400000000000ull};
  for (size_t i = 0; i < sizeof bits / sizeof *bits; ++i)
  {
    printf("%016llx", bits[i]);
    formatDouble(fromBits(bits[i]));
  }
  static const double values[] = {0.5, 1.5, 2.5, 0.125, 0.3, 1.0 / 3, 2.0 / 3, 100, 1e15, 1e16,
                                   1e17, 123456789012345678.0, 1e21, 1e22, 1e23, 1e100, 1e300,
                                   1e-5, 1e-6, 1e-7, 1e-300, 1e-310, 9.5, 99.5, 999.5, 0.05,
                                   0.0005, 5e-324, DBL_MAX, DBL_MIN, DBL_EPSILON, -2.75, -1e-10};
  for (size_t i = 0; i < sizeof values / sizeof *values; ++i)
  {
    printf("%016llx", toBits(values[i]));
    formatDouble(values[i]);
  }
  /* Powers of ten and of two around the limits, hashed. */
  for (int base = 2; base <= 10; base += 8)
  {
    unsigned long long hash = FNV_START;
    char buffer[512];
    double power = 1;
    for (int k = 0; k < 1100 && power != 0; ++k, power /= base)
      for (size_t f = 0; f < sizeof doubleFormats / sizeof *doubleFormats; ++f)
        if (doubleFormats[f][1] != 'L' && doubleFormats[f][2] != 'L')
        {
          int n = trio_snprintf(buffer, sizeof buffer, doubleFormats[f], power);
          hash = fnv1a(hash, buffer, strlen(buffer));
          hash = fnv1a(hash, &n, sizeof n);
        }
    power = 1;
    for (int k = 0; k < 1100 && power <= DBL_MAX; ++k, power *= base)
      for (size_t f = 0; f < sizeof doubleFormats / sizeof *doubleFormats; ++f)
        if (doubleFormats[f][1] != 'L' && doubleFormats[f][2] != 'L' && strstr(doubleFormats[f], "f") == NULL)
        {
          int n = trio_snprintf(buffer, sizeof buffer, doubleFormats[f], power);
          hash = fnv1a(hash, buffer, strlen(buffer));
          hash = fnv1a(hash, &n, sizeof n);
        }
    printf("powers of %d: %016llx\n", base, hash);
  }
}

/* Pseudo-random doubles: each printed in Lua's formats and in %e, %a, read back with trio_sscanf. */
static void randomDoubles(void)
{
  static const char* formats[] = {"%.14g", "%.17g", "%e", "%.20e", "%a", "%g", "%.3f"};
  for (int round = 0; round < 16; ++round)
  {
    unsigned long long hash = FNV_START;
    int exact = 0, readBack = 0;
    for (int i = 0; i < 4000; ++i)
    {
      unsigned long long bits = nextRandom();
      /* Half of them with exponents near 1, where %f stays short. */
      if (i & 1)
        bits = (bits & 0x800FFFFFFFFFFFFFull) | ((unsigned long long)(1023 - 30 + (bits >> 52) % 60) << 52);
      double value = fromBits(bits);
      char buffer[512];
      for (size_t f = 0; f < sizeof formats / sizeof *formats; ++f)
      {
        if (f == 6 && ((bits >> 52) & 0x7FF) > 1023 + 60)
          continue;
        int n = trio_snprintf(buffer, sizeof buffer, formats[f], value);
        hash = fnv1a(hash, buffer, strlen(buffer));
        hash = fnv1a(hash, &n, sizeof n);
        if (f == 1 || f == 3 || f == 4)
        {
          double back = 0;
          if (trio_sscanf(buffer, "%lf", &back) == 1)
          {
            ++readBack;
            unsigned long long backBits = toBits(back);
            hash = fnv1a(hash, &backBits, sizeof backBits);
            if (backBits == bits)
              ++exact;
          }
        }
      }
    }
    printf("random %d: %016llx, %d read back, %d exact\n", round, hash, readBack, exact);
  }
}

static int vformat(char* buffer, size_t size, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  int n = trio_vsnprintf(buffer, size, format, args);
  va_end(args);
  return n;
}

static int vallocate(char** result, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  int n = trio_vasprintf(result, format, args);
  va_end(args);
  return n;
}

struct Sink
{
  unsigned long long hash;
  int count;
};

static int toSink(trio_pointer_t closure, int c)
{
  struct Sink* sink = (struct Sink*)closure;
  unsigned char byte = (unsigned char)c;
  sink->hash = fnv1a(sink->hash, &byte, 1);
  ++sink->count;
  return 1;
}

static void others(void)
{
  char buffer[64];
  int n = trio_sprintf(buffer, "%d-%s-%.2f", 12, "ab", 3.14159);
  printf("sprintf [%s]%d\n", buffer, n);

  /* Truncation: the count is what the whole output would have been. */
  for (size_t size = 0; size < 12; ++size)
  {
    memset(buffer, '@', sizeof buffer);
    n = trio_snprintf(buffer, size, "%s=%d", "truncated", 123456);
    buffer[sizeof buffer - 1] = 0;
    printf("snprintf %u: %d [%.16s]\n", (unsigned)size, n, buffer);
  }
  n = vformat(buffer, sizeof buffer, "%5.1f|%-4d|%s", -0.05, 9, "va");
  printf("vsnprintf [%s]%d\n", buffer, n);
  n = vformat(buffer, 8, "%s", "longer than eight");
  printf("vsnprintf 8 [%s]%d\n", buffer, n);

  strcpy(buffer, "head:");
  n = trio_snprintfcat(buffer, sizeof buffer, "%d,%x", 10, 255);
  printf("snprintfcat [%s]%d\n", buffer, n);
  n = trio_snprintfcat(buffer, 16, "%s", "and more than fits");
  printf("snprintfcat 16 [%s]%d\n", buffer, n);

  char* allocated = NULL;
  n = trio_asprintf(&allocated, "%s %d %g", "allocated", -5, 1e-3);
  printf("asprintf [%s]%d\n", allocated ? allocated : "(null)", n);
  free(allocated);
  allocated = NULL;
  n = vallocate(&allocated, "%0*d", 300, 7);
  printf("vasprintf %d %u %c%c\n", n, allocated ? (unsigned)strlen(allocated) : 0u,
         allocated ? allocated[0] : '?', allocated ? allocated[299] : '?');
  free(allocated);

  struct Sink sink = {FNV_START, 0};
  n = trio_cprintf(toSink, &sink, "%s %d %e %x %c", "callback", 99, 6.02e23, 0xBEEF, 'z');
  printf("cprintf %d %d %016llx\n", n, sink.count, sink.hash);

  int count = -1;
  n = trio_snprintf(buffer, sizeof buffer, "abc%ndef", &count);
  printf("%%n [%s]%d %d\n", buffer, n, count);
  n = trio_snprintf(buffer, sizeof buffer, "%%|%c%c|%%%%", 'x', 0);
  printf("percent [%s]%d\n", buffer, n);
}

static void scanning(void)
{
  int a = 0, b = 0, c = 0, consumed = 0;
  unsigned u = 0;
  double d = 0, e = 0;
  float f = 0;
  long long ll = 0;
  char word[64] = "", set[64] = "", rest[64] = "";
  char ch = 0;

  int n = trio_sscanf("12 -34 +56", "%d %d %d", &a, &b, &c);
  printf("scan ints %d: %d %d %d\n", n, a, b, c);
  n = trio_sscanf("0x1F 017 ff", "%i %i %x", &a, &b, &u);
  printf("scan bases %d: %d %d %u\n", n, a, b, u);
  n = trio_sscanf("3.25 -1e-3 2.5", "%lf %lf %f", &d, &e, &f);
  printf("scan doubles %d: %016llx %016llx %08x\n", n, toBits(d), toBits(e), *(unsigned*)&f);
  n = trio_sscanf("inf -inf nan", "%lf %lf %lf", &d, &e, &d);
  printf("scan specials %d: %016llx\n", n, toBits(e));
  n = trio_sscanf("0x1.8p3 1e400 1e-400", "%lf %lf %lf", &d, &e, &d);
  printf("scan edges %d: %016llx %016llx\n", n, toBits(e), toBits(d));
  n = trio_sscanf("9223372036854775807 x", "%lld %c", &ll, &ch);
  printf("scan long %d: %lld %c\n", n, ll, ch);
  n = trio_sscanf("hello world", "%s %n", word, &consumed);
  printf("scan string %d: [%s] %d\n", n, word, consumed);
  n = trio_sscanf("abcabcxyz rest", "%[abc]%[^ ] %s", set, word, rest);
  printf("scan sets %d: [%s] [%s] [%s]\n", n, set, word, rest);
  n = trio_sscanf("key=value;", "%[^=]=%[^;]", word, set);
  printf("scan pairs %d: [%s] [%s]\n", n, word, set);
  n = trio_sscanf("12345", "%3d%d", &a, &b);
  printf("scan widths %d: %d %d\n", n, a, b);
  n = trio_sscanf("abc", "%d", &a);
  printf("scan mismatch %d\n", n);
  n = trio_sscanf("", "%d", &a);
  printf("scan empty %d\n", n);
  n = trio_sscanf("7 skip 8", "%d %*s %d", &a, &b);
  printf("scan skip %d: %d %d\n", n, a, b);
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
  bytes[*length] = 0;
  fclose(f);
  return bytes;
}

/* A file's numbers (a digit, or a point and a digit, not inside a name), read with trio_sscanf and
   printed as Lua prints them. */
static void file(const char* path)
{
  size_t length;
  unsigned char* bytes = readFile(path, &length);
  if (!bytes)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  unsigned long long hash = FNV_START;
  int numbers = 0;
  for (size_t i = 0; i < length; ++i)
  {
    unsigned char c = bytes[i];
    int starts = (c >= '0' && c <= '9') || (c == '.' && bytes[i + 1] >= '0' && bytes[i + 1] <= '9');
    if (!starts)
      continue;
    if (i > 0)
    {
      unsigned char p = bytes[i - 1];
      if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || p == '_' || (p >= '0' && p <= '9') || p == '.')
        continue;
    }
    double value = 0;
    int consumed = 0;
    if (trio_sscanf((const char*)bytes + i, "%lf%n", &value, &consumed) != 1 || consumed <= 0)
      continue;
    char buffer[512];
    int n = trio_snprintf(buffer, sizeof buffer, "%.14g|%.17g|%d|%a", value, value, (int)value, value);
    hash = fnv1a(hash, buffer, strlen(buffer));
    hash = fnv1a(hash, &n, sizeof n);
    ++numbers;
    i += consumed - 1;
  }
  printf("%s: %u bytes, %d numbers, %016llx\n", path, (unsigned)length, numbers, hash);
  free(bytes);
}

int main(int argc, char** argv)
{
  integers();
  strings();
  doubles();
  randomDoubles();
  others();
  scanning();
  if (argc > 1)
  {
    FILE* list = fopen(argv[1], "r");
    char line[1024];
    while (list && fgets(line, sizeof line, list))
    {
      line[strcspn(line, "\r\n")] = 0;
      if (line[0])
        file(line);
    }
    if (list)
      fclose(list);
  }
  return 0;
}
