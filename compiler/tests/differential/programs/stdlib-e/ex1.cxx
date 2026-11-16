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
#include <exception>
using ExceptionPtr = std::exception_ptr;
#define makeExceptionPtr std::make_exception_ptr
#define rethrowException std::rethrow_exception
using std::uncaught_exceptions;
// Exceptions and object lifetime against C++: the order objects are destroyed in while unwinding
// (nested scopes, a partly built array, aggregate and class; the members and bases of a constructor
// that throws), function-try-blocks, rethrow, nested try, catch by base, by reference and by value,
// exception pointers, temporaries' lifetime (full expressions, a reference bound to one, range-for),
// a destructor that throws when no other exception is in flight.
char logText[4096];
int logLength = 0;
void note(const char* text)
{
  while (*text)
    logText[logLength++] = *text++;
  logText[logLength++] = ' ';
}
void noteNumber(char kind, int id)
{
  char text[16];
  snprintf(text, 16, "%c%d", kind, id);
  note(text);
}
void flush(const char* label)
{
  logText[logLength] = 0;
  printf("%-30s %s\n", label, logText);
  logLength = 0;
}
int throwAt = -1;
struct Noisy
{
  int id;
  Noisy(int i) : id(i)
  {
    if (i == throwAt)
    {
      noteNumber('!', i);
      throw i;
    }
    noteNumber('+', i);
  }
  Noisy(const Noisy& o) : id(o.id + 100) { noteNumber('c', id); }
  ~Noisy() { noteNumber('-', id); }
};
struct Agg
{
  Noisy a;
  Noisy b;
  Noisy c;
};
class Base
{
public:
  Noisy n1{1};
  Base() { note("Base"); }
  virtual ~Base() { note("~Base"); }
};
class Built : public Base
{
public:
  Noisy n2{2};
  Noisy n3{3};
  Noisy n4{4};
  Built() : Base()
  {
    note("Built body");
    if (throwAt == 99)
      throw 99;
  }
  ~Built() override { note("~Built"); }
};
class Guarded
{
public:
  Noisy member;
  Guarded(int i) try : member(i)
  {
    note("Guarded body");
  }
  catch (int code)
  {
    noteNumber('g', code);
  }
};
int guardedFunction(int x) try
{
  if (x > 0)
    throw x;
  return 0;
}
catch (int code)
{
  noteNumber('f', code);
  return -code;
}
class Error
{
public:
  int code;
  Error(int c) : code(c) {}
  Error(const Error& o) : code(o.code) { note("copyError"); }
  virtual ~Error() {}
  virtual const char* name() const { return "Error"; }
};
class IoError : public Error
{
public:
  IoError(int c) : Error(c) {}
  IoError(const IoError& o) : Error(o) { note("copyIoError"); }
  const char* name() const override { return "IoError"; }
};
struct ThrowsInDestructor
{
  ~ThrowsInDestructor() noexcept(false)
  {
    note("~ThrowsInDestructor");
    throw 42;
  }
};
struct Counter
{
  ~Counter() { noteNumber('u', (int)uncaught_exceptions()); }
};
Noisy make(int i) { return Noisy(i); }
struct Holder
{
  Noisy items[2] = {Noisy(70), Noisy(71)};
};
int value(const Noisy& n) { return n.id; }

int main()
{
  try
  {
    Noisy a(1);
    {
      Noisy b(2);
      Noisy c(3);
      throw 5;
    }
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("nested scopes");
  throwAt = 13;
  try
  {
    Noisy array[5] = {Noisy(10), Noisy(11), Noisy(12), Noisy(13), Noisy(14)};
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("array, the 4th throws");
  throwAt = 22;
  try
  {
    Agg agg{Noisy(20), Noisy(21), Noisy(22)};
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("aggregate, the 3rd throws");
  throwAt = 3;
  try
  {
    Built built;
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("member 3 throws");
  throwAt = 99;
  try
  {
    Built built;
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("body throws");
  throwAt = 1;
  try
  {
    Built built;
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("base's member throws");
  throwAt = 31;
  try
  {
    Guarded g(31);
    note("not reached");
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("function-try-block constructor");
  throwAt = -1;
  {
    Guarded g(32);
  }
  noteNumber('r', guardedFunction(4));
  noteNumber('r', guardedFunction(0));
  flush("function-try-block");
  try
  {
    try
    {
      throw IoError(7);
    }
    catch (Error& e)
    {
      note(e.name());
      try
      {
        throw 8;
      }
      catch (int inner)
      {
        noteNumber('i', inner);
      }
      throw;
    }
  }
  catch (IoError& e)
  {
    note("outer");
    note(e.name());
    noteNumber('#', e.code);
  }
  flush("rethrow after a nested try");
  try
  {
    throw IoError(9);
  }
  catch (Error e)
  {
    note(e.name());
  }
  flush("catch by value slices");
  try
  {
    throw IoError(10);
  }
  catch (const IoError& e)
  {
    note(e.name());
  }
  catch (const Error& e)
  {
    note("wrong handler");
  }
  flush("first matching handler");
  try
  {
    throw Error(11);
  }
  catch (IoError&)
  {
    note("wrong handler");
  }
  catch (...)
  {
    note("catch-all");
  }
  flush("catch-all after no match");
  try
  {
    try
    {
      throw IoError(12);
    }
    catch (Error& e)
    {
      e.code = 120;
      throw;
    }
  }
  catch (Error& e)
  {
    noteNumber('#', e.code);
  }
  flush("rethrow keeps the object");
  try
  {
    try
    {
      throw IoError(13);
    }
    catch (Error& e)
    {
      throw e;
    }
  }
  catch (IoError&)
  {
    note("wrong handler");
  }
  catch (Error& e)
  {
    note(e.name());
  }
  flush("throw e copies the static type");
  ExceptionPtr p = makeExceptionPtr(IoError(14));
  note(p ? "set" : "empty");
  for (int i = 0; i < 2; ++i)
    try
    {
      rethrowException(p);
    }
    catch (const Error& e)
    {
      note(e.name());
      noteNumber('#', e.code);
    }
  ExceptionPtr none;
  note(none ? "set" : "empty");
  note(none == nullptr ? "null" : "not null");
  flush("exception pointer");
  logLength = 0;
  {
    int v = value(make(50)) + value(make(51));
    noteNumber('=', v);
    flush("temporaries end with the line");
    const Noisy& kept = make(52);
    note("bound");
    noteNumber('=', kept.id);
  }
  flush("a reference extends one");
  for (const Noisy& n : Holder().items)
    noteNumber('=', n.id);
  note("after");
  flush("range-for over a temporary's member");
  try
  {
    ThrowsInDestructor t;
    note("body");
  }
  catch (int code)
  {
    noteNumber('*', code);
  }
  flush("destructor throws");
  {
    Counter outside;
  }
  try
  {
    Counter during;
    throw 1;
  }
  catch (int)
  {
    Counter inHandler;
  }
  try
  {
    try
    {
      Counter first;
      throw 1;
    }
    catch (int)
    {
      Counter rethrowing;
      throw;
    }
  }
  catch (int)
  {
  }
  flush("uncaught_exceptions");
  return 0;
}
