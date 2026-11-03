/* Struct members as FreeBSD 14.3's headers write them (c_struct_member_attributes.kpp): an aligned
   array after other members (machine/ucontext.h's mc_fpstate, machine/signal.h's sc_fpstate) and
   _Nullable function pointers (stdio.h's struct __sFILE). Every member after them counts. */
struct SpecAlignedMember
{
  int before;
  long state[4] __attribute__((__aligned__(16)));
  int after;
};
struct SpecNullableMember
{
  void* cookie;
  int (* _Nullable close)(void*);
  int after;
};
unsigned long specAlignedSize(void);
unsigned long specAlignedAfter(void);
unsigned long specNullableSize(void);
unsigned long specNullableAfter(void);
