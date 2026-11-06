// The converter's test of member function bodies written in their classes (kovarex, 2026-11-04):
// Widget.hpp and Widget.cpp make one Widget.kpp, whose class holds each body at its declaration.
// What main.cpp prints compiled as C++ (convert_inline.expected) is what its conversion must print.
#pragma once
#include <utility>

class Widget
{
public:
  Widget(int width, int height);
  ~Widget();
  int area() const;
  static int count();
  int scaled(int factor = 2) const;
  bool operator==(const Widget& other) const;
  int renamed(int value) const; // named `amount` in the definition: the declaration takes its name
  int unnamed(int, int scale = 1) const; // `amount` unnamed here
  int summed(const std::pair<int, int>&) const; // a template's type, unnamed
  // `instances` in the definition, a name its default argument uses: stays outside its class
  int shadowed(int value = instances) const;
  template<typename T> T as() const;
  int inHeader() const;

private:
  int width;
  int height;
  static int instances;
};

inline int Widget::inHeader() const
{
  return this->width - this->height;
}

template<typename T> T Widget::as() const
{
  return T(this->width);
}
