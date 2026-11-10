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

// The width with the padding.
int Widget::padded(int left, int right) const
{
  int total = this->width + left + right;
  return total;
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

int Widget::unnamed(int amount, int scale) const
{
  return (this->height + amount) * scale;
}

int Widget::shadowed(int instances) const
{
  return instances * 10 + this->width;
}

int Widget::summed(const std::pair<int, int>& sizes) const
{
  return sizes.first + sizes.second + this->width;
}

namespace inner
{
  int Indented::value() const
  {
    return 1;
  }
}

int Registration::count = 0;
template<class T> Registration Registered<T>::registration;

template class Plain<int>;
template class Registered<int>;
