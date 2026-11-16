#include <typeinfo>
#include <stdio.h>
template<class T> struct Und { enum class E : T { a = 2 }; };
template<class T> struct Same { enum : T { v = 1 }; };
template<class A, class B> struct SameT { static const bool value = false; };
template<class A> struct SameT<A, A> { static const bool value = true; };
template<class T> const char* over(T) { return "generic"; }
const char* over(Und<short>::E) { return "short"; }
const char* over(Und<long long>::E) { return "long long"; }
int main()
{
  printf("%d\n", typeid(Und<short>::E) == typeid(Und<int>::E));
  printf("%d\n", typeid(Und<short>::E) == typeid(Und<short>::E));
  Und<short>::E s = Und<short>::E::a;
  Und<int>::E i = Und<int>::E::a;
  printf("%d\n", typeid(s) == typeid(i));
  printf("%d %d\n", SameT<Und<short>::E, Und<int>::E>::value, SameT<Und<short>::E, Und<short>::E>::value);
  printf("%s %s %s\n", over(Und<short>::E::a), over(Und<long long>::E::a), over(Und<int>::E::a));
  try
  {
    throw Und<short>::E::a;
  }
  catch (Und<int>::E) { printf("wrong\n"); }
  catch (Und<short>::E e) { printf("right %d\n", (int)e); }
  return 0;
}
