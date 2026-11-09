// The operator!= the converter keeps for review, where it cannot show one to be the negation of the class's ==: one
// that compares another field, a virtual one, one the class's == is written through. kpp refuses each (kpp's design.md
// 4: `a != b` is only ever `!(a == b)`), so a person decides what the == says: the game's are hand edits in
// overrides\source-edits.txt. convert.cmd checks that the 3 are left in the converted text and that kpp refuses exactly
// these 3.
struct Other
{
  int x, y;
  bool operator==(const Other& other) const { return x == other.x; }
  bool operator!=(const Other& other) const { return y != other.y; }
};

struct Shape
{
  int sides;
  virtual ~Shape() {}
  virtual bool operator!=(const Shape& other) const { return sides != other.sides; }
};

struct Through
{
  int v;
  bool operator==(const Through& other) const { return !(*this != other); }
  bool operator!=(const Through& other) const { return v != other.v; }
};

int keptUses()
{
  Other x1{1, 2}, x2{1, 3};
  Shape h1, h2;
  h1.sides = 3;
  h2.sides = 4;
  Through t1{8}, t2{8};
  return (x1 != x2) + (x1 == x2) + (h1 != h2) + (t1 != t2) + (t1 == t2);
}
