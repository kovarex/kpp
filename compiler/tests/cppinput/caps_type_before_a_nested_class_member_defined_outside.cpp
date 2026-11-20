// cppinput: a type named in capitals before the member of a class template's nested class, defined
// outside both (Unreal's DirectoryTree.h: `inline SIZE_T TDirectoryTree<ValueType>::FTreeNode::GetAllocatedSize() const`)
typedef unsigned long long SIZE_T;
template<typename T> struct Tree
{
  struct Node
  {
    T value;
    SIZE_T weight() const;
  };
  Node root;
};
template<typename T>
inline SIZE_T Tree<T>::Node::weight() const
{
  return value * 2;
}

int main()
{
  Tree<int> tree{{21}};
  return tree.root.weight() == 42 ? 0 : 1;
}
