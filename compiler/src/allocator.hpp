#pragma once

// kpp's allocator (allocator.c, docs/build.md "The allocator"): rpmalloc, malloc for the whole
// process on every host, and its self-check, which kpp --version reports.

#include <string>

extern "C" const char* kppAllocatorName(void);

namespace allocator
{
  // The allocator kpp is built with ("rpmalloc 1.4.5").
  inline const char* name()
  {
    return kppAllocatorName();
  }

  // Empty when every way kpp allocates (malloc, calloc, realloc, operator new and new[], the C
  // runtime's strdup) got its block from the allocator, on this thread; else which did not. A link
  // that took the C runtime's malloc again (a link order changed) fails it.
  std::string check();
}
