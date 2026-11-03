// The .natvis of a program (natvis.hpp): fixed rules for the library's containers, each checked
// against the members it reads, and one rule per HashTable instance, whose slots are cast to its
// entry type by name. Type names are kppc's (TypeTable::toString, qualified), which the PDB's are
// (docs/debugging.md, "Values in Visual Studio": agreed with the code generator).
#include "natvis.hpp"
#include "resolver.hpp"
#include "types.hpp"
#include <functional>
#include <map>
#include <set>

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

  // A rule for the library's classes: the classes and members it reads, and its XML.
  struct Rule
  {
    std::vector<std::string> classes;
    std::vector<std::string> members;
    std::string xml;
  };

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

std::string makeNatvis(const std::vector<ParsedFile*>& files, GlobalScope& global, const SymbolTable& symbols, std::vector<std::string>& problems)
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
  for (const ParsedFile* f : files)
    if (f->isLibrary)
      collect(f->decls, f);

  std::string largeMark;
  if (auto s = library.find("String"); s == library.end() || !s->second.second->tokens ||
                                       !literalOf(s->second.first, "largeMark", symbols, *s->second.second->tokens, s->second.second->text, largeMark))
    problems.push_back("String's largeMark is no number literal: the String rule is left out");

  // `mark` is a String's last byte: a small string's length, or largeMark for one on the heap.
  const std::string mark = "(unsigned char)smallChars[sizeof(smallChars) - 1]";
  std::vector<Rule> rules = {
    {{"String"}, {"smallChars", "heapChars", "heapLength", "heapCapacity"},
     "  <Type Name=\"String\">\n"
     "    <Intrinsic Name=\"large\" Expression=\"" + mark + " == " + largeMark + "\"/>\n"
     "    <DisplayString Condition=\"large()\">{heapChars,[heapLength]s8}</DisplayString>\n"
     "    <DisplayString>{smallChars,[" + mark + "]s8}</DisplayString>\n"
     "    <StringView Condition=\"large()\">heapChars,[heapLength]s8</StringView>\n"
     "    <StringView>smallChars,[" + mark + "]s8</StringView>\n"
     "    <Expand>\n"
     "      <Item Name=\"[length]\" Condition=\"large()\">heapLength</Item>\n"
     "      <Item Name=\"[length]\" Condition=\"!large()\">" + mark + "</Item>\n"
     "      <Item Name=\"[capacity]\" Condition=\"large()\">heapCapacity</Item>\n"
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
    {{"UniquePtr"}, {"held"},
     "  <Type Name=\"UniquePtr&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"held == 0\">empty</DisplayString>\n"
     "    <DisplayString>{*held}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"held != 0\">*held</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"SharedPtr"}, {"pointer", "control"},
     "  <Type Name=\"SharedPtr&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"pointer == 0\">empty</DisplayString>\n"
     "    <DisplayString>{*pointer}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"pointer != 0\">*pointer</ExpandedItem>\n"
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
  };

  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<!-- Written by kppc for a build with debug information (docs/debugging.md, \"Values in Visual Studio\"). -->\n"
                    "<AutoVisualizer xmlns=\"http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n";
  std::set<std::string> ruleOut; // classes whose rule is left out
  for (const Rule& r : rules)
  {
    bool ok = !(r.classes[0] == "String" && largeMark.empty());
    for (const std::string& cls : r.classes)
    {
      if (specialized.count(cls))
        problems.push_back("the library specializes " + cls + ", whose rule names only its primary template: add a rule for the specialization");
      auto found = library.find(cls);
      if (found == library.end())
      {
        problems.push_back("the library has no class " + cls + ": its rule is left out");
        ok = false;
        continue;
      }
      std::set<std::string> members;
      dataMembers(found->second.first, symbols, members);
      for (const std::string& m : r.members)
        if (!members.count(m))
        {
          problems.push_back(cls + " has no data member " + m + ": its rule is left out");
          ok = false;
        }
    }
    if (ok)
      out += r.xml;
    else
      ruleOut.insert(r.classes.begin(), r.classes.end());
  }

  // A HashTable's entries: in its slots (E) or behind them (E*, stable), where its tag is 0x80 or
  // above (HashIterator::skipFree). One rule per instance: the cast names its entry type.
  if (!ruleOut.count("HashTable"))
  {
    TypeTable& types = global.types();
    std::map<std::string, std::string> byName; // (by name: the instances are made in the threads' order)
    for (const Ty* t : types.classInstances())
    {
      if (!t->decl || t->decl->kind != NodeKind::ClassDecl || nameOf(static_cast<const ClassDecl*>(t->decl)->name, symbols) != "HashTable" ||
          t->args.size() != 6 || t->isConst || t->isVolatile || types.isDependent(t)) // (natvis matches a type by its unqualified name)
        continue;
      auto lib = library.find("HashTable");
      if (lib == library.end() || t->decl != lib->second.first)
        continue;
      std::string name = types.toString(t, &symbols, true);
      if (byName.count(name))
        continue;
      std::string entry = types.toString(t->args[0], &symbols, true);
      bool stable = t->args[5]->kind == TyKind::Value && t->args[5]->value != 0;
      std::string item = stable ? "*((" + entry + "**)slots)[i]" : "((" + entry + "*)slots)[i]";
      byName[name] = "  <Type Name=\"" + escape(name) + "\">\n"
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
    for (const auto& [name, xml] : byName)
      out += xml;
  }
  out += "</AutoVisualizer>\n";
  return out;
}
