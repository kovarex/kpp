/* The C side of c_callable_structs_by_value.kpp: calls the C-callable functions kpp defines with structs
   by value of every size and register class, as clang's C passes them for the target. Returns 0, or the
   number of the first check that failed. */
typedef struct { char a; } S1;
typedef struct { char a, b, c; } S3;
typedef struct { int a, b; } S8;
typedef struct { float x, y; } F2;
typedef struct { double x; } D1;
typedef struct { int a, b, c; } S12;
typedef struct { double x; int y; } DI;
typedef struct { float x, y, z; } F3;
typedef struct { double x, y; } D2;
typedef struct { int a; float b; } IF;
typedef struct { long long a, b, c; } S24;

int kppTakeS1(S1 s);
int kppTakeS3(S3 s);
int kppTakeS8(S8 s);
double kppTakeF2(F2 s);
double kppTakeD1(D1 s);
int kppTakeS12(S12 s);
double kppTakeDI(DI s);
double kppTakeF3(F3 s);
double kppTakeD2(D2 s);
double kppTakeIF(IF s);
long long kppTakeS24(S24 s);
long long kppAfterFive(int a, int b, int c, int d, int e, S12 s, int f);
double kppAfterSeven(double a, double b, double c, double d, double e, double f, double g, D2 s);
double kppMixed(int a, DI s, float b, S8 t);
S1 kppMakeS1(int a);
S3 kppMakeS3(int a);
S8 kppMakeS8(int a);
F2 kppMakeF2(float x);
S12 kppMakeS12(int a);
DI kppMakeDI(double x);
F3 kppMakeF3(float x);
D2 kppMakeD2(double x);
S24 kppMakeS24(long long a);

#define CHECK(condition) do { ++n; if (!(condition)) return n; } while (0)

int checkKppCallables(void)
{
  int n = 0;
  CHECK(kppTakeS1((S1){7}) == 7);
  CHECK(kppTakeS3((S3){1, 2, 3}) == 10203);
  CHECK(kppTakeS8((S8){4, 5}) == 45);
  CHECK(kppTakeF2((F2){1.5f, 2.25f}) == 3.75);
  CHECK(kppTakeD1((D1){2.5}) == 2.5);
  CHECK(kppTakeS12((S12){1, 2, 3}) == 123);
  CHECK(kppTakeDI((DI){1.5, 2}) == 3.5);
  CHECK(kppTakeF3((F3){1, 2, 4}) == 7);
  CHECK(kppTakeD2((D2){1.25, 2.5}) == 3.75);
  CHECK(kppTakeIF((IF){3, 0.5f}) == 3.5);
  CHECK(kppTakeS24((S24){1, 2, 3}) == 123);
  CHECK(kppAfterFive(1, 2, 3, 4, 5, (S12){6, 7, 8}, 9) == 702);
  CHECK(kppAfterSeven(1, 2, 3, 4, 5, 6, 7, (D2){8, 9}) == 117);
  CHECK(kppMixed(1, (DI){2.5, 3}, 4.5f, (S8){5, 6}) == 67);
  S1 s1 = kppMakeS1(9);
  CHECK(s1.a == 9);
  S3 s3 = kppMakeS3(4);
  CHECK(s3.a == 4 && s3.b == 5 && s3.c == 6);
  S8 s8 = kppMakeS8(7);
  CHECK(s8.a == 7 && s8.b == 8);
  F2 f2 = kppMakeF2(1.5f);
  CHECK(f2.x == 1.5f && f2.y == 2.5f);
  S12 s12 = kppMakeS12(1);
  CHECK(s12.a == 1 && s12.b == 2 && s12.c == 3);
  DI di = kppMakeDI(2.5);
  CHECK(di.x == 2.5 && di.y == 3);
  F3 f3 = kppMakeF3(1);
  CHECK(f3.x == 1 && f3.y == 2 && f3.z == 3);
  D2 d2 = kppMakeD2(0.5);
  CHECK(d2.x == 0.5 && d2.y == 1.5);
  S24 s24 = kppMakeS24(7);
  CHECK(s24.a == 7 && s24.b == 8 && s24.c == 9);
  return 0;
}
