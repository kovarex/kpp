// tests\convert_slips: what kpp's likely-slip errors take for a mistake, written as the C++ meant it: an `if`'s
// or `else`'s empty body `;` as `{}`, and an assignment as a condition in its own parentheses (a declaration
// in a condition stays as it is). convert.cmd compares the conversion with tests\convert_slips.expected.kpp and
// builds it with --slip-checks and runs it.
#include <cstdio>

struct Node
{
  Node* parent = nullptr;
  int value = 0;
};

int* find(int* p) { return p; }

int count(const char* pattern)
{
  int walls = 0;
  for (int x = 0; pattern[x]; x++)
    if (pattern[x] == ' ')
      ;
    else if (pattern[x] == '#')
      walls++;
    else
      ; // anything else
  return walls;
}

int main()
{
  int number = 5;
  int* p = nullptr;
  int found = 0;
  if (p = find(&number))
    found += *p;
  bool both = false;
  if (both = found > 0 && number > 0)
    found += 1;
  Node root{nullptr, 1};
  Node child{&root, 2};
  Node* node = &child;
  int depth = 0;
  while (node = node->parent)
    depth++;
  Node* walk = &child;
  do
    depth++;
  while(walk = walk->parent);
  if (int* q = find(&number))
    found += *q;
  if (found == 11)
    ;
  printf("%d %d %d %d\n", count("# #  ##"), found, depth, both ? 1 : 0);
  return 0;
}
