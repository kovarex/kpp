#pragma once
// Included by ast.hpp, where it was.

class Arena
{
public:
  // Sizes the first chunk so that a typical file needs exactly one allocation. Many small
  // allocations of a fixed large chunk size serialise on the OS page allocator.
  void reserve(size_t bytes)
  {
    if (capacity == 0 && bytes > 0)
    {
      chunks.emplace_back(new char[bytes]);
      base = chunks.back().get();
      capacity = bytes;
      pos = 0;
    }
  }
  void* allocate(size_t size, size_t align)
  {
    size_t aligned = (pos + align - 1) & ~(align - 1);
    if (aligned + size > capacity)
    {
      size_t chunk = std::max<size_t>(size + align, std::max<size_t>(capacity, 64 << 10));
      chunks.emplace_back(new char[chunk]); // Not value-initialised: no 1 MB memset per chunk.
      base = chunks.back().get();
      capacity = chunk;
      pos = 0;
      aligned = (reinterpret_cast<uintptr_t>(base) + align - 1) & ~(align - 1);
      aligned -= reinterpret_cast<uintptr_t>(base);
    }
    void* result = base + aligned;
    pos = aligned + size;
    bytes += size;
    return result;
  }
  template<typename T, typename... Args>
  T* make(Args&&... args)
  {
    return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }
  template<typename T>
  T* array(const T* src, size_t count)
  {
    if (count == 0)
      return nullptr;
    T* result = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    for (size_t i = 0; i < count; ++i)
      new (result + i) T(src[i]);
    return result;
  }
  size_t bytesAllocated() const { return bytes; }

private:
  std::vector<std::unique_ptr<char[]>> chunks;
  char* base = nullptr;
  size_t capacity = 0;
  size_t pos = 0;
  size_t bytes = 0;
};
