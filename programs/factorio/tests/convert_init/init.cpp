// The converter's test of namespace- and class-scope objects initialized by parentheses: at those
// scopes kpp reads `T f(...)` as a function always (design.md 4, the most vexing parse), so the
// conversion writes the object with braces, or with `=` where braces would choose another
// constructor (a class with an initializer_list constructor, whose list kpp's braces take first) or
// the class is the library's. What this file prints compiled as C++ (convert_init.expected) is what
// its conversion must print.
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <string>

struct Size
{
  int value;
  explicit Size(int value) : value(value) {}
};

class Widget
{
public:
  Widget(Size size) : width(size.value) {}
  Widget(int width, int height) : width(width * height) {}
  int width;
};

// Braces would take the list: `Bag b{3, 4}` is the list 3, 4 (count 2), `Bag b(3, 4)` count 3 of 4.
class Bag
{
public:
  Bag(std::initializer_list<int> values) : count(int(values.size())), first(*values.begin()) {}
  Bag(int count, int value) : count(count), first(value) {}
  int count;
  int first;
};

enum class Color { Red, Green };

// Only names in the parentheses, macros among them (CurrentVersion.cpp's
// `ApplicationVersion::currentVersion(VERSION_STRING, BUILD_VERSION, PLATFORM)`): C++ preprocesses
// them into the object's arguments.
#define VERSION_TEXT "1.2.3"
#define PLATFORM_TEXT "win64"
const int buildNumber = 84321;

class Version
{
public:
  Version(const char* number, int build, const char* platform) : number(number), build(build), platform(platform) {}
  const char* number;
  int build;
  const char* platform;
  static const Version current;
};

using WidgetAlias = Widget;

class Station
{
public:
  static const Widget empty;
  static const std::string name;
};

static constexpr int limit(7);
Widget widget(Size(3));
Widget area(4, 5);
const Widget constWidget(Size(6));
Bag bag(3, 4);
Color color(Color::Green);
WidgetAlias aliased(Size(8));
std::atomic<bool> flag(true);
std::string text("text");
const std::string constText("const text");
const int* pointer(&limit);
const Widget Station::empty(Size(9));
const std::string Station::name("station");
const Version Version::current(VERSION_TEXT, buildNumber, PLATFORM_TEXT);

// One a macro's expansion declares (UnitTest++'s TEST: `UnitTest::ListAdder adder##Fixture##Name
// (List, &test##Fixture##Name##Instance);`).
#define REGISTER(Name) Widget registered##Name (Size(12));
REGISTER(First)

namespace inner
{
  Widget nested(Size(10));
  Bag nestedBag(2, 11);
}

int main()
{
  printf("limit %d\n", limit);
  printf("widget %d, area %d, const %d\n", widget.width, area.width, constWidget.width);
  printf("bag %d of %d\n", bag.count, bag.first);
  printf("color %d\n", int(color));
  printf("aliased %d\n", aliased.width);
  printf("flag %d\n", int(flag.load()));
  printf("text %s, %s\n", text.c_str(), constText.c_str());
  printf("pointer %d\n", *pointer);
  printf("station %d %s\n", Station::empty.width, Station::name.c_str());
  printf("version %s %d %s\n", Version::current.number, Version::current.build, Version::current.platform);
  printf("registered %d\n", registeredFirst.width);
  printf("inner %d, bag %d of %d\n", inner::nested.width, inner::nestedBag.count, inner::nestedBag.first);
  return 0;
}
