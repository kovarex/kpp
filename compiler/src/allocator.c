// kpp's allocator (docs/build.md, "The allocator"): rpmalloc 1.4.5 (third_party/rpmalloc), malloc,
// free and the rest for the whole process, operator new (which calls malloc) and the C runtime's own
// allocations included. Compiled into this one file so that kppAllocatorNextBlock, the self-check's
// half (allocator.cpp), can read the calling thread's heap.
// - Windows (the static C runtime, /MT, CMakeLists.txt): rpmalloc's own malloc.c defines malloc and
//   the C runtime's _malloc_base family, which the linker takes before libucrt's; a thread's heap is
//   made at its first allocation and released at its exit by a fiber-local storage callback. The
//   configuration LLVM's Windows release builds into LLVMSupport.lib, which before 2026-11-09 gave
//   kpp built with LLVM this allocator by accident (LLVMSupport.lib's copy is no longer linked: these
//   definitions come first).
// - POSIX (Linux, FreeBSD): malloc and the rest are defined below, in the executable, where they
//   interpose libc's for every library (glibc and FreeBSD's libc support it). Not rpmalloc's preload
//   mode: it replaces pthread_create through dlsym, and finalizes the allocator from a destructor at
//   exit while kpp's detached threads (the compile server's idle check) may still allocate. Here a
//   thread's heap is made at its first allocation, and released at its exit by a pthread key's
//   destructor.

// - macOS: not yet (2026-11-13). A definition of malloc in the executable is not taken by libSystem
//   or libc++ (two-level namespace), so a block from libc++'s operator new would reach rpmalloc's free;
//   rpmalloc there needs a malloc zone of its own. kpp uses macOS's malloc, and --version says so.

#if defined(__APPLE__)

#include <stddef.h>

const char* kppAllocatorName(void)
{
  return "macOS's malloc (rpmalloc is not used on macOS yet)";
}

void* kppAllocatorNextBlock(size_t size)
{
  (void)size;
  return 0;
}

#else

#if defined(_WIN32)
#  define ENABLE_OVERRIDE 1
#  define ENABLE_PRELOAD 1
#else
#  define ENABLE_OVERRIDE 0
#  define ENABLE_PRELOAD 0
#endif
#include "../third_party/rpmalloc/rpmalloc.c"

#if !defined(_WIN32)

#include <pthread.h>

// Releases a thread's heap at its exit (after its C++ thread_local destructors, which glibc runs first).
static pthread_key_t kppThreadKey;

static void kppThreadExit(void* value)
{
  (void)value;
  rpmalloc_thread_finalize(1);
}

// The first allocation of a thread: its heap, and of the process (on the main thread, before any
// other thread exists): rpmalloc itself.
static void kppThreadStart(void)
{
  if (!_rpmalloc_initialized)
  {
    rpmalloc_set_main_thread();
    rpmalloc_initialize();
    pthread_key_create(&kppThreadKey, kppThreadExit);
  }
  else
    rpmalloc_thread_initialize();
  pthread_setspecific(kppThreadKey, (void*)1);
}

static inline void kppThreadHeap(void)
{
  if (UNEXPECTED(!get_thread_heap_raw()))
    kppThreadStart();
}

#define KPP_ALLOCATOR __attribute__((visibility("default")))

KPP_ALLOCATOR void* malloc(size_t size)
{
  kppThreadHeap();
  return rpmalloc(size);
}

KPP_ALLOCATOR void* calloc(size_t count, size_t size)
{
  kppThreadHeap();
  return rpcalloc(count, size);
}

KPP_ALLOCATOR void* realloc(void* block, size_t size)
{
  kppThreadHeap();
  return rprealloc(block, size);
}

KPP_ALLOCATOR void* reallocarray(void* block, size_t count, size_t size)
{
  size_t total;
  if (__builtin_mul_overflow(count, size, &total))
  {
    errno = ENOMEM;
    return 0;
  }
  kppThreadHeap();
  return rprealloc(block, total);
}

// (rpfree needs no heap of the calling thread: a block's span names its own)
KPP_ALLOCATOR void free(void* block)
{
  rpfree(block);
}

KPP_ALLOCATOR void* aligned_alloc(size_t alignment, size_t size)
{
  kppThreadHeap();
  return rpaligned_alloc(alignment, size);
}

KPP_ALLOCATOR void* memalign(size_t alignment, size_t size)
{
  kppThreadHeap();
  return rpmemalign(alignment, size);
}

KPP_ALLOCATOR int posix_memalign(void** block, size_t alignment, size_t size)
{
  kppThreadHeap();
  return rpposix_memalign(block, alignment, size);
}

KPP_ALLOCATOR void* valloc(size_t size)
{
  kppThreadHeap();
  return rpaligned_alloc(_memory_page_size, size);
}

KPP_ALLOCATOR void* pvalloc(size_t size)
{
  kppThreadHeap();
  size_t pages = (size + _memory_page_size - 1) / _memory_page_size;
  return rpaligned_alloc(_memory_page_size, (pages ? pages : 1) * _memory_page_size);
}

KPP_ALLOCATOR size_t malloc_usable_size(void* block)
{
  return block ? rpmalloc_usable_size(block) : 0;
}

#endif

const char* kppAllocatorName(void)
{
  return "rpmalloc 1.4.5";
}

// The block the next allocation of `size` bytes (a small one: at most SMALL_SIZE_LIMIT, 1024) on this
// thread takes from rpmalloc: the head of its size class's free list in this thread's heap, made
// non-empty first; 0 if it stays empty. allocator.cpp's check compares what malloc, operator new and
// the C runtime's strdup return with it: a block of another allocator is never this one.
void* kppAllocatorNextBlock(size_t size)
{
#if !defined(_WIN32)
  kppThreadHeap();
#endif
  void* first = rpmalloc(size == 0 ? 1 : size); // (this thread's heap made, the size class's span taken)
  heap_t* heap = get_thread_heap_raw();
  if (!heap || !first || size > SMALL_SIZE_LIMIT)
    return 0;
  heap_size_class_t* sizeClass = heap->size_class + ((size + (SMALL_GRANULARITY - 1)) >> SMALL_GRANULARITY_SHIFT);
  for (int i = 0; i < 4 && !sizeClass->free_list; ++i)
    rpfree(rpmalloc(size)); // (a freed block goes to its span's list, not the heap's)
  rpfree(first);
  return sizeClass->free_list;
}

#endif
