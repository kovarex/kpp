#include "Resolver/GlobalScopeImpl.hpp"

void ResolveStats::add(const ResolveStats& o)
{
  typesResolved += o.typesResolved;
  typesDependent += o.typesDependent;
  typesUnresolved += o.typesUnresolved;
  typesBuiltinByName += o.typesBuiltinByName;
  namesLocal += o.namesLocal;
  namesMember += o.namesMember;
  namesGlobal += o.namesGlobal;
  namesDependent += o.namesDependent;
  namesUnresolved += o.namesUnresolved;
  qualifiedUnresolvedStd += o.qualifiedUnresolvedStd;
  unresolvedMacroLike += o.unresolvedMacroLike;
  for (const auto& [symbol, count] : o.unresolvedSymbols)
    unresolvedSymbols[symbol] += count;
  auto earliest = [](std::unordered_map<uint32_t, Where>& into, uint32_t symbol, const Where& where)
  {
    auto [it, inserted] = into.emplace(symbol, where);
    if (!inserted && std::make_pair(where.file, where.token) < std::make_pair(it->second.file, it->second.token))
      it->second = where; // the earliest occurrence, whatever order the statistics are added in
  };
  for (const auto& [symbol, where] : o.unresolvedFirst)
    earliest(unresolvedFirst, symbol, where);
  membersBound += o.membersBound;
  membersDependent += o.membersDependent;
  membersUnknownBase += o.membersUnknownBase;
  membersNotFound += o.membersNotFound;
  for (const auto& [symbol, count] : o.missingMembers)
    missingMembers[symbol] += count;
  for (const auto& [symbol, where] : o.missingMembersFirst)
    earliest(missingMembersFirst, symbol, where);
  for (const auto& [symbol, count] : o.unknownBaseMembers)
    unknownBaseMembers[symbol] += count;
  exprsTyped += o.exprsTyped;
  exprsDependent += o.exprsDependent;
  exprsUnknown += o.exprsUnknown;
  unknownName += o.unknownName;
  unknownBase += o.unknownBase;
  unknownMember += o.unknownMember;
  unknownOverload += o.unknownOverload;
  unknownDeduction += o.unknownDeduction;
  unknownAutoReturn += o.unknownAutoReturn;
  unknownUnsupported += o.unknownUnsupported;
  unknownOperator += o.unknownOperator;
  unknownAuto += o.unknownAuto;
  callsResolved += o.callsResolved;
  callsAmbiguousArity += o.callsAmbiguousArity;
  instExprsTyped += o.instExprsTyped;
  instExprsDependent += o.instExprsDependent;
  instExprsUnknown += o.instExprsUnknown;
  for (const auto& [key, site] : o.why)
  {
    auto [it, inserted] = why.emplace(key, site);
    if (!inserted)
    {
      it->second.count += site.count;
      auto order = [](const WhySite& w) { return std::make_pair(w.file ? w.file->index : UINT32_MAX, w.token); };
      if (order(site) < order(it->second))
      {
        it->second.file = site.file; // the earliest place, whatever order the statistics are added in
        it->second.token = site.token;
      }
    }
  }
}

const Ty* stringLiteralArray(const Expr* e, const Ty* t, TypeTable& types)
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<const ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::LiteralExpr || !t || t->kind != TyKind::Pointer || !t->inner)
    return nullptr;
  auto* l = static_cast<const LiteralExpr*>(e);
  const ParsedFile* pf = l->file;
  if (l->literalKind != TokenKind::String || !pf || !pf->tokens || l->token >= pf->tokens->size())
    return nullptr;
  const Ty* element = types.unqualified(t->inner);
  if (element->kind != TyKind::Char || element->bits != 8)
    return nullptr; // u"", U"", L"": not decoded here
  std::string bytes;
  for (size_t i = l->token; i < pf->tokens->size() && (*pf->tokens)[i].getKind() == TokenKind::String; ++i)
  {
    std::string spliced;
    std::string_view spelled = tokenSpelling(pf->text, (*pf->tokens)[i], spliced);
    size_t quote = spelled.rfind('"');
    if (quote != std::string_view::npos && quote + 1 < spelled.size())
      spelled = spelled.substr(0, quote + 1); // (`"..."sv`: the chars the quotes hold)
    if (!decodeNarrowString(spelled, bytes))
      return nullptr;
  }
  return types.array(t->inner, uint32_t(bytes.size() + 1));
}

std::string whyReport(const ResolveStats& total, SymbolTable& symbols, int perCause)
{
  static const char* causes[] = {"none", "name", "object type", "member", "overload", "deduction", "auto return",
                                 "unsupported construct", "operator", "auto"};
  std::vector<std::pair<uint64_t, ResolveStats::WhySite>> sites(total.why.begin(), total.why.end());
  std::sort(sites.begin(), sites.end(), [&](auto& a, auto& b)
  {
    if ((a.first >> 55) != (b.first >> 55))
      return (a.first >> 55) < (b.first >> 55);
    if (a.second.count != b.second.count)
      return a.second.count > b.second.count;
    // Ties by node kind, then by the name's text: symbol ids depend on the threads that lexed.
    if (((a.first >> 32) & 0xFF) != ((b.first >> 32) & 0xFF))
      return ((a.first >> 32) & 0xFF) < ((b.first >> 32) & 0xFF);
    auto name = [&](uint64_t key) { return uint32_t(key) == UINT32_MAX ? std::string_view("-") : symbols.name(uint32_t(key)); };
    return name(a.first) < name(b.first);
  });
  std::string out;
  char line[512];
  for (size_t i = 0; i < sites.size();)
  {
    uint64_t group = sites[i].first >> 55;
    uint64_t cause = group >> 1;
    uint64_t sum = 0;
    size_t end = i;
    while (end < sites.size() && (sites[end].first >> 55) == group)
      sum += sites[end++].second.count;
    snprintf(line, sizeof line, "  unknown because of %s %s (%llu), most common constructs:\n",
             cause < std::size(causes) ? causes[cause] : "?", group & 1 ? "inside template instantiations" : "in plain code",
             (unsigned long long)sum);
    out += line;
    for (size_t k = i; k < end && int(k - i) < perCause; ++k)
    {
      const auto& [key, site] = sites[k];
      auto kind = NodeKind(uint8_t(key >> 32));
      uint32_t symbol = uint32_t(key);
      std::string_view name = symbol == UINT32_MAX ? std::string_view("-") : symbols.name(symbol);
      uint32_t lineNo = 0;
      if (site.file && site.file->tokens && !site.file->tokens->empty())
      {
        const Token& t = (*site.file->tokens)[std::min<size_t>(site.token, site.file->tokens->size() - 1)];
        lineNo = 1 + uint32_t(std::count(site.file->text.begin(), site.file->text.begin() + std::min<size_t>(t.offset, site.file->text.size()), '\n'));
      }
      snprintf(line, sizeof line, "    %6u  %-14s %-32.*s first at %s:%u\n", site.count, nodeKindName(kind), int(std::min<size_t>(name.size(), 60)),
               name.data(), site.file ? site.file->path.c_str() : "?", lineNo);
      out += line;
    }
    i = end;
  }
  return out;
}

// --- Scope tree ---------------------------------------------------------------------------
