#include "Widget.hpp"

int Widget::instances = 0;

Widget::Widget(int width, int height)
  : width(width)
  , height(height)
{
  ++Widget::instances;
}

Widget::~Widget()
{
  --Widget::instances;
}

// The area, in square units.
int Widget::area() const
{
  return this->width * this->height;
}

int Widget::count()
{
  return Widget::instances;
}

int Widget::scaled(int factor) const
{
  return this->area() * factor;
}

bool Widget::operator==(const Widget& other) const
{
  return this->width == other.width && this->height == other.height;
}

int Widget::renamed(int amount) const
{
  return this->width + amount;
}
