/* The C side of c_object_with_vtable.kpp: an object with a table of functions, and calls through
   any object's table (a kpp object's too). */
#include <stdlib.h>
#include "c_object_with_vtable.h"

static int counterAdd(SpecCounter* self, int n)
{
  self->value += n;
  return self->value;
}

static int counterGet(SpecCounter* self)
{
  return self->value;
}

static const SpecCounterVtbl counterVtbl = { counterAdd, counterGet };

SpecCounter* specMakeCounter(int start)
{
  SpecCounter* c = (SpecCounter*)malloc(sizeof(SpecCounter));
  c->vtbl = &counterVtbl;
  c->value = start;
  return c;
}

int specCallAdd(SpecCounter* counter, int n)
{
  return counter->vtbl->add(counter, n);
}

int specCallGet(SpecCounter* counter)
{
  return counter->vtbl->get(counter);
}
