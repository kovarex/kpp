/* For interop/c_import_member_alignment_attribute.kpp: a member aligned by an attribute written after its
   declarator, as FreeBSD's machine/ucontext.h has mcontext_t's mc_fpstate. */
struct AlignedTail
{
  long long first;
  long long block[8] __attribute__((__aligned__(16)));
  int tail;
};
unsigned long long c_layout(int which);
long long c_sum(const struct AlignedTail* a);
