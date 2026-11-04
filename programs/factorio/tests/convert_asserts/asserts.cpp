// Factorio's release_assert (src/Util/ReleaseAssert.hpp) and a statement macro in the do/while form
// (BlueprintLibrary.cpp's blueprintAssert), converted: kpp's own release_assert statement, which
// takes its file, line and expression text itself, with the game's handler still called through
// kpp_assert_failed; the do/while wrapper left out. The converted text is checked by convert.cmd too.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

[[noreturn]] void ReleaseAssertFailed(const char* file, uint32_t line, const char* message);

#define release_assert(value) do { if (!(value)) ReleaseAssertFailed(__FILE__, __LINE__, #value); } while (false)

#define checkedStep(expression)           \
  do                                      \
  {                                       \
    if (!(expression))                    \
    {                                     \
      printf("step failed: %s\n", #expression); \
      exit(2);                            \
    }                                     \
  }                                       \
  while (false)

// A `break` in a do/while macro leaves the do/while alone: this one keeps its wrapper (written
// `while (0)`, which convert.cmd's text check allows), or the break would leave the loop around it.
#define printIfEven(n) do { if ((n) % 2) break; printf("even %d\n", n); } while (0)
// A break of a switch in the body is the switch's: this one goes without its wrapper.
#define printKind(n) do { switch (n) { case 0: printf("zero\n"); break; default: printf("other\n"); break; } } while (false)

void ReleaseAssertFailed(const char* file, uint32_t line, const char* message)
{
  printf("release_assert failed: %s (%s)\n", message, line > 0 && file[0] ? "with its place" : "without");
  fflush(stdout);
  exit(0);
}

struct Holder
{
  int value = 3;
  void check() const { release_assert(this->value == 3); release_assert(value > 0); }
};

int main()
{
  Holder holder;
  holder.check();
  int a = 1;
  if (a == 1)
    release_assert(a > 0);
  else
    release_assert(a < 0);
  if (a == 1)
    checkedStep(a + 1 == 2);
  else
    checkedStep(a == 7);
  for (int i = 0; i < 3; ++i)
    printIfEven(i);
  printKind(0);
  printKind(a);
  printf("passed\n");
  release_assert(a == 2);
  return 1;
}
