// The converter's test of the platform dimension (CONVERSION.md TODO 17): the game is converted as
// Windows and as Linux, and the two made one by config::platform, as the configurations are by
// config::debug (design.md 12). What this file prints compiled as C++ for a platform
// (convert_platform.expected for Windows, convert_platform.linux.expected for Linux) is what its
// conversion prints built for that platform.
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <cstdlib>

// One spelling on every platform (design.md 12): a string macro defined per platform, written out at
// its uses with the literals it joins; a declaration whose string initializer differs, beside one that
// does not; errno and alloca, each a C header's macro of its own on each platform.
#if defined(_WIN32)
#include <malloc.h>
#define KEY_NAME "windows"
#define OS_NAME "win64"
static const char* platformTag = "win64";
static int tagCount = 1;
#else
#include <alloca.h>
#define KEY_NAME "super"
#define OS_NAME "linux64"
static const char* platformTag = "linux64";
static int tagCount = 1;
#endif

static const char* const keyName = "left-" KEY_NAME;

// Groups where no statement stands (a link of a call chain, a constructor's initializer): each
// platform's whole statement.
struct Chain
{
  Chain& operator()(const char* s)
  {
    printf("%s ", s);
    return *this;
  }
};

struct Scaled
{
  int a;
  int b;
  Scaled()
    : a(1)
#ifdef _WIN32
    , b(2)
#else
    , b(3)
#endif
  {
  }
};

#if defined(_WIN32)
static const char* platformName() { return "windows"; }
#elif defined(__linux__)
static const char* platformName() { return "linux"; }
#endif

struct Settings
{
#ifdef _WIN32
  int windowsOnly = 1;
#else
  long posixOnly = 2;
#endif
  int shared = 3;
};

static int pathSeparator()
{
#ifdef _WIN32
  return '\\';
#else
  return '/';
#endif
}

#if defined(_WIN32) && defined(DEBUG)
static const char* build() { return "windows debug"; }
#elif defined(_WIN32)
static const char* build() { return "windows release"; }
#elif defined(DEBUG)
static const char* build() { return "linux debug"; }
#else
static const char* build() { return "linux release"; }
#endif

int main()
{
  Settings settings;
  printf("%s %c %d\n", platformName(), pathSeparator(), settings.shared);
#ifdef _WIN32
  printf("only windows: %d\n", settings.windowsOnly);
#else
  printf("only linux: %ld\n", settings.posixOnly);
#endif
  printf("%s, long is %d bytes\n", build(), int(sizeof(long)));
  Chain chain;
  chain("a")
#ifdef _WIN32
    ("w")
#else
    ("l")
#endif
    ("z");
  printf("%d\n", Scaled().a + Scaled().b);
  // `x < width >> 1` is `x < (width >> 1)` (kpp would read `x<width>` as a template-id)
  struct Image { int width; } image = {12};
  int rows = 0;
  for (int x = 0; x < image.width >> 1; ++x)
    ++rows;
  printf("%d\n", rows);
  int* scratch = (int*)alloca(sizeof(int) * 4);
  scratch[0] = 7;
  errno = 0;
  strtol("99999999999999999999999", nullptr, 10);
  printf("%s %s %s %d %d %s\n", keyName, OS_NAME, platformTag, tagCount, scratch[0], errno == ERANGE ? "ERANGE" : "no ERANGE");
  return 0;
}
