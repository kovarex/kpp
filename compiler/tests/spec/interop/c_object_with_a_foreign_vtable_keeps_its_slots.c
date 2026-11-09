/* The C side of c_object_with_a_foreign_vtable_keeps_its_slots.kpp: an object with a table of three
   functions, and calls through any object's table (a kpp object's too). */
#include <stdlib.h>
#include "c_object_with_a_foreign_vtable_keeps_its_slots.h"

static int deviceAddRef(SpecDevice* self)
{
  return ++self->count;
}

static int deviceRelease(SpecDevice* self)
{
  return --self->count;
}

static int deviceCreate(SpecDevice* self, int kind)
{
  return 100 * self->count + kind;
}

static const SpecDeviceVtbl deviceVtbl = { deviceAddRef, deviceRelease, deviceCreate };

SpecDevice* specMakeDevice(void)
{
  SpecDevice* d = (SpecDevice*)malloc(sizeof(SpecDevice));
  d->vtbl = &deviceVtbl;
  d->count = 1;
  return d;
}

int specCallCreate(SpecDevice* device, int kind)
{
  return device->vtbl->create(device, kind);
}

int specCallRelease(SpecDevice* device)
{
  return device->vtbl->release(device);
}
