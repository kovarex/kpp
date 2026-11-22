// cppinput: memory orders stay where C++ writes them (the library's Atomic takes them, each sequentially consistent), so a
// program's own atomic wrapper that requires one keeps its argument (Unreal's FPackedDataAtomic::load(std::memory_order),
// whose calls lost theirs); std::common_type_t is the library's CommonType
#include <atomic>
#include <type_traits>

struct PackedAtomic
{
  std::atomic<unsigned> Value{3};
  unsigned load(std::memory_order Order) const { return Value.load(Order); }
  void store(unsigned V, std::memory_order Order) { Value.store(V, Order); }
};

template<typename A, typename B>
std::common_type_t<A, B> Sum(A a, B b)
{
  return a + b;
}

int main()
{
  PackedAtomic p;
  p.store(p.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  unsigned seen = p.Value.fetch_add(1, std::memory_order_acq_rel);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  static_assert(std::is_same_v<std::common_type_t<int, long long>, long long>);
  return seen == 4 && p.load(std::memory_order::acquire) == 5 && Sum(1, 2.5) == 3.5 ? 0 : 1;
}
