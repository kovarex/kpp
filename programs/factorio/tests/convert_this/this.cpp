// `this` as a reference (design.md 23d): each C++ spelling of the object, converted, built with --this-reference and
// run, prints what its C++ build prints (tests\convert_this.expected).
#include <cmath>
#include <cstdio>
#include <functional>

struct Counter;
void report(const Counter* counter);

struct Counter
{
  int value = 0;
  static int created;
  int Counter::* member = &Counter::value;

  Counter() { ++created; }
  Counter(const Counter& other) : value(other.value) { ++created; }

  int get() const { return value; }
  int getThroughThis() const { return this->value + this->get(); }
  int getThroughMemberPointer() const { return this->*member; }
  int getThroughParentheses() const { return (*this).value; }
  Counter& add(int amount) { this->value += amount; return *this; }
  Counter* self() { return this; }
  const Counter* constSelf() const { return this; }
  bool isSame(const Counter& other) const { return this == &other; }
  bool isNotSame(const Counter* other) const { return other != this; }
  Counter copy() const { return *this; }
  void passed() const { report(this); }
  int bare() const { return value + get(); }
  static int count() { return created; }
  int staticThroughBare() const { return created + count(); }
  int inLambda()
  {
    auto lambda = [this](int more) { return this->value + more; };
    auto byReference = [&]() { return this->value; };
    return lambda(1) + byReference();
  }
  int sizeOfObject() const { return sizeof(*this) == sizeof(Counter); }
  void selfDelete() { delete this; }
  int comment() const { /* this->value and *this are text */ return value; } // this->value
  const char* text() const { return "this->value"; }
};

int Counter::created = 0;

void report(const Counter* counter)
{
  printf("report %d\n", counter->value);
}

struct WithOperators
{
  int data = 5;
  int operator*() const { return 7; }
  int star() const { return (*this).data + **this; }
  explicit operator bool() const { return data == 5; }
  int asCondition() const { if (*this) return *this ? 1 : 2; return 3; }
  int (WithOperators::*action)() const = &WithOperators::star;
  int throughAction() const { return (*this.*action)(); }
};

struct Outer
{
  static const int shared = 4;
  static int makeStatic() { return helper(1); }
  static int helper(int value) { return value + 1; }
  int helper(int value, int more) const { return value + more; }
  struct Inner
  {
    static const int width = shared;
    int field : shared;
    int read() const { return shared + width; }
  };
};

template<class T>
struct Base
{
  T base = T(3);
  T read() const { return this->base; }
};

template<class T>
struct Derived : Base<T>
{
  T twice() const { return this->base + this->read(); }
};

int main()
{
  Counter a;
  a.add(2).add(3);
  printf("%d %d %d %d\n", a.get(), a.getThroughThis(), a.getThroughMemberPointer(), a.getThroughParentheses());
  printf("%d %d %d\n", a.self() == &a, a.constSelf() == &a, a.isSame(a));
  Counter b = a.copy();
  printf("%d %d\n", b.get(), a.isNotSame(&b));
  a.passed();
  printf("%d %d\n", a.bare(), a.staticThroughBare());
  printf("%d %d\n", a.inLambda(), a.sizeOfObject());
  printf("%d %s\n", a.comment(), a.text());
  Counter* heap = new Counter();
  heap->selfDelete();
  printf("%d\n", WithOperators().star());
  printf("%d %d\n", WithOperators().asCondition(), WithOperators().throughAction());
  printf("%d %d\n", Outer::makeStatic(), Outer::Inner().read());
  printf("%d\n", Derived<int>().twice());
  // (bare, as <cmath> has it: a double, not C's abs(int))
  printf("%g %g\n", abs(-1.5), floor(-1.5));
  return 0;
}
