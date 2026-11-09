// The .natvis of a program (natvis.hpp): fixed rules for the library's containers, each checked
// against the members it reads, and one rule per HashTable instance, whose slots are cast to its
// entry type by name. Type names are kpp's (TypeTable::toString, qualified), which the PDB's are
// (docs/debugging.md, "Values in Visual Studio": agreed with the code generator).
#include "natvis.hpp"
#include "codegen.hpp"
#include "resolver.hpp"
#include "TypeTable.hpp"
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
  std::string escape(const std::string& text)
  {
    std::string out;
    for (char c : text)
      if (c == '<')
        out += "&lt;";
      else if (c == '>')
        out += "&gt;";
      else if (c == '&')
        out += "&amp;";
      else if (c == '"')
        out += "&quot;";
      else
        out += c;
    return out;
  }

  std::string nameOf(const QualifiedName& name, const SymbolTable& symbols)
  {
    return name.parts.empty() || name.parts[name.parts.size() - 1].symbol == UINT32_MAX ? std::string()
                                                                                          : std::string(symbols.name(name.parts[name.parts.size() - 1].symbol));
  }

  // A class's data members by name: its own, and an anonymous struct's or union's in it (C++ reaches
  // those by name, and LLVM's CodeView puts them in the class).
  void dataMembers(const ClassDecl* c, const SymbolTable& symbols, std::set<std::string>& out)
  {
    for (const Decl* m : c->members)
      if (m->kind == NodeKind::VariableDecl && !(m->flags & DF_Static))
        out.insert(nameOf(static_cast<const VariableDecl*>(m)->name, symbols));
      else if (m->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(m)->name.parts.empty())
        dataMembers(static_cast<const ClassDecl*>(m), symbols, out);
  }

  // A rule for the library's classes: the classes it names and the members it reads of each, its
  // XML, and the members it reads of other classes (by name: a specialization of those is no
  // concern, the rule naming only the classes it reads through).
  struct Rule
  {
    std::vector<std::string> classes;
    std::vector<std::string> members;
    std::string xml;
    std::vector<std::pair<std::string, std::vector<std::string>>> reads = {};
  };

  // A declaration's text, its tokens joined by one space: what a rule that copies the library's
  // computation compares with.
  std::string tokenText(const Decl* d, const std::vector<Token>& tokens, std::string_view text)
  {
    std::string out;
    for (uint32_t i = d->token; i < d->endToken && i < tokens.size(); ++i)
    {
      if (!out.empty())
        out += ' ';
      out += text.substr(tokens[i].offset, tokens[i].length);
    }
    return out;
  }

  // The library's literal: largeMark's `0x80`, for the rule to compare with.
  bool literalOf(const ClassDecl* c, const std::string& member, const SymbolTable& symbols, const std::vector<Token>& tokens, std::string_view text,
                 std::string& value)
  {
    for (const Decl* m : c->members)
      if (m->kind == NodeKind::VariableDecl && nameOf(static_cast<const VariableDecl*>(m)->name, symbols) == member)
      {
        const Expr* init = static_cast<const VariableDecl*>(m)->init;
        if (!init || init->kind != NodeKind::LiteralExpr || init->token >= tokens.size())
          return false;
        const Token& t = tokens[init->token];
        if (t.getKind() != TokenKind::Number)
          return false;
        value = std::string(text.substr(t.offset, t.length));
        return true;
      }
    return false;
  }
}

