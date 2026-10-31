// The converter's test of std::variant: a variant becomes a typed union (`union enum`, design.md
// 8.5), and what this file prints compiled as C++ (convert_variant.expected) is what its conversion
// must print. The uses the game has: construction from an alternative, assignment, index(),
// holds_alternative, get and get_if by type, visit, comparison. (The tests' SaveLoadStdVariant
// has the rest: two alternatives of one type, in_place_index, converting construction.)
#include <cstdio>
#include <string>
#include <variant>

struct Walking
{
  int speed = 0;
  bool operator==(const Walking& other) const = default;
};

struct Flying
{
  double height = 0;
  bool operator==(const Flying& other) const = default;
};

class Unit
{
public:
  using State = std::variant<Walking, Flying>;
  State state = Walking{3};

  const char* describe() const
  {
    return std::visit([](const auto& s) { return sizeof(s) == sizeof(Walking) ? "walking" : "flying"; }, this->state);
  }
};


int main()
{
  Unit unit;
  printf("%zu %d %d %s\n", unit.state.index(), int(std::holds_alternative<Walking>(unit.state)), std::get<Walking>(unit.state).speed, unit.describe());
  unit.state = Flying{2.5};
  const Flying* flying = std::get_if<Flying>(&unit.state);
  printf("%zu %.1f %d %s\n", unit.state.index(), flying ? flying->height : -1.0, int(std::get_if<Walking>(&unit.state) == nullptr), unit.describe());
  Unit other;
  other.state = Flying{2.5};
  printf("equal %d\n", int(unit.state == other.state));
  return 0;
}
