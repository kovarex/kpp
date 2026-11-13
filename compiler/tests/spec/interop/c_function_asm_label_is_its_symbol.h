/* For interop/c_function_asm_label_is_its_symbol.kpp: a function the header gives another symbol (an asm
   label), as macOS's stdio.h gives freopen (`__DARWIN_ALIAS(freopen)`, `__asm("_" "freopen")`) and glibc's
   __REDIRECT its functions: the label is the symbol itself, the target's prefix included (Mach-O's `_`). */
#if defined(__APPLE__)
#define LABELLED(name) __asm("_" #name)
#else
#define LABELLED(name) __asm__("" #name)
#endif
int scaled(int x) LABELLED(scaled_by_label);
