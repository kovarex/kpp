/* The C side of c_union_members_class_its_eightbytes.kpp: unions by value, as clang's C passes them. */
typedef struct { int i; float f[2]; } Triple;
typedef union { Triple t; short s; unsigned long long u; } Mixed;
typedef union { float f; double d; } Floats;
typedef union { float f[4]; double d[2]; } Sse2;
typedef union { float f[2]; int i; } Forced;
typedef struct { Mixed m; } Wrapped;
float cUpperFloat(Mixed m, double d) { return m.t.f[1] + (float)d; }
Mixed cMakeMixed(float upper) { Mixed m; m.u = 7; m.t.f[1] = upper; return m; }
double cFloats(Floats a, Sse2 b) { return a.d + b.d[1]; }
Sse2 cMakeSse2(double x) { Sse2 s; s.d[0] = x; s.d[1] = x * 2; return s; }
int cForced(Forced a, float b) { return a.i + (int)b; }
float cWrapped(Wrapped w, float x) { return w.m.t.f[1] + x; }
extern float kppUpperFloat(Mixed m, double d);
float cCallsKpp(void) { Mixed m; m.u = 1; m.t.f[1] = 2.5f; return kppUpperFloat(m, 1.0); }
