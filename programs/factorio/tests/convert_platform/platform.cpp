// The converter's test of the platform dimension (CONVERSION.md TODO 17): the game is converted as
// Windows and as Linux, and the two made one by config::platform, as the configurations are by
// config::debug (design.md 12). What this file prints compiled as C++ for a platform
// (convert_platform.expected for Windows, convert_platform.linux.expected for Linux) is what its
// conversion prints built for that platform.
#include <cstdio>
#include <cstdint>

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
  return 0;
}
