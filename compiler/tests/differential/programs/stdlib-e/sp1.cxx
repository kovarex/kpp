#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
namespace filesystem = std::filesystem;
using ErrorCode = std::error_code;
using String = std::string;
template <typename T> using Vector = std::vector<T>;
void show(const char* label, const std::string& s) { printf("%s: [%s]\n", label, s.c_str()); }
void flag(const char* label, bool b) { printf("%s: %s\n", label, b ? "true" : "false"); }
void number(const char* label, long long n) { printf("%s: %lld\n", label, n); }
template <typename V> void sortStrings(V& v) { std::sort(v.begin(), v.end()); }
void appendText(std::string& to, const std::string& s) { to += s; }
#include <memory>
#include <utility>
using std::move;
template <typename T> using UniquePtr = std::unique_ptr<T>;
template <typename T> using SharedPtr = std::shared_ptr<T>;
template <typename T> using WeakPtr = std::weak_ptr<T>;
template <typename T> using EnableSharedFromThis = std::enable_shared_from_this<T>;
using BadWeakPtr = std::bad_weak_ptr;
#define makeUnique std::make_unique
#define makeShared std::make_shared
// UniquePtr, SharedPtr and WeakPtr against std::unique_ptr, shared_ptr and weak_ptr: use counts,
// conversions to a base (deleting the derived object), EnableSharedFromThis, weak lock after expiry,
// destruction order, and assignments whose source lives inside the object being given up
// (`head = move(head->next)`, walking a list).
int alive = 0;
char order[256];
int orderLength = 0;
class Base
{
public:
  int v;
  Base(int x) : v(x) { ++alive; }
  virtual ~Base()
  {
    --alive;
    order[orderLength++] = (char)('0' + v % 10);
  }
};
class Derived : public Base
{
public:
  int extra;
  Derived(int x) : Base(x), extra(x * 2) {}
  ~Derived() override { order[orderLength++] = 'd'; }
};
// No virtual destructor: a SharedPtr<Plain> made from a SharedPtr<PlainDerived> still deletes a PlainDerived.
struct Plain
{
  int v = 1;
  ~Plain() { order[orderLength++] = 'p'; }
};
struct PlainDerived : Plain
{
  ~PlainDerived() { order[orderLength++] = 'D'; }
};
struct UNode
{
  int v;
  UniquePtr<UNode> next;
  UNode(int x) : v(x) { ++alive; }
  ~UNode() { --alive; }
};
struct SNode
{
  int v;
  SharedPtr<SNode> next;
  SNode(int x) : v(x) { ++alive; }
  ~SNode() { --alive; }
};
struct Self : EnableSharedFromThis<Self>
{
  int v = 5;
};
void showOrder(const char* label)
{
  order[orderLength] = 0;
  printf("%s: alive %d order [%s]\n", label, alive, order);
  orderLength = 0;
}

int main()
{
  {
    UniquePtr<Base> b = makeUnique<Derived>(3);
    UniquePtr<Derived> d = makeUnique<Derived>(4);
    b = move(d);
    showOrder("UniquePtr = derived");
    printf("moved-from null %d, value %d\n", (int)(d == nullptr), b->v);
    Base* raw = b.release();
    printf("release keeps it alive %d\n", alive);
    b.reset(raw);
    b.reset();
    showOrder("UniquePtr release, reset");
  }
  showOrder("UniquePtr scope end");
  {
    UniquePtr<UNode> head = makeUnique<UNode>(1);
    head->next = makeUnique<UNode>(2);
    head->next->next = makeUnique<UNode>(3);
    printf("UniquePtr list alive %d\n", alive);
    head = move(head->next);
    printf("head = move(head->next): alive %d, head %d, next %d\n", alive, head ? head->v : -1, head && head->next ? head->next->v : -1);
    head = move(head->next);
    printf("again: alive %d, head %d, next null %d\n", alive, head ? head->v : -1, (int)(head && head->next == nullptr));
  }
  printf("UniquePtr list end alive %d\n", alive);

  {
    SharedPtr<Base> a = makeShared<Derived>(6);
    printf("use_count %d\n", (int)a.use_count());
    SharedPtr<Base> b = a;
    WeakPtr<Base> w = a;
    printf("copy: use_count %d %d, expired %d, weak use_count %d\n", (int)a.use_count(), (int)b.use_count(), (int)w.expired(), (int)w.use_count());
    SharedPtr<Base> c = move(b);
    printf("move: use_count %d, moved-from %d null %d\n", (int)a.use_count(), (int)b.use_count(), (int)(b == nullptr));
    {
      SharedPtr<Base> l = w.lock();
      printf("lock: use_count %d\n", (int)a.use_count());
    }
    a.reset();
    printf("reset one: use_count %d expired %d\n", (int)c.use_count(), (int)w.expired());
    c = nullptr;
    showOrder("SharedPtr last owner gone");
    printf("expired %d, lock null %d, weak use_count %d\n", (int)w.expired(), (int)(w.lock() == nullptr), (int)w.use_count());
  }
  {
    SharedPtr<Plain> p = SharedPtr<PlainDerived>(new PlainDerived());
    p.reset();
    showOrder("SharedPtr<Plain> from SharedPtr<PlainDerived>");
    UniquePtr<Derived> u = makeUnique<Derived>(7);
    SharedPtr<Base> fromUnique = move(u);
    printf("from UniquePtr: use_count %d, unique null %d\n", (int)fromUnique.use_count(), (int)(u == nullptr));
  }
  showOrder("from UniquePtr scope end");
  {
    SharedPtr<Base> x = makeShared<Base>(1), y = makeShared<Base>(2), z = makeShared<Base>(3);
    x = y;
    showOrder("x = y destroys x's");
    y = z;
    z.reset();
    showOrder("y = z, z.reset");
  }
  showOrder("three in a scope (destroyed in reverse order)");
  {
    SharedPtr<SNode> head = makeShared<SNode>(1);
    head->next = makeShared<SNode>(2);
    head->next->next = makeShared<SNode>(3);
    head = head->next;
    printf("head = head->next: alive %d, head %d, next %d, use_count %d\n", alive, head ? head->v : -1, head && head->next ? head->next->v : -1, (int)head.use_count());
    head = move(head->next);
    printf("head = move(head->next): alive %d, head %d, use_count %d\n", alive, head ? head->v : -1, (int)head.use_count());
    WeakPtr<SNode> w = head;
    head = nullptr;
    printf("expired %d alive %d\n", (int)w.expired(), alive);
  }
  {
    SharedPtr<Self> s = makeShared<Self>();
    SharedPtr<Self> t = s->shared_from_this();
    printf("shared_from_this: same %d, use_count %d\n", (int)(s == t), (int)s.use_count());
    WeakPtr<Self> w = s->weak_from_this();
    printf("weak_from_this: expired %d\n", (int)w.expired());
    Self onStack;
    try
    {
      SharedPtr<Self> bad = onStack.shared_from_this();
      printf("no throw\n");
    }
    catch (const BadWeakPtr&)
    {
      printf("shared_from_this of an unowned object threw\n");
    }
    printf("weak_from_this of an unowned object expired %d\n", (int)onStack.weak_from_this().expired());
    s.reset();
    t.reset();
    printf("after the owners: weak expired %d\n", (int)w.expired());
  }
  return 0;
}
