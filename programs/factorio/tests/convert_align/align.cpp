// tests\convert_align: continuation lines keep their alignment when the conversion changes the length of the text
// before what they align to (a std name one shorter in kpp, an integer type's name, `this` as a reference), and a
// block's lines follow the block, never an alignment (an `if` whose condition changed length).
// convert.cmd compares the conversion, `this` written as a reference, with tests\convert_align.expected.kpp and runs it.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

struct Box
{
  int a;
  int b;

  int total(int extra,
            int more) const
  {
    return this->sum(this->a, this->b,
                     extra + more);
  }

  int sum(int x, int y, int z) const { return x + y + z; }
};

std::unique_ptr<Box> make(int a,
                          int b)
{
  return std::make_unique<Box>(Box{a,
                                   b});
}

int main()
{
  std::unique_ptr<Box> box(make(uint32_t(1) + uint32_t(2),
                                4));
  if (std::make_unique<Box>(Box{1, 2})->total(1, 2) > 0 &&
      box->a == 3)
  {
    int kept = 1;
    printf("%d %d\n", box->total(kept,
                                 2), kept);
  }
  return 0;
}
