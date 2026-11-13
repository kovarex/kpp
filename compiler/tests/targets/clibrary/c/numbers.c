#include "numbers.h"

int scaled(int x) { return x * SCALE; }

const char* platform_name(void) { return PLATFORM_ID == 1 ? "windows" : PLATFORM_ID == 2 ? "linux" : PLATFORM_ID == 3 ? "freebsd" : "mac"; }
