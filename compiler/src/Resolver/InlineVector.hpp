#pragma once
// Part of the resolver (GlobalScopeImpl.hpp includes it inside namespace resolver_detail).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

// A vector of trivially copyable elements whose first N are held in the object itself: a Subst of
// N pairs or fewer (most templates have 4 parameters or fewer) is made and copied without the heap,
// whose lock was a quarter of the resolve phase's waits at -j 32 (A-resolve-profile-plan.md).
template<typename T, uint32_t N>
class InlineVector
{
  static_assert(std::is_trivially_copyable_v<T>);

  void grow(uint32_t wanted)
  {
    uint32_t newCapacity = std::max(wanted, capacity * 2);
    T* bigger = static_cast<T*>(::operator new(sizeof(T) * newCapacity));
    memcpy(static_cast<void*>(bigger), items, sizeof(T) * count);
    if (items != inlineItems)
      ::operator delete(items);
    items = bigger;
    capacity = newCapacity;
  }
  void copyFrom(const InlineVector& other)
  {
    if (other.count > capacity)
      grow(other.count);
    memcpy(static_cast<void*>(items), other.items, sizeof(T) * other.count);
    count = other.count;
  }
public:
  InlineVector() = default;
  InlineVector(const InlineVector& other) { copyFrom(other); }
  InlineVector(InlineVector&& other) noexcept
  {
    if (other.items != other.inlineItems)
    {
      items = other.items; // (the heap block changes hands)
      capacity = other.capacity;
      count = other.count;
      other.items = other.inlineItems;
      other.capacity = N;
      other.count = 0;
    }
    else
      copyFrom(other);
  }
  InlineVector& operator=(const InlineVector& other)
  {
    if (this != &other)
    {
      count = 0;
      copyFrom(other);
    }
    return *this;
  }
  InlineVector& operator=(InlineVector&& other) noexcept
  {
    if (this == &other)
      return *this;
    if (other.items != other.inlineItems)
    {
      if (items != inlineItems)
        ::operator delete(items);
      items = other.items;
      capacity = other.capacity;
      count = other.count;
      other.items = other.inlineItems;
      other.capacity = N;
      other.count = 0;
    }
    else
    {
      count = 0;
      copyFrom(other);
    }
    return *this;
  }
  ~InlineVector()
  {
    if (items != inlineItems)
      ::operator delete(items);
  }
  void push_back(const T& item)
  {
    if (count == capacity)
    {
      T copy = item; // (item may be one of ours)
      grow(count + 1);
      items[count++] = copy;
      return;
    }
    items[count++] = item;
  }
  void reserve(size_t wanted)
  {
    if (wanted > capacity)
      grow(uint32_t(wanted));
  }
  void clear() { count = 0; }
  bool empty() const { return count == 0; }
  size_t size() const { return count; }
  T* begin() { return items; }
  T* end() { return items + count; }
  const T* begin() const { return items; }
  const T* end() const { return items + count; }
  T& back() { return items[count - 1]; }
  const T& back() const { return items[count - 1]; }
  T& operator[](size_t i) { return items[i]; }
  const T& operator[](size_t i) const { return items[i]; }
private:
  T* items = inlineItems;
  uint32_t count = 0, capacity = N;
  T inlineItems[N];
};
