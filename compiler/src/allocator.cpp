// The allocator's self-check (allocator.hpp): each way kpp allocates must hand out the block the
// allocator's own heap would give next.
#include "allocator.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void* kppAllocatorNextBlock(size_t size);

namespace allocator
{
  namespace
  {
    constexpr size_t checkSize = 40;

    void* viaMalloc(size_t size)
    {
      return malloc(size);
    }

    void* viaCalloc(size_t size)
    {
      return calloc(1, size);
    }

    void* viaRealloc(size_t size)
    {
      return realloc(nullptr, size);
    }

    void* viaNew(size_t size)
    {
      return ::operator new(size);
    }

    void* viaNewArray(size_t size)
    {
      return ::operator new[](size);
    }

    // The C runtime's own allocation (Windows' UCRT: _malloc_base, not malloc).
    void* viaStrdup(size_t size)
    {
      char text[checkSize];
      memset(text, 'x', size - 1);
      text[size - 1] = 0;
#ifdef _WIN32
      return _strdup(text);
#else
      return strdup(text);
#endif
    }

    void releaseFree(void* block)
    {
      free(block);
    }

    void releaseDelete(void* block)
    {
      ::operator delete(block);
    }

    void releaseDeleteArray(void* block)
    {
      ::operator delete[](block);
    }

    struct Way
    {
      const char* name;
      void* (*allocate)(size_t);
      void (*release)(void*);
    };
  }

  std::string check()
  {
    // Called through volatile pointers, so the compiler cannot take them for the allocation functions
    // it knows and decide a fresh block's comparison itself.
    static const Way ways[] = {{"malloc", viaMalloc, releaseFree},
                               {"calloc", viaCalloc, releaseFree},
                               {"realloc", viaRealloc, releaseFree},
                               {"operator new", viaNew, releaseDelete},
                               {"operator new[]", viaNewArray, releaseDeleteArray},
                               {"the C runtime's strdup", viaStrdup, releaseFree}};
    std::string problems;
    for (const Way& way : ways)
    {
      void* (*volatile allocate)(size_t) = way.allocate;
      void (*volatile release)(void*) = way.release;
      void* expected = kppAllocatorNextBlock(checkSize);
      void* got = allocate(checkSize);
      if (!expected || got != expected)
        problems += std::string(problems.empty() ? "" : ", ") + way.name;
      release(got);
    }
    return problems.empty() ? "" : problems + " did not allocate from " + name();
  }
}
