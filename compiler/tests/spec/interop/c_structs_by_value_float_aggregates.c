/* The C side of c_structs_by_value_float_aggregates.kpp, compiled by clang for the target: structs of floats
   that AAPCS64 passes in vector registers (up to four of one type, a 24-byte one too), one of five floats
   it passes through memory, a union of floats, a 16-aligned struct, and float aggregates past the eight
   vector registers. */
typedef struct { float a, b, c, d; } F4;
typedef struct { double a, b, c; } D3;
typedef struct { float a[5]; } F5;
typedef union { float f; float g; } U1;
typedef struct { _Alignas(16) long long a; long long b; } A16;
typedef struct { float x, y; } F2;

F4 specF4(F4 v) { F4 r = {v.d, v.c, v.b, v.a}; return r; }
D3 specD3(D3 v) { D3 r = {v.a + v.b, v.b + v.c, v.c + v.a}; return r; }
F5 specF5(F5 v) { F5 r; for (int i = 0; i < 5; ++i) r.a[i] = v.a[4 - i] * 2; return r; }
U1 specU1(U1 v) { U1 r; r.f = v.g + 0.5f; return r; }
A16 specA16(A16 v) { A16 r = {v.b * 3, v.a + 1}; return r; }
/* Four F2 take eight vector registers; the fifth goes on the stack, then a double after it. */
double specManyF2(F2 a, F2 b, F2 c, F2 d, F2 e, double f)
{
  return a.x + b.y * 10 + c.x * 100 + d.y * 1000 + e.x * 10000 + e.y * 100000 + f * 1000000;
}

/* kpp's own C-callable functions, called from here. */
F4 kppF4(F4 v);
D3 kppD3(D3 v);
int specCallsBack(void)
{
  F4 a = {1, 2, 3, 4};
  F4 r = kppF4(a);
  if (r.a != 10 || r.b != 20 || r.c != 30 || r.d != 40)
    return 1;
  D3 b = {1.5, 2.5, 3.5};
  D3 s = kppD3(b);
  if (s.a != 3.5 || s.b != 2.5 || s.c != 1.5)
    return 2;
  return 0;
}
