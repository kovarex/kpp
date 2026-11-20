// cppinput: a type named in capitals before a class template's member defined outside the class is
// the member's return type, not a macro before a template type (Unreal's DirectoryTree.h: `inline
// SIZE_T TDirectoryTree<ValueType>::GetAllocatedSize() const`)
typedef unsigned long long SIZE_T;
template<typename T> struct Tree
{
  T values[3];
  SIZE_T allocatedSize() const;
};
template<typename T>
inline SIZE_T Tree<T>::allocatedSize() const
{
  return values[0] + values[2];
}

int main()
{
  Tree<int> tree{{4, 5, 6}};
  return tree.allocatedSize() == 10 ? 0 : 1;
}
