#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <compare>
template <typename T> using Vector = std::vector<T>;
using String = std::string;
using WString = std::wstring;
using OutOfRange = std::out_of_range;
using std::move;
#define uninitializedCopy std::uninitialized_copy
#define uninitializedMove std::uninitialized_move
#define uninitializedDefaultConstruct std::uninitialized_default_construct
#define uninitializedValueConstruct std::uninitialized_value_construct
#define destroy std::destroy
#define toWString std::to_wstring
// The uninitialized family and WString against std.
struct Tracked
{
  static int live, copies, moves, ticks, throwAt;
  int v;
  static void tick()
  {
    ticks++;
    if (ticks == throwAt)
      throw 42;
  }
  Tracked() : v(7) { tick(); live++; }
  explicit Tracked(int v) : v(v) { live++; }
  Tracked(const Tracked& o) : v(o.v) { tick(); copies++; live++; }
  Tracked(Tracked&& o) : v(o.v) { tick(); o.v = -1; moves++; live++; }
  ~Tracked() { live--; }
};
int Tracked::live = 0;
int Tracked::copies = 0;
int Tracked::moves = 0;
int Tracked::ticks = 0;
int Tracked::throwAt = 0;
struct Plain
{
  int a;
  int b;
};
struct Init
{
  int a;
  int b = 3;
};
long long storage[64];
void arm(int at)
{
  Tracked::ticks = 0;
  Tracked::throwAt = at;
  Tracked::copies = 0;
  Tracked::moves = 0;
}
void report(const char* label)
{
  printf("%s: live %d copies %d moves %d\n", label, Tracked::live, Tracked::copies, Tracked::moves);
}
void family()
{
  Tracked* p = (Tracked*)storage;
  {
    Vector<Tracked> source;
    source.reserve(8);
    for (int i = 0; i < 5; i++)
      source.emplace_back(i + 1);
    Tracked* s = source.data();
    arm(0);
    uninitializedCopy(s, s + 5, p);
    report("copied");
    for (int i = 0; i < 5; i++)
      printf(" %d", p[i].v);
    printf("\n");
    destroy(p, p + 5);
    report("destroyed");
    arm(3);
    try { uninitializedCopy(s, s + 5, p); printf("no throw\n"); }
    catch (int e) { printf("caught %d\n", e); }
    report("copy threw at 3");
    arm(1);
    try { uninitializedCopy(s, s + 5, p); printf("no throw\n"); }
    catch (int e) { printf("caught %d\n", e); }
    report("copy threw at 1");
    arm(5);
    try { uninitializedCopy(s, s + 5, p); printf("no throw\n"); }
    catch (int e) { printf("caught %d\n", e); }
    report("copy threw at 5");
    arm(0);
    uninitializedCopy(s, s, p);
    report("copied nothing");
    arm(0);
    uninitializedMove(s, s + 3, p);
    report("moved");
    for (int i = 0; i < 5; i++)
      printf(" %d", s[i].v);
    printf(" |");
    for (int i = 0; i < 3; i++)
      printf(" %d", p[i].v);
    printf("\n");
    destroy(p, p + 3);
    report("destroyed");
    arm(2);
    try { uninitializedMove(s + 3, s + 5, p); printf("no throw\n"); }
    catch (int e) { printf("caught %d\n", e); }
    report("move threw at 2");
  }
  report("source gone");
  arm(0);
  uninitializedDefaultConstruct(p, p + 4);
  report("default");
  printf(" %d %d\n", p[0].v, p[3].v);
  destroy(p, p + 4);
  arm(3);
  try { uninitializedDefaultConstruct(p, p + 4); printf("no throw\n"); }
  catch (int e) { printf("caught %d\n", e); }
  report("default threw at 3");
  arm(0);
  uninitializedValueConstruct(p, p + 4);
  report("value");
  destroy(p, p + 4);
  arm(2);
  try { uninitializedValueConstruct(p, p + 4); printf("no throw\n"); }
  catch (int e) { printf("caught %d\n", e); }
  report("value threw at 2");
  int* ints = (int*)storage;
  for (int i = 0; i < 8; i++)
    ints[i] = 5;
  uninitializedValueConstruct(ints, ints + 3);
  printf("ints %d %d %d %d\n", ints[0], ints[1], ints[2], ints[3]);
  Plain* plain = (Plain*)storage;
  for (int i = 0; i < 8; i++)
    ints[i] = 5;
  uninitializedValueConstruct(plain, plain + 2);
  printf("plain %d %d %d %d %d\n", plain[0].a, plain[0].b, plain[1].a, plain[1].b, ints[4]);
  Init* init = (Init*)storage;
  for (int i = 0; i < 8; i++)
    ints[i] = 5;
  uninitializedValueConstruct(init, init + 2);
  printf("init %d %d %d %d\n", init[0].a, init[0].b, init[1].a, init[1].b);
  int from[3] = {1, 2, 3};
  uninitializedCopy(from, from + 3, ints);
  printf("int copy %d %d %d\n", ints[0], ints[1], ints[2]);
  uninitializedMove(from, from + 3, ints + 3);
  printf("int move %d %d\n", ints[3], ints[5]);
  // strings: an owner in uninitialized memory
  String* texts = (String*)storage;
  Vector<String> words = {"one", "a much longer word than the small buffer holds", ""};
  uninitializedCopy(words.data(), words.data() + 3, texts);
  printf("%s|%s|%s\n", texts[0].c_str(), texts[1].c_str(), texts[2].c_str());
  destroy(texts, texts + 3);
  uninitializedMove(words.data(), words.data() + 2, texts);
  printf("%s|%s\n", texts[0].c_str(), texts[1].c_str());
  destroy(texts, texts + 2);
}
void show(const char* label, const WString& s)
{
  printf("%s [%d]", label, (int)s.size());
  for (wchar_t c : s)
    printf(" %x", (unsigned)c);
  printf("\n");
}
void text(const char* label, const WString& s)
{
  printf("%s: ", label);
  for (wchar_t c : s)
    printf("%c", (char)c);
  printf("\n");
}
int sign(int x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }
void wide()
{
  WString empty;
  show("empty", empty);
  printf("c_str %d data %d\n", (int)empty.c_str()[0], (int)((const WString&)empty).data()[0]);
  WString abc = L"abc";
  WString counted(L"abcdef", 4);
  WString none(L"abcdef", 0);
  const wchar_t* letters = L"xyz";
  WString range(letters, letters + 2);
  WString filled(3, L'q');
  WString nofill(0, L'q');
  text("abc", abc);
  text("counted", counted);
  text("none", none);
  text("range", range);
  text("filled", filled);
  text("nofill", nofill);
  WString nul(L"a\0b", 3);
  show("nul", nul);
  printf("nul %d %d %d\n", (int)(nul == WString(L"a")), (int)(nul == L"a"), (int)(nul > WString(L"a")));
  WString big = L"ž\U0001F600!";
  show("big", big);
  // growth and edits
  WString s;
  for (int i = 0; i < 200; i++)
    s.push_back((wchar_t)(L'a' + i % 26));
  printf("%d %c %c\n", (int)s.size(), (char)s.front(), (char)s.back());
  s.pop_back();
  s.resize(5);
  text("resized", s);
  s.resize(8, L'!');
  text("grown", s);
  s.resize(10);
  show("grown with zeros", s);
  s.resize(3);
  s.append(L"def");
  s.append(L"ghijkl", 2);
  s.append(WString(L"XY"));
  s.append(3, L'-');
  s.append(0, L'+');
  text("appended", s);
  s.erase(0, 1);
  s.erase(2, 2);
  s.erase(s.size() - 1, 100);
  s.erase(s.size(), 0);
  text("erased", s);
  s.insert(0, L"<<");
  s.insert(s.size(), L">>");
  s.insert(3, WString(L"mid"));
  s.insert(1, 2, L'*');
  s.insert(2, L"pqrs", 2);
  s.insert(2, L"", 0);
  text("inserted", s);
  s.clear();
  show("cleared", s);
  // aliasing: arguments inside the string itself
  WString a = L"abcdefgh";
  a.append(a.c_str() + 2, 3);
  text("append own", a);
  a = L"abcdefgh";
  a.append(a);
  text("append self", a);
  a = L"abcdefgh";
  a.append(a.c_str() + 5);
  text("append own tail", a);
  a = L"abcdefgh";
  a.insert(2, a.c_str() + 1, 3);
  text("insert own", a);
  a = L"abcdefgh";
  a.insert(4, a);
  text("insert self", a);
  a = L"abcdefgh";
  a.insert(0, a.c_str() + 6);
  text("insert own tail", a);
  a = L"abcdefgh";
  for (int i = 0; i < 40; i++)
    a.append(a.c_str(), 2);
  printf("%d\n", (int)a.size());
  a = L"abcdefgh";
  a = a.c_str() + 3;
  text("assign own tail", a);
  WString& alias = a;
  a = alias;
  text("self assign", a);
  a = move(alias);
  printf("self move size %d\n", (int)(a.size() <= 5));
  // searching
  WString h = L"abcabcabc";
  printf("find %d %d %d %d %d\n", (int)h.find(L"bc"), (int)h.find(L"bc", 2), (int)h.find(L"bc", 8), (int)h.find(L"zz"), (int)h.find(L"bc", 100));
  printf("find empty %d %d %d %d\n", (int)h.find(L""), (int)h.find(L"", 4), (int)h.find(L"", 9), (int)h.find(L"", 10));
  printf("find longer %d\n", (int)h.find(L"abcabcabcabc"));
  printf("find counted %d %d\n", (int)h.find(L"bcz", 3, 2), (int)h.find(L"cab", 1, 1));
  printf("find string %d %d\n", (int)h.find(WString(L"ca")), (int)h.find(WString(L"ca"), 3));
  printf("find char %d %d %d %d\n", (int)h.find(L'c'), (int)h.find(L'c', 3), (int)h.find(L'z'), (int)h.find(L'a', 9));
  printf("rfind %d %d %d %d %d\n", (int)h.rfind(L'a'), (int)h.rfind(L'a', 5), (int)h.rfind(L'a', 0), (int)h.rfind(L'c', 1), (int)h.rfind(L'z'));
  printf("rfind big %d\n", (int)h.rfind(L'c', 1000));
  printf("first of %d %d %d %d %d\n", (int)h.find_first_of(L"cb"), (int)h.find_first_of(L"c", 3), (int)h.find_first_of(L""), (int)h.find_first_of(L"z"), (int)h.find_first_of(L"a", 9));
  printf("last of %d %d %d %d\n", (int)h.find_last_of(L"ab"), (int)h.find_last_of(L'b'), (int)h.find_last_of(L""), (int)h.find_last_of(L"z"));
  printf("on empty %d %d %d %d\n", (int)empty.find(L"a"), (int)empty.find(L""), (int)empty.rfind(L'a'), (int)empty.find_last_of(L"a"));
  text("substr", h.substr(3));
  text("substr 2", h.substr(3, 2));
  text("substr long", h.substr(7, 100));
  text("substr end", h.substr(9));
  // comparison
  WString x = L"abc", y = L"abd", z = L"ab";
  printf("compare %d %d %d %d\n", sign(x.compare(y)), sign(y.compare(x)), sign(x.compare(z)), sign(x.compare(x)));
  printf("relations %d %d %d %d %d\n", (int)(x < y), (int)(x > z), (int)(x <= x), (int)(y >= x), (int)(x == L"abc"));
  printf("<=> %d %d %d\n", (int)((x <=> y) < 0), (int)((x <=> z) > 0), (int)((x <=> x) == 0));
  WString high(1, (wchar_t)0xFFF0), low = L"a";
  printf("high %d %d\n", sign(high.compare(low)), (int)(high > low));
  WString minus(1, (wchar_t)-1);
  printf("minus %d %d\n", sign(minus.compare(low)), (int)(minus > low));
  // concatenation
  text("+", x + y);
  text("+ text", x + L"!");
  text("text +", L"!" + x);
  text("+ char", x + L'!');
  text("char +", L'!' + x);
  // numbers
  text("0", toWString(0));
  text("int min", toWString((int)-2147483647 - 1));
  text("unsigned", toWString(4294967295u));
  text("long", toWString((long)-5));
  text("unsigned long", toWString((unsigned long)4294967295u));
  text("long long", toWString(-9223372036854775807ll - 1));
  text("largest unsigned long", toWString(~(unsigned long)0));
  // errors
  try { h.at(9); printf("no throw\n"); } catch (const OutOfRange&) { printf("at\n"); }
  printf("at %c\n", (char)h.at(8));
  try { h.substr(10); printf("no throw\n"); } catch (const OutOfRange&) { printf("substr\n"); }
  try { WString e = h; e.erase(10, 1); printf("no throw\n"); } catch (const OutOfRange&) { printf("erase\n"); }
  try { WString e = h; e.insert(10, L"x"); printf("no throw\n"); } catch (const OutOfRange&) { printf("insert\n"); }
  try { WString e = h; e.insert(10, 1, L'x'); printf("no throw\n"); } catch (const OutOfRange&) { printf("insert count\n"); }
  // iterators
  WString r = L"abc";
  for (auto i = r.rbegin(); i != r.rend(); ++i)
    printf("%c", (char)*i);
  printf("\n");
  for (wchar_t& c : r)
    c = (wchar_t)(c + 1);
  r.front() = L'A';
  r.back() = L'Z';
  r[1] = L'-';
  text("written", r);
  WString moved = move(r);
  text("moved", moved);
}
int main()
{
  family();
  wide();
  return 0;
}
