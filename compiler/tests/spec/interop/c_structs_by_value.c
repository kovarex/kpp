// The C side of c_structs_by_value.kpp: functions that take and return structs by value, of every
// size and register class (MS x64: 1, 2, 4 or 8 bytes in a register, others through memory; SysV:
// up to 16 bytes in INTEGER or SSE registers, others in memory), compiled by clang for the target.
typedef struct { char a; } S1;
typedef struct { short a; } S2;
typedef struct { char a, b, c; } S3;
typedef struct { int a; } S4;
typedef struct { int a, b; } S8;
typedef struct { float x, y; } F2;
typedef struct { double x; } D1;
typedef struct { int a, b, c; } S12;
typedef struct { double x; int y; } DI;
typedef struct { float x, y, z; } F3;
typedef struct { double x, y; } D2;
typedef struct { int a; float b; } IF;
typedef struct { long long a, b, c; } S24;

int takeS1(S1 s) { return s.a; }
int takeS2(S2 s) { return s.a; }
int takeS3(S3 s) { return s.a * 10000 + s.b * 100 + s.c; }
int takeS4(S4 s) { return s.a; }
int takeS8(S8 s) { return s.a * 1000 + s.b; }
double takeF2(F2 s) { return s.x * 10 + s.y; }
double takeD1(D1 s) { return s.x; }
int takeS12(S12 s) { return s.a * 10000 + s.b * 100 + s.c; }
double takeDI(DI s) { return s.x * 100 + s.y; }
double takeF3(F3 s) { return s.x * 100 + s.y * 10 + s.z; }
double takeD2(D2 s) { return s.x * 10 + s.y; }
double takeIF(IF s) { return s.a * 10 + s.b; }
long long takeS24(S24 s) { return s.a * 10000 + s.b * 100 + s.c; }
// Registers running out: five integers before a struct of two INTEGER eightbytes leave one register,
// so on SysV the whole struct goes on the stack; the int after it still takes the last register.
long long afterFive(int a, int b, int c, int d, int e, S12 s, int f) { return (a + b + c + d + e) * 1000000LL + s.a * 10000 + s.b * 100 + s.c + f * 1000000000LL; }
// Seven doubles leave one SSE register: a struct of two doubles goes on the stack.
double afterSeven(double a, double b, double c, double d, double e, double f, double g, D2 s) { return a + b + c + d + e + f + g + s.x * 1000 + s.y * 100; }
// Mixed with scalars around it, and through a variadic call's fixed part.
double mixed(int a, DI s, float b, S8 t) { return a + s.x * 10 + s.y + b * 1000 + t.a * 10000 + t.b * 100000; }

S1 makeS1(int a) { S1 s = {(char)a}; return s; }
S3 makeS3(int a) { S3 s = {(char)a, (char)(a + 1), (char)(a + 2)}; return s; }
S8 makeS8(int a) { S8 s = {a, a * 2}; return s; }
F2 makeF2(float x) { F2 s = {x, x * 2}; return s; }
S12 makeS12(int a) { S12 s = {a, a + 1, a + 2}; return s; }
DI makeDI(double x) { DI s = {x, (int)x * 3}; return s; }
F3 makeF3(float x) { F3 s = {x, x + 1, x + 2}; return s; }
D2 makeD2(double x) { D2 s = {x, x / 2}; return s; }
IF makeIF(int a) { IF s = {a, a * 0.5f}; return s; }
S24 makeS24(long long a) { S24 s = {a, a + 1, a + 2}; return s; }
