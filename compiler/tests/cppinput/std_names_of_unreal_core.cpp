// cppinput: the standard names Unreal's Core uses that the library gained on 2026-11-22 convert to it (the type traits,
// std::conjunction_v, add_volatile_t, addressof, launder, the memory orders, atomic_int and atomic_uint), where they were
// written without std:: and were undeclared
#include <atomic>
#include <memory>
#include <type_traits>

struct Empty
{
};

template<typename T>
constexpr bool Check = std::is_lvalue_reference_v<T&> && std::is_rvalue_reference_v<T&&> && std::is_volatile_v<volatile T> && std::is_object_v<T> &&
                       std::is_scalar_v<T*> && std::is_empty_v<Empty> && std::is_default_constructible_v<T> && std::conjunction_v<std::true_type> &&
                       !std::disjunction_v<std::false_type>;

struct Counter
{
  std::atomic_uint Active;
  std::atomic_int Count;
  std::add_volatile_t<int>* Flag = nullptr;

  int Next(int& v)
  {
    constexpr std::memory_order MemoryOrder = std::memory_order_relaxed;
    Active.store(3);
    unsigned x = Active.load(MemoryOrder);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return *std::launder(std::addressof(v)) + int(x);
  }
};

int main()
{
  Counter c;
  int v = 4;
  return Check<int> && c.Next(v) == 7 ? 0 : 1;
}
