/* The C side of c_struct_bit_fields_match_c.kpp: structs with bit fields of mixed types, laid out as the
   target's C lays them out (MSVC's rules on Windows, SysV's elsewhere): their sizes, alignments and the
   places of their members after the bit fields; fields C writes for kpp to read, and fields kpp wrote
   for C to read. */
#include <stddef.h>

struct Mixed
{
  char a : 3;
  int b : 5;
  long long c : 40;
  short d : 9;
  unsigned char e;
  int f : 7;
  unsigned g : 1;
  int : 0;
  char h;
};

struct Narrow
{
  char a : 3;
  short : 0;
  char b : 2;
  char c : 2;
  short tail;
};

int cSizeMixed(void) { return (int)sizeof(struct Mixed); }
int cAlignMixed(void) { return (int)_Alignof(struct Mixed); }
int cOffsetE(void) { return (int)offsetof(struct Mixed, e); }
int cOffsetH(void) { return (int)offsetof(struct Mixed, h); }
int cSizeNarrow(void) { return (int)sizeof(struct Narrow); }
int cOffsetTail(void) { return (int)offsetof(struct Narrow, tail); }

void cFill(struct Mixed* m)
{
  m->a = -3;
  m->b = 11;
  m->c = -123456789012LL;
  m->d = -200;
  m->e = 250;
  m->f = -50;
  m->g = 1;
  m->h = 'h';
}

long long cField(const struct Mixed* m, int which)
{
  switch (which)
  {
    case 0: return m->a;
    case 1: return m->b;
    case 2: return m->c;
    case 3: return m->d;
    case 4: return m->e;
    case 5: return m->f;
    case 6: return m->g;
    default: return m->h;
  }
}

long long cNarrow(const struct Narrow* n) { return n->a * 1000 + n->b * 100 + n->c * 10 + n->tail * 10000; }
