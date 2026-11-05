#include "convert.hpp"
#include "parser.hpp"
#include "source.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

// The member functions whose bodies go inside their classes: collectInlineBodies.
struct InlineBodies
{
  std::unordered_map<const Decl*, uint32_t> declarations, definitions;
  std::map<std::string, size_t> kept; // why a definition stays outside, counted
};

namespace
{
  struct Edit
  {
    uint32_t offset;
    uint32_t length; // 0: an insertion
    std::string text;
  };

  struct Counts
  {
    std::map<std::string, int> values;
    void add(const std::string& what, int n = 1) { values[what] += n; }
  };

#include "Converter/PreprocessorConfiguration.inl"
#include "Converter/IfEval.inl"
#include "Converter/Preprocessor.inl"
#include "Converter/NameMap.inl"
#include "Converter/Variants.inl"
#include "Converter/Overrides.inl"
#include "Converter/MacroKinds.inl"
#include "Converter/FileConverter.hpp"

  std::string relativeTo(const std::filesystem::path& p, const std::vector<std::filesystem::path>& roots)
  {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec).lexically_normal();
    for (const auto& root : roots)
    {
      std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
      if (abs == r && r.has_parent_path())
        return (r.parent_path().filename() / r.filename()).generic_string(); // a file given alone keeps its directory (Lua/LuaCPPUtilities.hpp)
      std::filesystem::path rel = abs.lexically_relative(r);
      if (!rel.empty() && rel.native()[0] != '.')
        return (r.filename() / rel).generic_string();
    }
    return abs.filename().generic_string();
  }
}

void cKeywordsAsNames(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols,
                      const std::map<std::string, std::string>* keywordMacros)
{
  // C++'s keywords that C does not have. C's own (and those it has as macros of its headers:
  // bool, true, false, alignas, static_assert, thread_local, wchar_t) stay keywords.
  static const Kw cppOnly[] = {Kw::and_, Kw::and_eq_, Kw::bitand_, Kw::bitor_, Kw::catch_, Kw::char8_t_, Kw::class_, Kw::compl_,
                               Kw::concept_, Kw::consteval_, Kw::constexpr_, Kw::constinit_, Kw::const_cast_, Kw::co_await_,
                               Kw::co_return_, Kw::co_yield_, Kw::decltype_, Kw::delete_, Kw::dynamic_cast_, Kw::explicit_,
                               Kw::export_, Kw::friend_, Kw::mutable_, Kw::namespace_, Kw::new_, Kw::noexcept_, Kw::not_,
                               Kw::not_eq_, Kw::operator_, Kw::or_, Kw::or_eq_, Kw::private_, Kw::protected_, Kw::public_,
                               Kw::reinterpret_cast_, Kw::requires_, Kw::static_cast_, Kw::template_, Kw::this_, Kw::throw_,
                               Kw::try_, Kw::typeid_, Kw::typename_, Kw::using_, Kw::virtual_, Kw::xor_, Kw::xor_eq_,
                               Kw::when_}; // (kpp's: design.md 19)
  for (Token& t : tokens)
    if (t.getKind() == TokenKind::Keyword && std::find(std::begin(cppOnly), std::end(cppOnly), Kw(t.id)) != std::end(cppOnly))
    {
      t.kind = uint32_t(TokenKind::Identifier);
      t.id = symbols.intern(text.substr(t.offset, t.length));
    }
    else if (t.getKind() == TokenKind::Identifier && keywordMacros && !keywordMacros->empty())
      if (auto macro = keywordMacros->find(std::string(text.substr(t.offset, t.length))); macro != keywordMacros->end())
      {
        t.kind = uint32_t(TokenKind::Keyword);
        t.id = lookupKeyword(macro->second);
      }
}

#include "Converter/CMacros.inl"
#include "Converter/SourceConfiguration.inl"
#include "Converter/NamespaceBlocks.inl"
#include "Converter/HeaderMacros.inl"
#include "Converter/ProgramPasses.inl"
#include "Converter/ConvertToKpp.inl"
#include "Converter/ConfigurationMerge.inl"
