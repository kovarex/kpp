/* The C side of c_narrow_integers_extended.kpp, compiled by clang, which on SysV x86-64 and Apple's arm64
   reads a char, short or _Bool argument from its whole 32-bit register, extended by the caller, and lets
   its caller read a char, short or _Bool result the same way, extended by the callee. */
int specWiden(short s, signed char c, unsigned char u, unsigned short w, _Bool b)
{
  return s * 10000 + c * 100 + u + (w - 65530) * 100000000 + b * 7;
}

/* A result of kpp's functions, through pointers: each used as the whole register it came in. */
int specNarrowResults(short (*s)(int), signed char (*c)(int), unsigned char (*u)(int), _Bool (*b)(int))
{
  return s(0x12348765) + c(0x123456F9) * 100000 + u(0x12345602) * 1000 + b(0x12345600) * 7;
}
