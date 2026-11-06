#include "c_pragma_pack_forms.h"
size_t cOffsets(int which)
{
  switch (which)
  {
    case 0: return sizeof(Labelled) * 100 + offsetof(Labelled, v);
    case 1: return sizeof(Inner) * 100 + offsetof(Inner, v);
    case 2: return sizeof(Changed) * 100 + offsetof(Changed, d);
    case 3: return sizeof(Natural) * 100 + offsetof(Natural, d);
    case 4: return sizeof(ThroughPragma) * 100 + offsetof(ThroughPragma, v);
    case 5: return sizeof(Mixed) * 100 + _Alignof(Mixed);
    case 6: return sizeof(Holder) * 100 + offsetof(Holder, natural);
    case 7: return sizeof(Bits) * 100 + offsetof(Bits, d);
    case 8: return sizeof(Outside) * 100 + offsetof(Outside, array);
    case 9: return sizeof(Wide) * 100 + offsetof(Wide, d);
    default: return sizeof(AfterExtraPop) * 100 + _Alignof(Holder);
  }
}
void cFill(Holder* h, Changed* c)
{
  h->nested.v = 0x01020304;
  h->natural.d = 2.5;
  c[1].d = 3.5;
}
