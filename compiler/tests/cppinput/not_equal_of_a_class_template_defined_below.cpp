// cppinput: a class template's `operator!=` declared in the class and defined below it (its body stays outside) is
// treated as one defined in the class: made the `==` it negates where there is no `==` of its parameters (Unreal's
// TDirectoryTree::TIterator against its sentinel), the declaration with the definition; dropped with its definition
// where it is `!(*this == o)`; and a deleted `!=` beside a deleted `==` goes (kpp's `a != b` is `!(a == b)`)
struct Sentinel
{
};

template<typename T>
struct Range
{
  T* at;
  T* end;
  bool operator!=(const Sentinel&) const;
  friend bool operator==(const Range&, const Range&) = delete;
  friend bool operator!=(const Range&, const Range&) = delete;
};

template<typename T>
bool Range<T>::operator!=(const Sentinel&) const
{
  return at != end;
}

template<typename T>
struct Box
{
  T v;
  bool operator==(const Box& o) const { return v == o.v; }
  bool operator!=(const Box& o) const;
};

template<typename T>
bool Box<T>::operator!=(const Box& o) const
{
  return !(*this == o);
}

int main()
{
  int values[2] = {1, 2};
  Range<int> r{values, values + 2};
  int count = 0;
  while (r != Sentinel{})
  {
    ++r.at;
    ++count;
  }
  Box<int> a{1}, b{2};
  return count == 2 && a != b && !(a != a) ? 0 : 1;
}
