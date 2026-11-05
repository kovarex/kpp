/* targets.ps1's C library: a C function, one whose answer is the platform's define, and a C++ one. */
#ifdef __cplusplus
extern "C" {
#endif
int scaled(int x);
const char* platform_name(void);
int cpp_sum(int count);
#ifdef __cplusplus
}
#endif
