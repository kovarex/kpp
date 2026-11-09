/* A C object whose first word points at a table of three functions, as a COM interface deriving from
   another one (IUnknown's two and its own), for c_object_with_a_foreign_vtable_keeps_its_slots.kpp. */
typedef struct SpecDevice SpecDevice;
typedef struct SpecDeviceVtbl
{
  int (*addRef)(SpecDevice* self);
  int (*release)(SpecDevice* self);
  int (*create)(SpecDevice* self, int kind);
} SpecDeviceVtbl;
struct SpecDevice
{
  const SpecDeviceVtbl* vtbl;
  int count;
};
SpecDevice* specMakeDevice(void);
int specCallCreate(SpecDevice* device, int kind);
int specCallRelease(SpecDevice* device);
