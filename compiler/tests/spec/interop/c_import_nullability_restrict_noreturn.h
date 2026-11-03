/* For interop/c_import_nullability_restrict_noreturn.kpp: clang's nullability qualifiers, C99's restrict
   before a parameter's name and C11's _Noreturn, as FreeBSD's headers have them (stdio.h's FILE,
   pthread.h, stdlib.h's exit). */
struct Callbacks
{
  int (* _Nullable combine)(int, int);
  void * _Nonnull context;
  int (* _Null_unspecified fallback)(void);
  int tail;
};
int sum_values(const int * _Nonnull restrict values, int count);
int apply_callbacks(struct Callbacks * restrict callbacks);
_Noreturn void exit_with(int code);
