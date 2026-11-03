/* The C side of c_import_member_alignment_attribute.kpp: the struct as clang lays it out, and its
   members read at clang's places. */
#include <stddef.h>
#include "c_import_member_alignment_attribute.h"

unsigned long long c_layout(int which)
{
  switch (which)
  {
    case 0: return sizeof(struct AlignedTail);
    case 1: return _Alignof(struct AlignedTail);
    case 2: return offsetof(struct AlignedTail, block);
    default: return offsetof(struct AlignedTail, tail);
  }
}

long long c_sum(const struct AlignedTail* a)
{
  return a->first + a->block[0] + a->block[7] + a->tail;
}
