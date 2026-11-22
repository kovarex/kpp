/* For interop/c_macro_constant_with_sizeof.kpp: object-like macros whose body adds a sizeof of a typedef to another
   constant (winuser.h's `#define DWLP_DLGPROC DWLP_MSGRESULT + sizeof(LRESULT)`, `#define DWLP_USER DWLP_DLGPROC +
   sizeof(DLGPROC)`). */
typedef int my_result;
typedef double my_proc;
#define AA_MSGRESULT 0
#define AA_DLGPROC AA_MSGRESULT + sizeof(my_result)
#define AA_USER AA_DLGPROC + sizeof(my_proc)
