/* The rules where C and C++ differ, one function each, in what C code that Factorio links relies on
   (libraries\vorbis, ogg, zlib, trio, minizip, curl, SDL3: "as C++" errors of
   docs/path-to-factorio.md's C libraries). Converted as C (--convert's C mode), built and run, it
   prints what its C build prints (tests\convert_c.expected). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A void* converts to any object pointer without a cast: initialized, assigned, passed and returned
   (vorbis's `float *lpc=alloca(...)`, ogg's `os->body_data=_ogg_realloc(...)`). */
static int sumInts(const int* values, int count)
{
  int sum = 0;
  for (int i = 0; i < count; ++i)
    sum += values[i];
  return sum;
}
static int* makeInts(int count)
{
  void* block = malloc(count * sizeof(int));
  return block;
}
static void voidPointers(void)
{
  int* a = malloc(4 * sizeof(int));
  for (int i = 0; i < 4; ++i)
    a[i] = i + 1;
  void* untyped = a;
  int* b;
  b = untyped;
  int* c = makeInts(2);
  c[0] = 10;
  c[1] = 20;
  printf("void pointers: %d %d %d\n", b[3], sumInts(untyped, 4), sumInts(c, 2));
  free(a);
  free(c);
}

/* An int converts to an enum and back without a cast; an enum's arithmetic is int's. */
enum Color { RED, GREEN, BLUE };
static enum Color next(enum Color color) { return color + 1; }
static void enums(void)
{
  enum Color color = 1;
  int n = color + 1;
  color = n;
  enum Color after = next(GREEN);
  color++;
  printf("enums: %d %d %d %d\n", (int)color, n, (int)after, (int)sizeof(enum Color));
}

/* Braced initializers convert as assignment does: no narrowing error, the value wraps or truncates. */
static void narrowing(int big)
{
  unsigned char bytes[3] = {-1, 300, big};
  short shorts[2] = {big, 70000};
  float floats[2] = {big, 1.5};
  int ints[2] = {2.9, -2.9};
  printf("narrowing: %d %d %d %d %d %g %g %d %d\n", bytes[0], bytes[1], bytes[2], shorts[0], shorts[1], floats[0], floats[1],
         ints[0], ints[1]);
}

/* C++'s keywords are ordinary names in C (trio's `class`, curl's and SDL's `new`, `this`,
   `template`, `delete`, `private`). */
struct Keywords
{
  int class;
  int new;
  int this;
};
static int keywords(int template, int delete)
{
  struct Keywords private = {1, 2, 3};
  int namespace = private.class + private.new + private.this;
  int typename = template * delete;
  return namespace + typename;
}

/* `register` is a storage class C keeps (C++17 removed it). */
static int registers(int n)
{
  register int total = 0;
  for (register int i = 0; i < n; ++i)
    total += i;
  return total;
}

/* Tentative definitions: a file-scope object declared without an initializer, then defined. */
int counter;
int counter = 5;
static int table[];
static int table[3] = {7, 8, 9};

/* Designated initializers in any order, array designators, nested ones; the rest is zero. */
struct Point { int x, y; };
struct Box { struct Point corner; int size; const char* name; };
static void designated(void)
{
  struct Point p = {.y = 2, .x = 1};
  int sparse[6] = {[4] = 40, [1] = 10, 11};
  struct Box box = {.size = 3, .corner.y = 9};
  printf("designated: %d %d %d %d %d %d %d %d %d %s\n", p.x, p.y, sparse[0], sparse[1], sparse[2], sparse[4], box.corner.x,
         box.corner.y, box.size, box.name ? box.name : "null");
}

/* Compound literals: an unnamed object of a given type, an lvalue that lives until the block ends. */
static int area(const struct Point* p) { return p->x * p->y; }
static void compoundLiterals(void)
{
  struct Point q = (struct Point){3, 4};
  int* list = (int[]){5, 6, 7};
  list[0] += 1;
  printf("compound literals: %d %d %d %d\n", q.x + q.y, list[0] + list[2], area(&(struct Point){.x = 2, .y = 8}),
         ((struct Point){1, 1}).y);
}

/* Struct tags have a namespace of their own; a struct or enum declared in a struct is at file
   scope (C++ nests it). */
struct stat_like { int a; };
int stat_like = 3;
struct Outer
{
  struct Inner { int z; } in;
  enum Mode { MODE_A = 4, MODE_B } mode;
};
static void tagsAndNesting(void)
{
  struct stat_like s = {stat_like};
  struct Inner standalone = {6};
  enum Mode mode = MODE_B;
  struct Outer outer = {{7}, MODE_A};
  printf("tags: %d %d %d %d %d\n", s.a, standalone.z, (int)mode, outer.in.z, (int)outer.mode);
}

/* The types of expressions: a character literal and a comparison are int in C. */
static void expressionTypes(void)
{
  printf("types: %d %d %d %d\n", (int)sizeof('a'), (int)sizeof(1 < 2), (int)sizeof(!0), (int)sizeof(RED));
}

/* A function declared with () takes unspecified arguments; its definition gives them. */
static int unspecified();
static int unspecified(int a, int b) { return a * 10 + b; }

/* A string literal is a char array: a char* takes it, and an array exactly its length takes its
   characters without the terminator. */
static void strings(void)
{
  char* text = "abc";
  char exact[3] = "xyz";
  printf("strings: %s %c%c%c %d\n", text, exact[0], exact[1], exact[2], (int)sizeof(exact));
}

/* _Bool: any nonzero value is 1. */
static void booleans(void)
{
  _Bool b = 5;
  _Bool z = 0.0;
  _Bool p = "x";
  printf("booleans: %d %d %d %d\n", b, z, p, (int)sizeof(_Bool));
}

/* A goto may jump past a declaration with an initializer (not into a VLA's scope); the object is
   then uninitialized (curl and zlib jump to cleanup labels this way). */
static int jumpOver(int fail)
{
  int result = 1;
  if (fail)
    goto done;
  int doubled = result * 2;
  result = doubled + 1;
done:
  return result;
}

/* Anonymous structs and unions as members (C11). */
struct Value
{
  int kind;
  union
  {
    int i;
    float f;
  };
  struct
  {
    short lo, hi;
  };
};
static void anonymous(void)
{
  struct Value v;
  memset(&v, 0, sizeof v);
  v.f = 1.0f;
  v.lo = -2;
  v.hi = 3;
  printf("anonymous: %08x %d %d %d\n", (unsigned)v.i, v.lo, v.hi, (int)sizeof v);
}

/* A const qualifier dropped by an implicit conversion: C warns and converts (minizip, curl). */
static char* dropConst(const char* text) { return text; }

int main(void)
{
  voidPointers();
  enums();
  narrowing(258);
  printf("keywords: %d\n", keywords(4, 5));
  printf("register: %d\n", registers(10));
  printf("tentative: %d %d %d\n", counter, table[0], table[2]);
  designated();
  compoundLiterals();
  tagsAndNesting();
  expressionTypes();
  printf("unspecified: %d\n", unspecified(4, 2));
  strings();
  booleans();
  printf("goto: %d %d\n", jumpOver(1), jumpOver(0));
  anonymous();
  printf("const dropped: %c\n", dropConst("q")[0]);
  return 0;
}
