/* For interop/c_function_pointer_struct_by_value.kpp: the functions, and the struct of pointers to them. */
#include "c_function_pointer_struct_by_value.h"

int take_s8(S8 s) { return s.a * 10 + s.b; }
S8 make_s8(int a, int b) { S8 s = {a, b}; return s; }
F16 scale_f16(F16 v, double by) { F16 r = {v.x * by, v.y * by}; return r; }
long long sum_s24(S24 s, int times) { return (s.a + s.b + s.c) * times; }
Ops ops(void) { Ops o = {take_s8, make_s8, scale_f16}; return o; }
