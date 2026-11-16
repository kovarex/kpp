// The game's build variants (fbuild.bff's Steam, demo, headless and vanilla builds): each #if on
// STEAM, DEMO, HEADLESS, DISABLE_UPDATER or SPACE_AGE converted with its define and without, merged by
// --convert-merge-flag into one kpp source whose config constants choose (convert.cmd's variants check).
#include <cstdio>

#ifdef STEAM
#define BUILD_MODE "steam"
#elif defined(DEMO)
#define BUILD_MODE "demo"
#else
#define BUILD_MODE "standalone"
#endif

class Context
{
public:
  int ticket = 7;
#ifdef STEAM
  int steamId = 76561;
#endif
#ifndef HEADLESS
  int windowWidth = 1920;
#endif
};

#ifndef DISABLE_UPDATER
static int updaterChecks()
{
  return 3;
}
#endif

// (a link of an if-else chain that one variant alone has: joined with its constant, the chain once)
static int choose(int x)
{
  if (x == 0)
    return 0;
#ifndef DEMO
  else if (x == 1)
    return 1;
#endif
  else
    return 2;
}

// (links of one variant inside another's: the headless merge joins the inner ones, then the demo merge
// joins both sides' links, an `if constexpr` block never starting with `else`)
static int nested(int x)
{
  if (x == 0)
    return 0;
#ifndef DEMO
#ifndef HEADLESS
  else if (x == 2)
    return 2;
#endif
  else if (x == 1)
    return 1;
#endif
  else
    return 3;
}

static bool isSpaceAge()
{
#ifdef SPACE_AGE
  return true;
#else
  return false;
#endif
}

int main()
{
  Context context;
  printf("mode %s, ticket %d\n", BUILD_MODE, context.ticket);
#ifdef STEAM
  printf("steam id %d\n", context.steamId);
#endif
#ifndef HEADLESS
  printf("window %d\n", context.windowWidth);
#else
  printf("headless\n");
#endif
#ifndef DEMO
  printf("full game\n");
#endif
#ifndef DISABLE_UPDATER
  printf("updater checks %d\n", updaterChecks());
#endif
  printf("space age %d, choice %d\n", isSpaceAge() ? 1 : 0, choose(1));
  printf("nested %d %d\n", nested(1), nested(2));
  return 0;
}
