/* For interop/c_function_pointer_struct_by_value.kpp: C functions that take and return structs by value, of
   the sizes each target's C passes differently (8 bytes: one register everywhere; 16 bytes of doubles: two
   SSE registers on SysV, a pointer to a copy on MS x64; 24 bytes: memory on SysV, a pointer on MS x64), a
   header's pointer type for one, and a struct of pointers to them that C fills in. */
typedef struct S8 { int a, b; } S8;
typedef struct F16 { double x, y; } F16;
typedef struct S24 { long long a, b, c; } S24;
int take_s8(S8 s);
S8 make_s8(int a, int b);
F16 scale_f16(F16 v, double by);
long long sum_s24(S24 s, int times);
typedef int (*TakeS8)(S8 s);
typedef struct Ops { TakeS8 take; S8 (*make)(int a, int b); F16 (*scale)(F16 v, double by); } Ops;
Ops ops(void);
