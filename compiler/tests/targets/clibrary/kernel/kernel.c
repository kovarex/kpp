/* The kernel library (Linux only): a header of a directory of the target's sysroot, given to the library as
   "=/usr/include/linux" (linux/version.h as <version.h>), which only the sysroot's include path reaches. */
#include <version.h>
#ifndef LINUX_VERSION_CODE
#error "not the sysroot's linux/version.h"
#endif

int kernel_headers_major(void) { return LINUX_VERSION_CODE >> 16; }
