// A switch's cases converted for no implicit fallthrough (kpp's design.md "No implicit fallthrough"): every
// group of statements after a label ends with a break, so the `break;` C++ ends one with goes, on a line of its
// own, after a statement on the label's line, as a block's last statement (also before a stray `;`) and before
// a comment (which stays), also as a group's only statement (`case 4: break;` becomes `case 4:`, which runs
// nothing); a `break;` that leaves early (in an if, in a loop of the case) stays; C++'s running on into the next
// case is `fallthrough;`, after the block's brace when it ends a block; labels side by side, which share their
// statements in C++, are one label listing their values (`case 0, 1:`, each label runs only its own in kpp), the
// comments between them above it, and a label that cannot be joined to the next (a default, a label a macro
// writes, #if lines between) gets `fallthrough;`. Converted and run, it prints what its C++ build prints
// (tests\convert_switch.expected), and convert.cmd counts the `break;`s left in its text (4: cases 6 and 7, the
// loops' macros' two) and its `fallthrough;`s (8: classify's cases 8, 10 and 13, shared's 8, 11 and 12, and
// joined's default and case 60).
#include <cstdio>

int classify(int x)
{
  int r = 0;
  switch (x)
  {
    case 0:
      r = 10;
      break;
    case 1: r = 11; break;
    case 2:
    {
      int twice = x * 2;
      r = twice + 10;
      break;
    }
    case 3:
      r = 13;
      break; // done with three
    case 4:
      break;
    case 5:
    {
      break;
    }
    case 6:
      if (r == 0)
        break;
      r = 16;
      break;
    case 7:
      for (int i = 0;; ++i)
        if (i == 3)
        {
          r = 17 + i;
          break;
        }
      break;
    case 8:
      r = 18;
      [[fallthrough]];
    case 9:
      r += 1;
      break;
    case 10:
      r = 20;
    case 11:
      r += 2;
      break;
    case 12:
      return 22;
    case 13:
    {
      r = 23;
      [[fallthrough]];
    }
    case 14:
      r += 1;
      break;
    default:
      break;
  }
  return r;
}

int lastGroup(int x)
{
  int r = 0;
  switch (x)
  {
    case 0:
      r = 1;
      break;
    default:
    {
      r = 2;
      break;
    };
  }
  return r;
}

// Cases a macro writes: an X-macro's `case ...: ...; break;` before another, Lua's vmcase, a switch inside a
// do/while statement macro; and breaks a macro writes into a loop, which stay.
#define ONE(value, label) case label: r = value; break;
#define vmcase(l, b) case l: { b; } break;
#define ROUTE(t) do { switch (t) { case 0: r += 100; break; case 1: r += 200; break; } } while (0)
#define STOP_IF(c) if (c) break;
#define STEP(x) x += 1; if (x > 5) break;

int macros(int x)
{
  int r = 0;
  switch (x)
  {
    ONE(10, 1)
    ONE(20, 2)
    vmcase(3, r = 30)
    vmcase(4, r = 40)
    case 5:
      ROUTE(x - 5);
      break;
    case 6:
      ROUTE(x - 5);
      break;
  }
  return r;
}

int loops(int n)
{
  int i = 0;
  while (true)
  {
    STOP_IF(i == n)
    ++i;
  }
  int j = 0;
  for (;;)
  {
    STEP(j)
  }
  return i * 10 + j;
}

// A macro whose `break` takes the use's `;` (Agui's HANDLE_CASE), one whose case is a block ending with the break
// (InputAction's CALL_DESTRUCTOR), a switch a macro spells (Lua's vmdispatch), labels a macro writes before the
// source's loop body and break (stb_image's STBI__CASE).
#define HANDLE(c, v) case c: r = v; break
#define DESTROY(c, v) case c: { r = v; break; }
#define dispatch(o) switch (o)
#define ROW(a) case a: for (int i = 0; i < a; ++i)
int moreMacros(int x)
{
  int r = 0;
  switch (x)
  {
    case 0: break;
    HANDLE(1, 10);
    HANDLE(2, 20);
    DESTROY(3, 30)
    DESTROY(4, 40)
  }
  dispatch (x)
  {
    vmcase(5, r = 50)
    vmcase(6, r = 60)
  }
  switch (x)
  {
    ROW(7) { r += i; } break;
    ROW(8) { r += 2 * i; } break;
    default: break;
  }
  return r;
}

