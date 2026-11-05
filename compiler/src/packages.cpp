#include "packages.hpp"
#include "Ast/Ast.hpp"
#include "lexer.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string_view>

std::string packageDescription(const std::filesystem::path& dir)
{
  std::ifstream in(dir / "README.md");
  std::string line, paragraph;
  bool fence = false;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t start = line.find_first_not_of(" \t");
    std::string_view text = start == std::string::npos ? std::string_view() : std::string_view(line).substr(start);
    if (text.substr(0, 3) == "```")
    {
      fence = !fence;
      continue;
    }
    if (fence)
      continue;
    if (text.empty())
    {
      if (!paragraph.empty())
        break;
      continue;
    }
    if (paragraph.empty() && (text[0] == '#' || text.substr(0, 2) == "![" || text[0] == '<'))
      continue;
    paragraph += (paragraph.empty() ? "" : " ") + std::string(text);
  }
  return paragraph;
}

void printPackages(const std::vector<PackageOf>& packages, const std::vector<std::filesystem::path>& files,
                   const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols)
{
  struct Package
  {
    std::filesystem::path dir;
    std::vector<size_t> files;
  };
  std::map<std::string, Package> tree; // by path: a package right before its children
  for (size_t i = 0; i < packages.size() && i < parsed.size() && i < files.size(); ++i)
  {
    const PackageOf& of = packages[i];
    std::string path = of.rootName;
    tree[path].dir = of.rootDir;
    std::filesystem::path dir = of.rootDir;
    for (size_t from = 0; from < of.under.size();)
    {
      size_t slash = std::min(of.under.find('/', from), of.under.size());
      std::string part = of.under.substr(from, slash - from);
      path += "/" + part;
      dir /= part;
      tree[path].dir = dir;
      from = slash + 1;
    }
    tree[path].files.push_back(i);
  }
  for (auto& [path, package] : tree)
  {
    std::string description = packageDescription(package.dir);
    printf("%s%s%s\n", path.c_str(), description.empty() ? "" : ": ", description.c_str());
    std::sort(package.files.begin(), package.files.end(), [&](size_t a, size_t b) { return files[a].filename() < files[b].filename(); });
    for (size_t i : package.files)
    {
      std::vector<std::string> types, functions, variables, namespaces;
      auto add = [](std::vector<std::string>& to, std::string_view name)
      {
        if (!name.empty() && std::find(to.begin(), to.end(), name) == to.end())
          to.emplace_back(name);
      };
      // (an out-of-line member's definition, `Order::total`, is its class's, not a name of the file)
      auto spelled = [&](const QualifiedName& name) -> std::string_view
      {
        if (name.parts.size() != 1 || name.global || name.last().symbol == UINT32_MAX)
          return {};
        return symbols.name(name.last().symbol);
      };
      // (a namespace's names qualified by it, `agui::Button`; a namespace that declares none of them
      // is listed as one)
      auto collect = [&](auto& self, Span<Decl*> decls, const std::string& prefix) -> bool
      {
        bool any = false;
        auto named = [&](std::vector<std::string>& to, std::string_view name)
        {
          if (!name.empty())
          {
            add(to, prefix + std::string(name));
            any = true;
          }
        };
        for (const Decl* d : decls)
          switch (d->kind)
          {
          case NodeKind::ClassDecl: named(types, spelled(static_cast<const ClassDecl*>(d)->name)); break;
          case NodeKind::EnumDecl: named(types, spelled(static_cast<const EnumDecl*>(d)->name)); break;
          case NodeKind::UsingAliasDecl: named(types, symbols.name(static_cast<const UsingAliasDecl*>(d)->name)); break;
          case NodeKind::FunctionDecl: named(functions, spelled(static_cast<const FunctionDecl*>(d)->name)); break;
          case NodeKind::VariableDecl: named(variables, spelled(static_cast<const VariableDecl*>(d)->name)); break;
          case NodeKind::NamespaceDecl:
          {
            const auto* n = static_cast<const NamespaceDecl*>(d);
            std::string inner = n->name == UINT32_MAX ? prefix : prefix + std::string(symbols.name(n->name)) + "::";
            if (!self(self, n->decls, inner) && n->name != UINT32_MAX)
              add(namespaces, inner.substr(0, inner.size() - 2)); // (dropped below if another block of it declares names)
            any = true;
            break;
          }
          default: break;
          }
        return any;
      };
      collect(collect, parsed[i]->decls, std::string());
      // (a namespace listed for one block of it that declares nothing, `namespace agui { }` holding only
      // definitions of members, while another block's names are listed: those say it already)
      std::erase_if(namespaces, [&](const std::string& n)
      {
        std::string prefix = n + "::";
        for (const std::vector<std::string>* names : {&types, &functions, &variables})
          for (const std::string& name : *names)
            if (name.compare(0, prefix.size(), prefix) == 0)
              return true;
        return false;
      });
      std::string line = "  " + files[i].filename().string();
      const char* separator = ": ";
      const std::pair<const char*, const std::vector<std::string>*> kinds[] = {
        {"types", &types}, {"functions", &functions}, {"variables", &variables}, {"namespaces", &namespaces}};
      for (const auto& [label, names] : kinds)
        if (!names->empty())
        {
          line += separator + std::string(label);
          for (size_t k = 0; k < names->size(); ++k)
            line += (k ? ", " : " ") + (*names)[k];
          separator = "; ";
        }
      printf("%s\n", line.c_str());
    }
  }
  fflush(stdout);
}
