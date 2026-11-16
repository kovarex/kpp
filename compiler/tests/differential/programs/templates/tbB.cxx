#include <stdio.h>
#include <compare>
#include <compare>
#include <string.h>
#include <stdint.h>
#include <initializer_list>
#include <new>
#include <tuple>
#include <utility>
template <typename... T> using Tuple = std::tuple<T...>;
template <typename A, typename B> using Pair = std::pair<A, B>;
using std::get;
using std::tie;
Tuple<int*, bool> lookUp(int* where) { return Tuple<int*, bool>(where, true); }

template <typename T, typename... Args>
int inTemplate(int* where, Args&&... args)
{
  auto [it, created] = lookUp(where);
  int before = (0 + ... + args);
  if (created)
    *it = T(before * 2);
  *it += (args + ...);
  return *it + int(created);
}

template <typename... Args>
int bothSides(Args... args)
{
  Tuple<int, int> t(1, 2);
  auto& [a, b] = t;
  a += (args + ...);
  auto f = [&] { b += (args * ...); return a * 100 + b; };
  int r = f();
  return r + get<0>(t) * 10000;
}

template <typename... Args>
int inLambdaInTemplate(Args... args)
{
  Tuple<int, int> rows[2] = {Tuple<int, int>(1, 4), Tuple<int, int>(2, 5)};
  int sum = 0;
  auto run = [&]
  {
    for (auto& [key, value] : rows)
    {
      value += sizeof...(args);
      sum += key * 10 + value + (args + ...);
    }
  };
  run();
  return sum + get<1>(rows[1]);
}

template <typename T, typename... Rest>
int recursiveWithBinding(T first, Rest... rest)
{
  auto [x, y] = Tuple<T, int>(first, int(sizeof...(rest)));
  if constexpr (sizeof...(rest) == 0)
    return int(x) + y;
  else
    return int(x) * 10 + y + recursiveWithBinding(rest...);
}

int main()
{
  int x = 0;
  printf("%d\n", inTemplate<int>(&x, 7));
  int r2 = inTemplate<int>(&x, 1, 2, 3);
  printf("%d %d\n", r2, x);
  printf("%d\n", bothSides(2, 3));
  printf("%d\n", inLambdaInTemplate(1, 1));
  printf("%d\n", recursiveWithBinding(1, 2, 3));
  return 0;
}