// Cases a macro writes that end with a return (NetworkMessage's clone): no `fallthrough;` after them.
#define RET(c, v) case c: return v;
int returns(int x)
{
  switch (x)
  {
    RET(1, 100)
    RET(2, 200)
  }
  return 0;
}

// A macro's case that is a block ending with a return (UnparsedNetworkMessage's PARSE), and an X-macro list
// whose element macro is undefined before the next label (EditorAction's CALL_DESTRUCTOR).
#define PARSE(c, v) case c: { if (v > 0) return v; return -1; }
int parsed(int x)
{
  switch (x)
  {
    PARSE(1, 7)
    PARSE(2, 0)
  }
  return 0;
}

#define LIST(M) M(1) M(2)
#define SET(c) case c: r = c * 3; break;
int listed(int x)
{
  int r = 0;
  switch (x)
  {
    LIST(SET)
#undef SET
    default:
      r = -1;
  }
  return r;
}

// Labels side by side, on lines of their own, on one line, with comments between, in a macro's expansion, before
// a label a macro writes, before default, beside #if lines (trio's); and a case of a break alone, which runs nothing.
#define PAIR(a, b, v) case a: case b: r = v; break;
int shared(int x)
{
  int r = 0;
  switch (x)
  {
    case 0:
    case 1:
      r = 1;
      break;
    case 2: case 3: r = 3; break;
    case 4: // four
    // and five
    case 5:
      r = 5;
      break;
    PAIR(6, 7, 7)
    case 8:
    ONE(9, 9)
    case 10:
      break;
    case 11:
#ifdef SWITCH_NEVER
    case 99:
#endif
#ifndef SWITCH_NEVER
    case 12:
#endif
    default:
      r = 12;
  }
  return r;
}

// Runs of labels joined: three, with comments of both kinds after their labels; C++'s [[fallthrough]] between two
// (joined, it goes) and before default (kept); default before labels; a run after the switch's `{` on its line.
int joined(int x)
{
  int r = -1;
  switch (x)
  {
    case 20: // twenty
    case 21: /* twenty-one */ case 22:
    // twenty-three
    case 23:
      r = 23;
      break;
    case 50:
      [[fallthrough]];
    case 51:
      r = 51;
      break;
    default:
    case 30:
    case 31:
      r = 31;
  }
  switch (x) { case 40: // forty
    case 41: r = 41; break; case 60: [[fallthrough]]; default: if (x == 60) r = 60; }
  return r;
}

int main()
{
  for (int x = 0; x <= 15; ++x)
    printf("%d %d\n", x, classify(x));
  printf("last %d %d\n", lastGroup(0), lastGroup(5));
  for (int x = 0; x <= 7; ++x)
    printf("macro %d %d\n", x, macros(x));
  printf("loops %d\n", loops(3));
  for (int x = 0; x < 10; ++x)
    printf("more %d %d\n", x, moreMacros(x));
  printf("returns %d %d %d\n", returns(0), returns(1), returns(2));
  printf("parsed %d %d %d\n", parsed(0), parsed(1), parsed(2));
  printf("listed %d %d %d %d\n", listed(0), listed(1), listed(2), listed(3));
  for (int x = 0; x <= 12; ++x)
    printf("shared %d %d\n", x, shared(x));
  int joins[] = {20, 21, 22, 23, 24, 30, 31, 40, 41, 50, 51, 60};
  for (int x : joins)
    printf("joined %d %d\n", x, joined(x));
  return 0;
}