std::string makeNatvis(const std::vector<ParsedFile*>& files, GlobalScope& global, const SymbolTable& symbols, const std::vector<DebugClass>& classes,
                       std::vector<std::string>& problems)
{
  // The library's class definitions at namespace scope, by name.
  // A specialization (`Vector<bool>`) is named with /spec<token> in the PDB (B's names): a wildcard
  // rule misses it, so one of a class with a rule is a problem.
  std::map<std::string, std::pair<const ClassDecl*, const ParsedFile*>> library;
  std::set<std::string> specialized;
  std::function<void(Span<Decl*>, const ParsedFile*)> collect = [&](Span<Decl*> decls, const ParsedFile* file)
  {
    for (const Decl* d : decls)
      if (d->kind == NodeKind::ClassDecl && !static_cast<const ClassDecl*>(d)->name.parts.empty() &&
          static_cast<const ClassDecl*>(d)->name.parts[static_cast<const ClassDecl*>(d)->name.parts.size() - 1].hasTemplateArgs)
        specialized.insert(nameOf(static_cast<const ClassDecl*>(d)->name, symbols));
      else if (d->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(d)->isDefinition)
        library.emplace(nameOf(static_cast<const ClassDecl*>(d)->name, symbols), std::make_pair(static_cast<const ClassDecl*>(d), file));
      else if (d->kind == NodeKind::NamespaceDecl)
        collect(static_cast<const NamespaceDecl*>(d)->decls, file);
  };
  bool scoped = false; // the standard library is the scoped library std
  for (const ParsedFile* f : files)
    if (f->isLibrary)
    {
      collect(f->decls, f);
      scoped = scoped || f->libraryScope != UINT32_MAX;
    }

  std::set<std::string> broken; // rules left out for what they compute: by their first class
  std::string largeMark;
  if (auto s = library.find("String"); s == library.end() || !s->second.second->tokens ||
                                       !literalOf(s->second.first, "largeMark", symbols, *s->second.second->tokens, s->second.second->text, largeMark))
  {
    problems.push_back("String's largeMark is no number literal: the String rule is left out");
    broken.insert("String");
  }

  // `mark` is a String's last byte: a small string's length, or largeMark for one on the heap.
  const std::string mark = "(unsigned char)smallChars[sizeof(smallChars) - 1]";
  // (written out, no <Intrinsic>: a natvis file given to an evaluation by name, as Visual Studio's kpp evaluator
  // does, does not apply a type that has one; docs/debugging.md, "this in the watch window")
  const std::string large = "(" + mark + " == " + largeMark + ")";
  const std::string block = "(sizeof(**map) &gt; 16 ? 16 : sizeof(**map) &gt; 8 ? 32 : sizeof(**map) &gt; 4 ? 64 : "
                            "sizeof(**map) &gt; 2 ? 128 : sizeof(**map) &gt; 1 ? 256 : 512)";
  std::vector<Rule> rules = {
    {{"String"}, {"smallChars", "heapChars", "heapLength", "heapCapacity"},
     "  <Type Name=\"String\">\n"
     "    <DisplayString Condition=\"" + large + "\">{heapChars,[heapLength]s8}</DisplayString>\n"
     "    <DisplayString>{smallChars,[" + mark + "]s8}</DisplayString>\n"
     "    <StringView Condition=\"" + large + "\">heapChars,[heapLength]s8</StringView>\n"
     "    <StringView>smallChars,[" + mark + "]s8</StringView>\n"
     "    <Expand>\n"
     "      <Item Name=\"[length]\" Condition=\"" + large + "\">heapLength</Item>\n"
     "      <Item Name=\"[length]\" Condition=\"!" + large + "\">(int)" + mark + "</Item>\n"
     "      <Item Name=\"[capacity]\" Condition=\"" + large + "\">heapCapacity</Item>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"StringView"}, {"chars", "byteCount"},
     "  <Type Name=\"StringView\">\n"
     "    <DisplayString>{chars,[byteCount]s8}</DisplayString>\n"
     "    <StringView>chars,[byteCount]s8</StringView>\n"
     "    <Expand>\n"
     "      <Item Name=\"[length]\">byteCount</Item>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Vector"}, {"items", "length", "allocated"},
     "  <Type Name=\"Vector&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[capacity]\" ExcludeView=\"simple\">allocated</Item>\n"
     "      <ArrayItems>\n"
     "        <Size>length</Size>\n"
     "        <ValuePointer>items</ValuePointer>\n"
     "      </ArrayItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Span"}, {"items", "length"},
     "  <Type Name=\"Span&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <ArrayItems>\n"
     "        <Size>length</Size>\n"
     "        <ValuePointer>items</ValuePointer>\n"
     "      </ArrayItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    // SmartPointer: `->` and `*` in a watch expression reach the object, as for std::unique_ptr
    // (`entity->controlBehavior->readCharge`, controlBehavior a T^).
    {{"UniquePtr"}, {"held"},
     "  <Type Name=\"UniquePtr&lt;*&gt;\">\n"
     "    <SmartPointer Usage=\"Minimal\">held</SmartPointer>\n"
     "    <DisplayString Condition=\"held == 0\">empty</DisplayString>\n"
     "    <DisplayString>{*held}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"held != 0\">*held</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    // A SharedControl's strong count is the owners'; its weak one the weak references' plus one
    // while an owner lives (sharedReleaseWeak).
    {{"SharedPtr"}, {"pointer", "control"},
     "  <Type Name=\"SharedPtr&lt;*&gt;\">\n"
     "    <SmartPointer Usage=\"Minimal\">pointer</SmartPointer>\n"
     "    <DisplayString Condition=\"pointer == 0\">empty</DisplayString>\n"
     "    <DisplayString Condition=\"control == 0\">{*pointer}</DisplayString>\n"
     "    <DisplayString>{*pointer} [{control-&gt;strong.value} strong]</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[use count]\" Condition=\"control != 0\">control-&gt;strong.value</Item>\n"
     "      <Item Name=\"[weak count]\" Condition=\"control != 0\">control-&gt;weak.value - (control-&gt;strong.value != 0)</Item>\n"
     "      <ExpandedItem Condition=\"pointer != 0\">*pointer</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n",
     {{"SharedControl", {"strong", "weak"}}, {"Atomic", {"value"}}}},
    {{"WeakPtr"}, {"pointer", "control"},
     "  <Type Name=\"WeakPtr&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"control == 0\">empty</DisplayString>\n"
     "    <DisplayString Condition=\"control-&gt;strong.value == 0\">expired</DisplayString>\n"
     "    <DisplayString>{*pointer} [{control-&gt;strong.value} strong]</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[use count]\" Condition=\"control != 0\">control-&gt;strong.value</Item>\n"
     "      <ExpandedItem Condition=\"control != 0 &amp;&amp; control-&gt;strong.value != 0\">*pointer</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n",
     {{"SharedControl", {"strong", "weak"}}, {"Atomic", {"value"}}}},
    {{"Map", "Set", "MultiMap", "MultiSet"}, {"tree"},
     "  <Type Name=\"Map&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"Set&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"MultiMap&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"MultiSet&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"OptionalTrivialValue", "OptionalCopiedValue", "OptionalOwnedValue"}, {"storage", "present"},
     "  <Type Name=\"Optional&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"!present\">empty</DisplayString>\n"
     "    <DisplayString>{storage.value}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"present\">storage.value</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Pair"}, {"first", "second"},
     "  <Type Name=\"Pair&lt;*&gt;\">\n"
     "    <DisplayString>({first}, {second})</DisplayString>\n"
     "  </Type>\n"},
    {{"HashMap", "HashSet"}, {"table"},
     "  <Type Name=\"HashMap&lt;*&gt;\">\n"
     "    <DisplayString>{table}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>table</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"HashSet&lt;*&gt;\">\n"
     "    <DisplayString>{table}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>table</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"HashTable"}, {"tags", "slots", "capacity", "count"}, ""},
    // The rules of these are per instance (below): a node's cast names its element type.
    {{"List"}, {"sentinel", "length"}, "", {{"ListLinks", {"next"}}, {"ListNode", {"value"}}}},
    {{"ForwardList"}, {"beforeHead"}, "", {{"ForwardLinks", {"next"}}, {"ForwardNode", {"value"}}}},
    {{"RbTree"}, {"header", "count"}, "", {{"RbNodeBase", {"parent", "left", "right"}}, {"RbNode", {"entry"}}}},
    // Element i is at position start + i of the map's space: block (start + i) / blockSize() at
    // that offset. `block` is Deque::blockSize() (checked below), sizeof(T) taken as sizeof(**map).
    {{"Deque"}, {"map", "start", "length"},
     "  <Type Name=\"Deque&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <IndexListItems>\n"
     "        <Size>length</Size>\n"
     "        <ValueNode>map[(start + $i) / " + block + "][(start + $i) % " + block + "]</ValueNode>\n"
     "      </IndexListItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
  };
  // The Deque rule's `block` is this blockSize: elements per block, about 512 bytes' worth, a power
  // of two, at least 16.
  if (auto d = library.find("Deque"); d != library.end())
  {
    const ParsedFile* file = d->second.second;
    std::string found;
    for (const Decl* m : d->second.first->members)
      if (m->kind == NodeKind::FunctionDecl && nameOf(static_cast<const FunctionDecl*>(m)->name, symbols) == "blockSize" && file->tokens)
        found = tokenText(m, *file->tokens, file->text);
    const std::string expected = "static i32 blockSize ( ) { i32 size = 16 ; while ( ( usize ) size * 2 * sizeof ( T ) <= 512 ) size *= 2 ; return size ; }";
    if (found != expected)
    {
      problems.push_back("Deque's blockSize is not the one its rule computes (" + found + "): the Deque rule is left out");
      broken.insert("Deque");
    }
  }

  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<!-- Written by kpp for a build with debug information (docs/debugging.md, \"Values in Visual Studio\"). -->\n"
                    "<AutoVisualizer xmlns=\"http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n";
  std::set<std::string> ruleOut; // classes whose rule is left out
  for (const Rule& r : rules)
  {
    bool ok = !broken.count(r.classes[0]);
    // The classes it names, with its members, then those it reads through.
    std::vector<std::pair<std::string, std::vector<std::string>>> checked;
    for (const std::string& cls : r.classes)
    {
      if (specialized.count(cls))
        problems.push_back("the library specializes " + cls + ", whose rule names only its primary template: add a rule for the specialization");
      checked.emplace_back(cls, r.members);
    }
    checked.insert(checked.end(), r.reads.begin(), r.reads.end());
    for (const auto& [cls, read] : checked)
    {
      auto found = library.find(cls);
      if (found == library.end())
      {
        problems.push_back("the library has no class " + cls + ": the rule of " + r.classes[0] + " is left out");
        ok = false;
        continue;
      }
      std::set<std::string> members;
      dataMembers(found->second.first, symbols, members);
      for (const std::string& m : read)
        if (!members.count(m))
        {
          problems.push_back(cls + " has no data member " + m + ": the rule of " + r.classes[0] + " is left out");
          ok = false;
        }
    }
    if (ok)
    {
      // (the classes are std::String to the debugger when the library is scoped, design.md 16; a rule per instance below has its name from the debug information)
      std::string xml = r.xml;
      const std::string opening = "<Type Name=\"";
      for (size_t at = xml.find(opening); scoped && at != std::string::npos; at = xml.find(opening, at + 1))
        xml.insert(at + opening.size(), "std::");
      out += xml;
    }
    else
      ruleOut.insert(r.classes.begin(), r.classes.end());
  }

  // Rules per instance of a library class, whose casts name its element's type: of the classes the
  // debug information defines, by the names it gives them (CodegenResult::debugClasses, sorted by
  // name), so that a rule and the PDB agree by construction. (natvis matches a type by its
  // unqualified name.)
  TypeTable& types = global.types();
  std::unordered_map<const Ty*, const std::string*> named;
  for (const DebugClass& c : classes)
    named.emplace(c.type, &c.name);
  // A type's name in the PDB: a class's as the debug information gives it, a pointer's its pointee's
  // with `*`, the rest kpp's.
  std::function<std::string(const Ty*)> typeName = [&](const Ty* t)
  {
    if (auto n = named.find(t); n != named.end())
      return *n->second;
    if (t->kind == TyKind::Pointer && t->inner && !t->isConst && !t->isVolatile)
      return typeName(t->inner) + "*";
    return types.toString(t, &symbols, true);
  };
  auto instancesOf = [&](const std::string& cls, size_t arity)
  {
    std::vector<const DebugClass*> found;
    auto lib = library.find(cls);
    if (lib == library.end() || ruleOut.count(cls))
      return found;
    for (const DebugClass& c : classes)
      if (c.type->decl == lib->second.first && c.type->args.size() == arity)
        found.push_back(&c);
    return found;
  };
  // The name of the library's node class `cls` of element `element` (ListNode<T>): empty when the
  // debug information has none, so no node of it either.
  auto nodeName = [&](const std::string& cls, const Ty* element)
  {
    auto lib = library.find(cls);
    if (lib != library.end())
      for (const DebugClass& c : classes)
        if (c.type->decl == lib->second.first && c.type->args.size() == 1 && c.type->args[0] == element)
          return c.name;
    return std::string();
  };

  // A HashTable's entries: in its slots (E) or behind them (E*, stable), where its tag is 0x80 or
  // above (HashIterator::skipFree).
  for (const DebugClass* c : instancesOf("HashTable", 6))
  {
    const Ty* t = c->type;
    std::string entry = typeName(t->args[0]);
    bool stable = t->args[5]->kind == TyKind::Value && t->args[5]->value != 0;
    std::string item = stable ? "*((" + entry + "**)slots)[i]" : "((" + entry + "*)slots)[i]";
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ count={count} }}</DisplayString>\n"
           "    <Expand>\n"
           "      <Item Name=\"[capacity]\" ExcludeView=\"simple\">capacity</Item>\n"
           "      <CustomListItems MaxItemsPerView=\"5000\">\n"
           "        <Variable Name=\"i\" InitialValue=\"0\"/>\n"
           "        <Loop Condition=\"i &lt; capacity\">\n"
           "          <If Condition=\"tags[i] &gt;= 0x80\">\n"
           "            <Item>" + escape(item) + "</Item>\n"
           "          </If>\n"
           "          <Exec>i++</Exec>\n"
           "        </Loop>\n"
           "      </CustomListItems>\n"
           "    </Expand>\n"
           "  </Type>\n";
  }
  // A UniquePtr with its default deleter is named `Item^` (Emitter::debuggerClassName), which the
  // rule for UniquePtr<*> does not match: the same rule by each such name.
  for (const DebugClass* c : instancesOf("UniquePtr", 2))
    if (!c->name.empty() && c->name.back() == '^')
      out += "  <Type Name=\"" + escape(c->name) + "\">\n"
             "    <SmartPointer Usage=\"Minimal\">held</SmartPointer>\n"
             "    <DisplayString Condition=\"held == 0\">empty</DisplayString>\n"
             "    <DisplayString>{*held}</DisplayString>\n"
             "    <Expand>\n"
             "      <ExpandedItem Condition=\"held != 0\">*held</ExpandedItem>\n"
             "    </Expand>\n"
             "  </Type>\n";
  // A List's nodes: a ring through its sentinel, `length` of them after it.
  for (const DebugClass* c : instancesOf("List", 1))
  {
    std::string node = escape(nodeName("ListNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ length={length} }}</DisplayString>\n";
    if (!node.empty())
      out += "    <Expand>\n"
             "      <LinkedListItems>\n"
             "        <Size>length</Size>\n"
             "        <HeadPointer>(" + node + "*)sentinel.next</HeadPointer>\n"
             "        <NextPointer>(" + node + "*)next</NextPointer>\n"
             "        <ValueNode>value</ValueNode>\n"
             "      </LinkedListItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // A ForwardList's nodes: from beforeHead.next to a null next.
  for (const DebugClass* c : instancesOf("ForwardList", 1))
  {
    std::string node = escape(nodeName("ForwardNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString Condition=\"beforeHead.next == 0\">empty</DisplayString>\n";
    if (!node.empty())
      out += "    <DisplayString>{{ front={((" + node + "*)beforeHead.next)-&gt;value} }}</DisplayString>\n"
             "    <Expand>\n"
             "      <LinkedListItems>\n"
             "        <HeadPointer>(" + node + "*)beforeHead.next</HeadPointer>\n"
             "        <NextPointer>(" + node + "*)next</NextPointer>\n"
             "        <ValueNode>value</ValueNode>\n"
             "      </LinkedListItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // An RbTree's (Map's, Set's, MultiMap's, MultiSet's) entries in order: its root is header.parent,
  // a missing child null.
  for (const DebugClass* c : instancesOf("RbTree", 4))
  {
    std::string node = escape(nodeName("RbNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ count={count} }}</DisplayString>\n";
    if (!node.empty())
      out += "    <Expand>\n"
             "      <TreeItems>\n"
             "        <Size>count</Size>\n"
             "        <HeadPointer>(" + node + "*)header.parent</HeadPointer>\n"
             "        <LeftPointer>(" + node + "*)left</LeftPointer>\n"
             "        <RightPointer>(" + node + "*)right</RightPointer>\n"
             "        <ValueNode>entry</ValueNode>\n"
             "      </TreeItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // A typed union (`union enum`, design.md 8.5) as its active alternative: its tag (ClassDecl::tagField,
  // valued in the alternatives' order) and each alternative's storage (ClassDecl::alternatives), by
  // the names the parser gave them, which the PDB's are. One rule per class, and per instance of one
  // in a template.
  {
    std::set<const ClassDecl*> unions;
    std::function<void(Span<Decl*>)> find = [&](Span<Decl*> decls)
    {
      for (const Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
          find(static_cast<const NamespaceDecl*>(d)->decls);
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<const ClassDecl*>(d);
          if (c->isTypedUnion && c->isDefinition && c->tagField)
            unions.insert(c);
          find(c->members);
        }
    };
    for (const ParsedFile* f : files)
      find(f->decls);
    for (const DebugClass& d : classes)
    {
      if (!d.type->decl || d.type->decl->kind != NodeKind::ClassDecl || !unions.count(static_cast<const ClassDecl*>(d.type->decl)))
        continue;
      auto* c = static_cast<const ClassDecl*>(d.type->decl);
      std::string tag = nameOf(static_cast<const VariableDecl*>(c->tagField)->name, symbols);
      std::string shown, items;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
      {
        std::string storage = nameOf(static_cast<const VariableDecl*>(c->alternatives[i])->name, symbols);
        std::string alternative = storage.rfind("__alt_", 0) == 0 ? storage.substr(6) : storage;
        std::string when = "(int)" + tag + " == " + std::to_string(i);
        shown += "    <DisplayString Condition=\"" + when + "\">" + escape(alternative) + " {" + storage + "}</DisplayString>\n";
        items += "      <Item Name=\"" + escape(alternative) + "\" Condition=\"" + when + "\">" + storage + "</Item>\n";
      }
      out += "  <Type Name=\"" + escape(d.name) + "\">\n" + shown +
             "    <Expand>\n"
             "      <Item Name=\"[tag]\">" + tag + "</Item>\n" + items +
             "    </Expand>\n"
             "  </Type>\n";
    }
  }
  out += "</AutoVisualizer>\n";
  return out;
}
