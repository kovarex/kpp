/* For interop/c_header_noreturn_callback_parameter.kpp: a function whose parameter is a callback that
   does not return. */
void set_fatal_handler(void (*handler)(int) __attribute__((noreturn)));
