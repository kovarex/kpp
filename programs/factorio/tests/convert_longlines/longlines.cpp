// tests\convert_longlines: X-macro lists, whose expansions the converter writes on one line each, laid out as the
// code style lays them out (StyleLongLines.inl; Factorio's InputAction.hpp/.cpp wrote lines of up to 172,566
// characters): an enum's constants filled to the line's length, a switch's cases one to a line, a case label's values
// filled and lined up after `case `, definitions one to a line and expanded when they do not fit, an if's and else's
// statement on the next line a step in, an empty statement `;` kept on its statement's line. convert.cmd compares
// the conversion with tests\convert_longlines.expected.kpp, builds it and runs it (tests\convert_longlines.expected).
#include <cstdio>
#include <cstdint>

#define CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(MACRO) \
  MACRO(Nothing) \
  MACRO(SomethingWithAVeryLongName) \
  MACRO(AnotherThingWithAnEvenLongerName) \
  MACRO(YetAnotherThingWithALongName) \
  MACRO(TheLastOneWithoutASizeOfItsOwn)

#define CALL_ON_EVERY_SHAPE_WITH_SIZE(MACRO) \
  MACRO(SquareWithAVeryLongNameIndeed, int32_t, side) \
  MACRO(RectangleWithAVeryLongNameIndeed, int64_t, area) \
  MACRO(CircleWithAVeryLongNameIndeedToo, double, radius) \
  MACRO(TriangleWithAVeryLongNameIndeedToo, float, height)

enum class Shape : uint8_t
{
#define CREATE_SHAPE(NAME) NAME,
  CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(CREATE_SHAPE)
#undef CREATE_SHAPE
#define CREATE_SHAPE(NAME, TYPE, VALUE) NAME,
  CALL_ON_EVERY_SHAPE_WITH_SIZE(CREATE_SHAPE)
#undef CREATE_SHAPE
};

struct Holder
{
  static bool noSize(Shape shape)
  {
    switch (shape)
    {
#define NO_SIZE(NAME) case Shape::NAME:
      CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(NO_SIZE)
#undef NO_SIZE
        return true;
      default:
        return false;
    }
  }

  static const char* name(Shape shape)
  {
    switch (shape)
    {
#define NAME_OF(NAME) case Shape::NAME: return #NAME;
      CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(NAME_OF)
#undef NAME_OF
#define NAME_OF(NAME, TYPE, VALUE) case Shape::NAME: return #NAME;
      CALL_ON_EVERY_SHAPE_WITH_SIZE(NAME_OF)
#undef NAME_OF
    }
    return "?";
  }

#define MAKE_NAMED(NAME) \
  static Holder NAME() { return Holder(Shape::NAME); }
  CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(MAKE_NAMED)
#undef MAKE_NAMED

#define CREATE_GETTER(NAME, TYPE, VALUE) \
  TYPE get ## NAME() const \
  { \
    if constexpr (sizeof(TYPE) > sizeof(int32_t)) \
      return TYPE(this->value) * 2; \
    else /* the small ones as they are */ \
      return TYPE(this->value); \
  }
  CALL_ON_EVERY_SHAPE_WITH_SIZE(CREATE_GETTER)
#undef CREATE_GETTER

  Holder(Shape shape) : shape(shape), value(int(shape)) {}
  Shape shape;
  int value;
};

int main()
{
  int sum = 0;
#define ADD(NAME) sum += Holder::NAME().value + Holder::noSize(Shape::NAME); printf("%s\n", Holder::name(Shape::NAME));;
  CALL_ON_EVERY_SHAPE_WITHOUT_SIZE(ADD)
#undef ADD
  Holder h(Shape::CircleWithAVeryLongNameIndeedToo);
  printf("%d %g ", sum, h.getCircleWithAVeryLongNameIndeedToo());
  printf("%g\n", double(h.getSquareWithAVeryLongNameIndeed()));
  return 0;
}
