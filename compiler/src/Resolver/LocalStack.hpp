#pragma once
// Part of the resolver: GlobalScopeImpl.hpp includes it inside namespace resolver_detail.

// Local declarations inside a function body, template parameter lists and lambdas, kept as a
// stack that is searched backwards.
struct LocalStack
{
  struct Item
  {
    uint32_t symbol;
    Decl* decl;
    // The type this walk deduced for a local whose declared type has no initializer to deduce
    // from (a range-for variable): per walk, since one template body is typed concurrently for
    // several instantiations.
    const Ty* type = nullptr;
  };
  // A class declared in a function body (Walker's walk of its members): the items below floor
  // are the locals of the functions around it, which a name in a member function finds only
  // after the class's scope, its members and bases ([basic.lookup.unqual]; lookupUnqualified).
  // The innermost class last.
  struct ClassFrame
  {
    size_t floor;
    Scope* classScope;
  };
  // The symbol of an item that is a body's `using namespace N;` (decl the UsingDirectiveDecl):
  // no name, a namespace for unqualified lookups to try.
  static constexpr uint32_t usingDirective = UINT32_MAX - 1;
  // The symbol of an item that only carries a nameless local's type for this walk (the hidden
  // object of structured bindings, while their get<i> calls are typed: Walker::tupleBindings).
  static constexpr uint32_t hiddenObject = UINT32_MAX - 2;
  size_t mark() const { return items.size(); }
  void pop(size_t mark) { items.resize(mark); }
  void push(uint32_t symbol, Decl* decl)
  {
    if (symbol != UINT32_MAX)
      items.push_back({symbol, decl});
  }
  Decl* find(uint32_t symbol) const
  {
    for (size_t i = items.size(); i-- > 0;)
      if (items[i].symbol == symbol)
        return items[i].decl;
    return nullptr;
  }
  const Ty* deducedTypeOf(const Decl* decl) const
  {
    for (size_t i = items.size(); i-- > 0;)
      if (items[i].decl == decl)
        return items[i].type;
    return nullptr;
  }

  std::vector<Item> items;
  std::vector<ClassFrame> classFrames;
};
