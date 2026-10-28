/* For interop/c_import_anonymous_enum.kpp: the C shapes of curl's CURLcode and CURLMsg. */
typedef enum { SPEC_OK, SPEC_FAILED = 5 } SpecCode;
struct SpecMsg { int kind; union { void* whatever; SpecCode result; } data; };
enum { SPEC_LIMIT = 64 } specLimitHolder;
