// A class's operator!= converted for `a != b` being only ever `!(a == b)` (kpp's design.md 4): dropped where it is
// the negation of the class's == (`!(*this == other)`, `!operator==(other)`, a defaulted one, a free one negating
// the free ==, terms the negation of =='s terms, `|` between parenthesized terms, each data member's term beside a
// defaulted ==, one defined out of its class, a template with a requires-clause, which goes with it), made the ==
// where the class has none (an iterator's `position != other.position`, a sentinel's `!ended()` and its
// `index < count`, negated). Converted and run, it prints what its C++ build prints (tests\convert_notequal.expected),
// and convert.cmd checks that no `operator!=` is left in its text. The ones the converter keeps are in kept.cpp.
#include <cstdio>
#include <cstddef>

struct Negated
{
  int x;
  bool operator==(const Negated& other) const { return x == other.x; }
  bool operator!=(const Negated& other) const { return !(*this == other); }
};

struct Fields
{
  int x, y;
  bool operator==(const Fields& other) const { return x == other.x && y == other.y; }
  bool operator!=(const Fields& other) const { return other.y != y || x != other.x; }
};

struct Masked
{
  float x, y;
  bool operator==(const Masked& other) const { return (x == other.x) & (y == other.y); }
  bool operator!=(const Masked& other) const { return (x != other.x) | (y != other.y); }
};

struct Defaulted
{
  int x;
  bool operator==(const Defaulted& other) const = default;
  bool operator!=(const Defaulted& other) const = default;
};

struct Free
{
  int x;
};
bool operator==(Free a, Free b) { return a.x == b.x; }
bool operator!=(Free a, Free b) { return !(a == b); }

struct OutOfLine
{
  int x;
  bool operator==(const OutOfLine& other) const;
  bool operator!=(const OutOfLine& other) const;
};
bool OutOfLine::operator==(const OutOfLine& other) const { return x == other.x; }
bool OutOfLine::operator!=(const OutOfLine& other) const { return !(*this == other); }

template<class T> struct Box
{
  T v;
};
template<class T> requires (sizeof(T) > 0) bool operator==(const Box<T>& a, const Box<T>& b) { return a.v == b.v; }
template<class T> requires (sizeof(T) > 0) bool operator!=(const Box<T>& a, const Box<T>& b) { return !(a == b); }
struct AfterBox
{
  int z;
};

struct Called
{
  int x;
  bool operator==(const Called& other) const { return x == other.x; }
  bool operator!=(const Called& other) const { return !operator==(other); }
};

struct Members
{
  int x, y;
  bool operator==(const Members& other) const = default;
  bool operator!=(const Members& other) const { return x != other.x || other.y != y; }
};

struct Counted
{
  int index, count;
  bool operator!=(std::nullptr_t) const { return index < count; }
};

struct Iterator
{
  int position;
  bool operator!=(const Iterator& other) const { return position != other.position; }
};

struct Sentinel
{
  int left;
  bool ended() const { return left == 0; }
  bool operator!=(std::nullptr_t) const { return !ended(); }
};

int main()
{
  Negated n1{1}, n2{2};
  Fields f1{1, 2}, f2{1, 3};
  Masked m1{1.0f, 2.0f}, m2{1.0f, 2.0f};
  Defaulted d1{4}, d2{4};
  Free g1{5}, g2{6};
  OutOfLine o1{7}, o2{7};
  Iterator i1{3}, i2{4};
  Sentinel s1{2}, s2{0};
  Box<int> b1{1}, b2{2};
  AfterBox after{9};
  Called c1{1}, c2{1};
  Members e1{1, 2}, e2{1, 3};
  Counted k1{1, 3}, k2{3, 3};
  printf("negated %d %d\n", n1 != n2, n1 != n1);
  printf("fields %d %d\n", f1 != f2, f1 != f1);
  printf("masked %d %d\n", m1 != m2, m1 == m2);
  printf("defaulted %d\n", d1 != d2);
  printf("free %d %d\n", g1 != g2, g2 != g1);
  printf("outofline %d\n", o1 != o2);
  printf("iterator %d %d\n", i1 != i2, i1 != i1);
  printf("sentinel %d %d\n", s1 != nullptr, s2 != nullptr);
  printf("box %d %d %d\n", b1 != b2, b1 != b1, after.z);
  printf("called %d members %d %d counted %d %d\n", c1 != c2, e1 != e2, e1 != e1, k1 != nullptr, k2 != nullptr);
  return 0;
}
